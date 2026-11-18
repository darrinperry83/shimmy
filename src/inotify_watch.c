#include "inotify_watch.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <unistd.h>

#define L_IN_IGNORED     0x00008000u
#define L_IN_MODIFY      0x00000002u
#define L_IN_ATTRIB      0x00000004u
#define L_IN_DELETE_SELF 0x00000400u
#define L_IN_MOVE_SELF   0x00000800u
#define L_IN_MASK_ADD    0x20000000u
#define L_IN_MASK_CREATE 0x10000000u
#define L_IN_ONLYDIR     0x01000000u

struct inotify_event_wire {
  int32_t wd;
  uint32_t mask, cookie, len;
};

struct watch {
  struct watch *next;
  char *path;
  unsigned mask;
  int wd;
  int host_fd;
};

struct instance {
  struct instance *next;
  dev_t dev;
  ino_t ino;
  int writer;
  int kq;
  int (*reserve_fd)(int);
  void (*unreserve_fd)(int);
  int worker_started;
  int backend_failed;
  pid_t owner_pid;
  int next_wd;
  struct watch *watches;
};

static struct instance *instances;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t fork_setup = PTHREAD_ONCE_INIT;

static void before_fork(void) { pthread_mutex_lock(&lock); }
static void after_fork(void) { pthread_mutex_unlock(&lock); }
static void register_fork_handlers(void) {
  (void)pthread_atfork(before_fork, after_fork, after_fork);
}

static void emit_event(struct instance *i, int wd, unsigned mask) {
  struct inotify_event_wire event = { wd, mask, 0, 0 };
  // A notification is one atomic pipe write. A full queue drops the event;
  // never block the host watcher or a guest filesystem syscall.
  (void)write(i->writer, &event, sizeof event);
}

static void *watch_worker(void *arg) {
  struct instance *i = arg;
  for (;;) {
    struct kevent event;
    int n = kevent(i->kq, NULL, 0, &event, 1, NULL);
    if (n < 0) {
      if (errno == EINTR) continue;
      // A dead kqueue must not turn this background worker into a CPU spin.
      pthread_mutex_lock(&lock);
      i->backend_failed = 1;
      pthread_mutex_unlock(&lock);
      return NULL;
    }
    if (n != 1) continue;
    pthread_mutex_lock(&lock);
    struct watch **pp = &i->watches;
    while (*pp && (*pp)->wd != (int)(intptr_t)event.udata) pp = &(*pp)->next;
    if (pp && *pp) {
      struct watch *w = *pp;
      if ((event.fflags & NOTE_WRITE) && (w->mask & L_IN_MODIFY))
        emit_event(i, w->wd, L_IN_MODIFY);
      if ((event.fflags & NOTE_ATTRIB) && (w->mask & L_IN_ATTRIB))
        emit_event(i, w->wd, L_IN_ATTRIB);
      if ((event.fflags & NOTE_RENAME) && (w->mask & L_IN_MOVE_SELF))
        emit_event(i, w->wd, L_IN_MOVE_SELF);
      if (event.fflags & NOTE_DELETE) {
        if (w->mask & L_IN_DELETE_SELF) emit_event(i, w->wd, L_IN_DELETE_SELF);
        emit_event(i, w->wd, L_IN_IGNORED);
        *pp = w->next;
        if (i->unreserve_fd) i->unreserve_fd(w->host_fd);
        close(w->host_fd);
        free(w->path);
        free(w);
      }
    }
    pthread_mutex_unlock(&lock);
  }
  return NULL;
}

static struct instance *instance_for_fd(int fd) {
  struct stat st;
  if (fstat(fd, &st) != 0) return NULL;
  for (struct instance *i = instances; i; i = i->next)
    if (i->dev == st.st_dev && i->ino == st.st_ino) return i;
  return NULL;
}

int inotify_watch_register(int read_fd, int write_fd, int kq_fd,
                           int (*reserve_fd)(int), void (*unreserve_fd)(int)) {
  pthread_once(&fork_setup, register_fork_handlers);
  struct stat st;
  if (fstat(read_fd, &st) != 0) return -1;
  struct instance *i = calloc(1, sizeof *i);
  if (!i) return -1;
  i->dev = st.st_dev;
  i->ino = st.st_ino;
  i->writer = write_fd;
  // A guest can close every read alias while a vnode event is in flight.
  // Suppress process-fatal SIGPIPE on this private notification pipe.
  if (fcntl(write_fd, F_SETNOSIGPIPE, 1) != 0) { free(i); return -1; }
  i->kq = kq_fd;
  i->owner_pid = getpid();
  i->reserve_fd = reserve_fd;
  i->unreserve_fd = unreserve_fd;
  i->next_wd = 1;
  pthread_mutex_lock(&lock);
  i->next = instances;
  instances = i;
  pthread_mutex_unlock(&lock);
  return 0;
}

