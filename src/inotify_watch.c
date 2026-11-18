#include "inotify_watch.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define L_IN_IGNORED     0x00008000u
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
};

struct instance {
  struct instance *next;
  dev_t dev;
  ino_t ino;
  int writer;
  int next_wd;
  struct watch *watches;
};

static struct instance *instances;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static struct instance *instance_for_fd(int fd) {
  struct stat st;
  if (fstat(fd, &st) != 0) return NULL;
  for (struct instance *i = instances; i; i = i->next)
    if (i->dev == st.st_dev && i->ino == st.st_ino) return i;
  return NULL;
}

int inotify_watch_register(int read_fd, int write_fd) {
  struct stat st;
  if (fstat(read_fd, &st) != 0) return -1;
  struct instance *i = calloc(1, sizeof *i);
  if (!i) return -1;
  i->dev = st.st_dev;
  i->ino = st.st_ino;
  i->writer = write_fd;
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
  struct watch *w = calloc(1, sizeof *w);
  if (!w) { pthread_mutex_unlock(&lock); return -ENOMEM; }
  w->path = strdup(host_path);
  if (!w->path) { free(w); pthread_mutex_unlock(&lock); return -ENOMEM; }
  w->wd = i->next_wd++;
  w->mask = mask;
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
  struct inotify_event_wire event = { wd, L_IN_IGNORED, 0, 0 };
  // The writer is nonblocking. Linux can discard notifications when its queue is full;
  // removal itself must still complete.
  (void)write(i->writer, &event, sizeof event);
  free(w->path);
  free(w);
  pthread_mutex_unlock(&lock);
  return 0;
}
