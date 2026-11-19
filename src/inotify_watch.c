#include "inotify_watch.h"

#include <errno.h>
#include <dirent.h>
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
#define L_IN_CREATE      0x00000100u
#define L_IN_DELETE      0x00000200u
#define L_IN_Q_OVERFLOW  0x00004000u
#define L_IN_ISDIR       0x40000000u
#define L_IN_MASK_ADD    0x20000000u
#define L_IN_MASK_CREATE 0x10000000u
#define L_IN_ONLYDIR     0x01000000u

struct inotify_event_wire {
  int32_t wd;
  uint32_t mask, cookie, len;
};

struct name_entry {
  struct name_entry *next;
  char *name;
  int is_dir;
};

struct watch {
  struct watch *next;
  char *path;
  unsigned mask;
  int wd;
  int host_fd;
  int is_dir;
  int snapshot_ready;
  struct name_entry *names;
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
  pthread_t worker;
  int closing;
  int backend_failed;
  pid_t owner_pid;
  int next_wd;
  struct watch *watches;
  int initial_fd;
  int aliases[64];
  int alias_count;
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

static void emit_named_event(struct instance *i, int wd, unsigned mask, const char *name) {
  size_t name_len = strlen(name) + 1;
  size_t padded = (name_len + 15) & ~(size_t)15;
  if (padded > 272) return;
  unsigned char packet[sizeof(struct inotify_event_wire) + 272] = {0};
  struct inotify_event_wire event = { wd, mask, 0, (uint32_t)padded };
  memcpy(packet, &event, sizeof event);
  memcpy(packet + sizeof event, name, name_len);
  (void)write(i->writer, packet, sizeof event + padded);
}

static void free_names(struct name_entry *entries) {
  while (entries) {
    struct name_entry *next = entries->next;
    free(entries->name); free(entries); entries = next;
  }
}

static int has_name(const struct name_entry *entries, const char *name) {
  for (; entries; entries = entries->next)
    if (strcmp(entries->name, name) == 0) return 1;
  return 0;
}

// Snapshot a directory through its resolved host path, without traversing its
// children. A hard cap keeps a noisy directory from monopolising the worker.
static int scan_names(const char *path, struct name_entry **out) {
  DIR *dir = opendir(path);
  if (!dir) return -errno;
  struct name_entry *entries = NULL;
  int count = 0, error = 0;
  struct dirent *de;
  errno = 0;
  while ((de = readdir(dir)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    if (++count > 512) { error = EOVERFLOW; break; }
    struct name_entry *entry = calloc(1, sizeof *entry);
    if (!entry || !(entry->name = strdup(de->d_name))) {
      free(entry); error = ENOMEM; break;
    }
    struct stat st;
    if (fstatat(dirfd(dir), de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0)
      entry->is_dir = S_ISDIR(st.st_mode);
    entry->next = entries; entries = entry;
    errno = 0;
  }
  if (!error && errno) error = errno;
  closedir(dir);
  if (error) { free_names(entries); return -error; }
  *out = entries;
  return 0;
}

static void scan_directory_events(struct instance *i, struct watch *w) {
  struct name_entry *current = NULL;
  if (scan_names(w->path, &current) < 0) {
    emit_event(i, -1, L_IN_Q_OVERFLOW);
    return;
  }
  for (struct name_entry *e = current; e; e = e->next)
    if (!has_name(w->names, e->name) && (w->mask & L_IN_CREATE))
      emit_named_event(i, w->wd, L_IN_CREATE | (e->is_dir ? L_IN_ISDIR : 0), e->name);
  for (struct name_entry *e = w->names; e; e = e->next)
    if (!has_name(current, e->name) && (w->mask & L_IN_DELETE))
      emit_named_event(i, w->wd, L_IN_DELETE | (e->is_dir ? L_IN_ISDIR : 0), e->name);
  free_names(w->names);
  w->names = current;
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
    if (event.filter == EVFILT_USER) return NULL;
    int old_cancel_state;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state);
    pthread_mutex_lock(&lock);
    struct watch **pp = &i->watches;
    while (*pp && (*pp)->wd != (int)(intptr_t)event.udata) pp = &(*pp)->next;
    if (pp && *pp) {
      struct watch *w = *pp;
      if ((event.fflags & NOTE_WRITE) && w->is_dir && w->snapshot_ready)
        scan_directory_events(i, w);
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
        free_names(w->names);
        free(w->path);
        free(w);
      }
    }
    pthread_mutex_unlock(&lock);
    pthread_setcancelstate(old_cancel_state, NULL);
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
  i->initial_fd = read_fd;
  i->aliases[0] = read_fd;
  i->alias_count = 1;
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
      unsigned updated = (mask & L_IN_MASK_ADD) ? w->mask | mask : mask;
      if (w->is_dir && (updated & (L_IN_CREATE | L_IN_DELETE)) && !w->snapshot_ready) {
        int status = scan_names(host_path, &w->names);
        if (status < 0) { pthread_mutex_unlock(&lock); return status; }
        w->snapshot_ready = 1;
      }
      w->mask = updated;
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
  w->is_dir = S_ISDIR(st.st_mode);
  if (w->is_dir && (mask & (L_IN_CREATE | L_IN_DELETE))) {
    int status = scan_names(host_path, &w->names);
    if (status < 0) {
      free(w->path); free(w);
      pthread_mutex_unlock(&lock);
      return status;
    }
    w->snapshot_ready = 1;
  }
  w->host_fd = open(host_path, O_EVTONLY | O_CLOEXEC);
  if (w->host_fd < 0) {
    int error = errno;
    free_names(w->names); free(w->path); free(w);
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
    close(w->host_fd); free_names(w->names); free(w->path); free(w);
    pthread_mutex_unlock(&lock);
    return -error;
  }
  if (!i->worker_started) {
    pthread_t thread;
    EV_SET(&change, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(i->kq, &change, 1, NULL, 0, NULL) < 0) {
      int error = errno;
      EV_SET(&change, w->host_fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
      (void)kevent(i->kq, &change, 1, NULL, 0, NULL);
      if (i->unreserve_fd) i->unreserve_fd(w->host_fd);
      close(w->host_fd); free_names(w->names); free(w->path); free(w);
      pthread_mutex_unlock(&lock);
      return -error;
    }
    if (pthread_create(&thread, NULL, watch_worker, i) != 0) {
      EV_SET(&change, w->host_fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
      (void)kevent(i->kq, &change, 1, NULL, 0, NULL);
      if (i->unreserve_fd) i->unreserve_fd(w->host_fd);
      close(w->host_fd); free_names(w->names); free(w->path); free(w);
      pthread_mutex_unlock(&lock);
      return -EAGAIN;
    }
    i->worker = thread;
    i->worker_started = 1;
  }
  w->next = i->watches;
  i->watches = w;
  pthread_mutex_unlock(&lock);
  return w->wd;
}

// Accept a watch on a path with no host backing store (a shim-synthesized path such as a child
// cgroup dir). The watch gets a real descriptor and never fires: host_fd stays -1, so no kevent is
// registered and the worker can never match it. systemd treats inotify_add_watch ENOENT/EPERM on
// its cgroup dir as fatal enough to spam every unit at boot; returning a valid, inert wd keeps it
// quiet without fabricating any event.
long inotify_watch_add_noop(int fd, const char *logical_path, unsigned mask) {
  pthread_mutex_lock(&lock);
  struct instance *i = instance_for_fd(fd);
  if (!i) {
    int error = fcntl(fd, F_GETFD) < 0 ? EBADF : EINVAL;
    pthread_mutex_unlock(&lock);
    return -error;
  }
  // Same ownership/backend guards as inotify_watch_add.
  if (i->owner_pid != getpid()) { pthread_mutex_unlock(&lock); return -EOPNOTSUPP; }
  if (i->backend_failed) { pthread_mutex_unlock(&lock); return -EIO; }
  int watch_count = 0;
  for (struct watch *w = i->watches; w; w = w->next) watch_count++;
  if (watch_count >= 128) { pthread_mutex_unlock(&lock); return -ENOSPC; }
  struct watch *w = calloc(1, sizeof *w);
  if (!w) { pthread_mutex_unlock(&lock); return -ENOMEM; }
  w->path = strdup(logical_path ? logical_path : "");
  if (!w->path) { free(w); pthread_mutex_unlock(&lock); return -ENOMEM; }
  w->wd = i->next_wd++;
  w->mask = mask;
  w->is_dir = 1;
  w->host_fd = -1;                 // no host vnode to monitor: never fires
  w->snapshot_ready = 0;
  w->names = NULL;
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
  if (w->host_fd >= 0) {
    struct kevent change;
    EV_SET(&change, w->host_fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
    (void)kevent(i->kq, &change, 1, NULL, 0, NULL);
    if (i->unreserve_fd) i->unreserve_fd(w->host_fd);
    close(w->host_fd);
  }
  free_names(w->names);
  free(w->path);
  free(w);
  pthread_mutex_unlock(&lock);
  return 0;
}

static struct instance *instance_for_alias(int fd) {
  for (struct instance *i = instances; i; i = i->next)
    for (int n = 0; n < i->alias_count; n++)
      if (i->aliases[n] == fd) return i;
  return NULL;
}

int inotify_watch_is_fd(int fd) {
  pthread_mutex_lock(&lock);
  int found = instance_for_alias(fd) != NULL;
  pthread_mutex_unlock(&lock);
  return found;
}

int inotify_watch_can_dup(int old_fd, int new_fd) {
  pthread_mutex_lock(&lock);
  struct instance *source = instance_for_alias(old_fd);
  int result = 0;
  if (new_fd >= 0 && instance_for_alias(new_fd)) result = -EOPNOTSUPP;
  else if (source && source->alias_count >= 64) result = -EMFILE;
  pthread_mutex_unlock(&lock);
  return result;
}

void inotify_watch_dup(int old_fd, int new_fd) {
  pthread_mutex_lock(&lock);
  struct instance *source = instance_for_alias(old_fd);
  if (source && source->alias_count < 64)
    source->aliases[source->alias_count++] = new_fd;
  pthread_mutex_unlock(&lock);
}

int inotify_watch_close(int fd) {
  pthread_mutex_lock(&lock);
  struct instance *i = instance_for_alias(fd);
  if (!i) { pthread_mutex_unlock(&lock); return 0; }
  int slot = 0;
  while (i->aliases[slot] != fd) slot++;
  i->aliases[slot] = i->aliases[--i->alias_count];
  int final = i->alias_count == 0;
  int was_initial = fd == i->initial_fd;
  void (*unreserve_fd)(int) = i->unreserve_fd;
  if (final) {
    struct instance **pp = &instances;
    while (*pp && *pp != i) pp = &(*pp)->next;
    if (*pp) *pp = i->next;
    i->closing = 1;
  }
  pthread_mutex_unlock(&lock);

  if (was_initial && unreserve_fd) unreserve_fd(fd);
  close(fd);
  if (!final) return 1;

  if (i->owner_pid == getpid() && i->worker_started) {
    struct kevent wake;
    EV_SET(&wake, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    if (kevent(i->kq, &wake, 1, NULL, 0, NULL) < 0 && !i->backend_failed)
      pthread_cancel(i->worker);
    pthread_join(i->worker, NULL);
  }
  while (i->watches) {
    struct watch *w = i->watches;
    i->watches = w->next;
    if (w->host_fd >= 0) {
      if (i->unreserve_fd) i->unreserve_fd(w->host_fd);
      close(w->host_fd);
    }
    free_names(w->names); free(w->path); free(w);
  }
  if (i->unreserve_fd) {
    i->unreserve_fd(i->writer);
    i->unreserve_fd(i->kq);
  }
  close(i->writer);
  close(i->kq);
  free(i);
  return 1;
}
