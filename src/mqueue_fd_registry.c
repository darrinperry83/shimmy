#include "mqueue_fd_registry.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

enum { MQFD_MAX = 256 };

struct mqfd_open_file {
  struct mq_core_handle* handle;
  unsigned refs;
  int access_mode, nonblock;
};

struct mqfd_entry {
  int used, fd, cloexec;
  struct mqfd_open_file* open_file;
};

static pthread_mutex_t g_mqfd_mu = PTHREAD_MUTEX_INITIALIZER;
static struct mqfd_entry g_mqfd[MQFD_MAX];

static struct mqfd_entry* mqfd_find_locked(int fd) {
  for (unsigned i = 0; i < MQFD_MAX; ++i)
    if (g_mqfd[i].used && g_mqfd[i].fd == fd) return &g_mqfd[i];
  return NULL;
}

static struct mqfd_entry* mqfd_free_locked(void) {
  for (unsigned i = 0; i < MQFD_MAX; ++i)
    if (!g_mqfd[i].used) return &g_mqfd[i];
  return NULL;
}

static void mqfd_release_locked(struct mqfd_entry* entry) {
  struct mqfd_open_file* open_file = entry->open_file;
  *entry = (struct mqfd_entry){0};
  if (--open_file->refs == 0) {
    mq_core_close(open_file->handle);
    free(open_file);
  }
}

int mqfd_bind(int fd, struct mq_core_handle* handle, int access_mode,
              int nonblock, int cloexec) {
  if (fd < 0 || !handle || access_mode < 0 || access_mode > 2) return EINVAL;
  struct mqfd_open_file* open_file = malloc(sizeof *open_file);
  if (!open_file) return ENOMEM;
  *open_file = (struct mqfd_open_file){ .handle = handle, .refs = 1,
                                      .access_mode = access_mode,
                                      .nonblock = !!nonblock };
  pthread_mutex_lock(&g_mqfd_mu);
  struct mqfd_entry* entry = mqfd_free_locked();
  int err = mqfd_find_locked(fd) ? EEXIST : (!entry ? ENOSPC : 0);
  if (!err) *entry = (struct mqfd_entry){ .used = 1, .fd = fd,
                                         .cloexec = !!cloexec,
                                         .open_file = open_file };
  pthread_mutex_unlock(&g_mqfd_mu);
  if (err) free(open_file); // Ownership stays with caller on failure.
  return err;
}

int mqfd_duplicate(int oldfd, int newfd, int cloexec) {
  if (oldfd < 0 || newfd < 0) return EBADF;
  pthread_mutex_lock(&g_mqfd_mu);
  struct mqfd_entry* old = mqfd_find_locked(oldfd);
  if (!old) { pthread_mutex_unlock(&g_mqfd_mu); return EBADF; }
  if (oldfd == newfd) { pthread_mutex_unlock(&g_mqfd_mu); return 0; }
  struct mqfd_entry* target = mqfd_find_locked(newfd);
  if (!target) target = mqfd_free_locked();
  if (!target) { pthread_mutex_unlock(&g_mqfd_mu); return ENOSPC; }
  // A replacement target can share the same open-file description. Increment
  // first so releasing that target cannot destroy the source description.
  struct mqfd_open_file* shared = old->open_file;
  ++shared->refs;
  if (target->used) mqfd_release_locked(target);
  *target = (struct mqfd_entry){ .used = 1, .fd = newfd,
                                 .cloexec = !!cloexec, .open_file = shared };
  pthread_mutex_unlock(&g_mqfd_mu);
  return 0;
}

int mqfd_detach(int fd) {
  pthread_mutex_lock(&g_mqfd_mu);
  struct mqfd_entry* entry = mqfd_find_locked(fd);
  if (!entry) { pthread_mutex_unlock(&g_mqfd_mu); return EBADF; }
  mqfd_release_locked(entry);
  pthread_mutex_unlock(&g_mqfd_mu);
  return 0;
}

int mqfd_acquire(int fd, struct mq_core_handle** handle,
                 int* access_mode, int* nonblock, int* cloexec) {
  if (!handle) return EFAULT;
  *handle = NULL;
  pthread_mutex_lock(&g_mqfd_mu);
  struct mqfd_entry* entry = mqfd_find_locked(fd);
  if (!entry) { pthread_mutex_unlock(&g_mqfd_mu); return EBADF; }
  int err = mq_core_retain(entry->open_file->handle, handle);
  if (!err) {
    if (access_mode) *access_mode = entry->open_file->access_mode;
    if (nonblock) *nonblock = entry->open_file->nonblock;
    if (cloexec) *cloexec = entry->cloexec;
  }
  pthread_mutex_unlock(&g_mqfd_mu);
  return err;
}

int mqfd_set_nonblock(int fd, int nonblock) {
  pthread_mutex_lock(&g_mqfd_mu);
  struct mqfd_entry* entry = mqfd_find_locked(fd);
  if (entry) entry->open_file->nonblock = !!nonblock;
  pthread_mutex_unlock(&g_mqfd_mu);
  return entry ? 0 : EBADF;
}

int mqfd_set_cloexec(int fd, int cloexec) {
  pthread_mutex_lock(&g_mqfd_mu);
  struct mqfd_entry* entry = mqfd_find_locked(fd);
  if (entry) entry->cloexec = !!cloexec;
  pthread_mutex_unlock(&g_mqfd_mu);
  return entry ? 0 : EBADF;
}