long inotify_watch_add(int fd, const char *host_path, unsigned mask) {
  pthread_mutex_lock(&lock);
  struct instance *i = instance_for_fd(fd);
  if (!i) {
    int error = fcntl(fd, F_GETFD) < 0 ? EBADF : EINVAL;
    pthread_mutex_unlock(&lock);
    return -error;
  }
  // The worker thread does not survive fork. Refuse to add an inert child
  // watch rather than claiming it will deliver notifications.
  if (i->owner_pid != getpid()) { pthread_mutex_unlock(&lock); return -EOPNOTSUPP; }
  if (i->backend_failed) { pthread_mutex_unlock(&lock); return -EIO; }
  if (!host_path || !*host_path) { pthread_mutex_unlock(&lock); return -ENOENT; }
  struct stat st;
  if (lstat(host_path, &st) != 0) {
    int error = errno;
    pthread_mutex_unlock(&lock);
    return -error;
  }
  if ((mask & L_IN_ONLYDIR) && !S_ISDIR(st.st_mode)) {
    pthread_mutex_unlock(&lock);
    return -ENOTDIR;
  }
  for (struct watch *w = i->watches; w; w = w->next) {
    if (strcmp(w->path, host_path) == 0) {
      if (mask & L_IN_MASK_CREATE) { pthread_mutex_unlock(&lock); return -EEXIST; }
      w->mask = (mask & L_IN_MASK_ADD) ? w->mask | mask : mask;
      pthread_mutex_unlock(&lock);
      return w->wd;
    }
  }
  int watch_count = 0;
  for (struct watch *w = i->watches; w; w = w->next) watch_count++;
  if (watch_count >= 128) { pthread_mutex_unlock(&lock); return -ENOSPC; }
  struct watch *w = calloc(1, sizeof *w);
  if (!w) { pthread_mutex_unlock(&lock); return -ENOMEM; }
  w->path = strdup(host_path);
  if (!w->path) { free(w); pthread_mutex_unlock(&lock); return -ENOMEM; }
  w->wd = i->next_wd++;
  w->mask = mask;
  w->host_fd = open(host_path, O_EVTONLY | O_CLOEXEC);
  if (w->host_fd < 0) {
    int error = errno;
    free(w->path); free(w);
    pthread_mutex_unlock(&lock);
    return -error;
  }
  if (i->reserve_fd) w->host_fd = i->reserve_fd(w->host_fd);
  struct kevent change;
  EV_SET(&change, w->host_fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
         NOTE_WRITE | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME, 0,
         (void *)(intptr_t)w->wd);
  if (kevent(i->kq, &change, 1, NULL, 0, NULL) < 0) {
    int error = errno;
    if (i->unreserve_fd) i->unreserve_fd(w->host_fd);
    close(w->host_fd); free(w->path); free(w);
    pthread_mutex_unlock(&lock);
    return -error;
  }
  if (!i->worker_started) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, watch_worker, i) != 0) {
      EV_SET(&change, w->host_fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
      (void)kevent(i->kq, &change, 1, NULL, 0, NULL);
      if (i->unreserve_fd) i->unreserve_fd(w->host_fd);
      close(w->host_fd); free(w->path); free(w);
      pthread_mutex_unlock(&lock);
      return -EAGAIN;
    }
    pthread_detach(thread);
    i->worker_started = 1;
  }
  w->next = i->watches;
  i->watches = w;
  pthread_mutex_unlock(&lock);
  return w->wd;
}

long inotify_watch_remove(int fd, int wd) {
  pthread_mutex_lock(&lock);
  struct instance *i = instance_for_fd(fd);
  if (!i) {
    int error = fcntl(fd, F_GETFD) < 0 ? EBADF : EINVAL;
    pthread_mutex_unlock(&lock);
    return -error;
  }
  struct watch **pp = &i->watches;
  while (*pp && (*pp)->wd != wd) pp = &(*pp)->next;
  if (!*pp) { pthread_mutex_unlock(&lock); return -EINVAL; }
  struct watch *w = *pp;
  *pp = w->next;
  // The writer is nonblocking. Linux can discard notifications when its queue is full;
  // removal itself must still complete.
  emit_event(i, wd, L_IN_IGNORED);
  struct kevent change;
  EV_SET(&change, w->host_fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
  (void)kevent(i->kq, &change, 1, NULL, 0, NULL);
  if (i->unreserve_fd) i->unreserve_fd(w->host_fd);
  close(w->host_fd);
  free(w->path);
  free(w);
  pthread_mutex_unlock(&lock);
  return 0;
}
