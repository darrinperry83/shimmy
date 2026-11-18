#include "virtual_pipe_core.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define VP_MAGIC 0x56504331u
struct vp_disk {
  uint32_t magic, read_at, write_at, count;
  unsigned char bytes[VP_CAPACITY];
};
struct vp_core { char *path; dev_t device; ino_t inode; };

static int secure_fd(int fd, struct stat *st) {
  return fstat(fd, st) == 0 && S_ISREG(st->st_mode) &&
         st->st_uid == geteuid() && st->st_nlink == 1 &&
         (st->st_mode & 077) == 0 && st->st_size == sizeof(struct vp_disk);
}
static int valid(const struct vp_disk *p) {
  return p->magic == VP_MAGIC && p->read_at < VP_CAPACITY &&
         p->write_at < VP_CAPACITY && p->count <= VP_CAPACITY &&
         (p->read_at + p->count) % VP_CAPACITY == p->write_at;
}
static int read_disk(int fd, struct vp_disk *p) {
  return pread(fd, p, sizeof *p, 0) == sizeof *p && valid(p) ? 0 : -EIO;
}
static int write_disk(int fd, const struct vp_disk *p) {
  return pwrite(fd, p, sizeof *p, 0) == sizeof *p && fsync(fd) == 0 ? 0 : -EIO;
}
/* Each operation opens its own file description, including after fork and
 * duplicate opens. flock on an inherited fd would not serialize them. */
static int lock_pipe(vp_core *p, int exclusive) {
  if (!p) return -EINVAL;
  int fd = open(p->path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return -errno;
  struct stat st;
  if (!secure_fd(fd, &st) || st.st_dev != p->device || st.st_ino != p->inode) {
    close(fd); return -EACCES;
  }
  if (flock(fd, exclusive ? LOCK_EX : LOCK_SH)) {
    int e = errno; close(fd); return -e;
  }
  return fd;
}
static void unlock_pipe(int fd) { flock(fd, LOCK_UN); close(fd); }

vp_core *vp_core_open(const char *path) {
  if (!path || path[0] != '/') { errno = EINVAL; return NULL; }
  int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return NULL;
  struct stat st;
  if (!secure_fd(fd, &st)) { close(fd); errno = EACCES; return NULL; }
  struct vp_disk data;
  if (flock(fd, LOCK_SH) || read_disk(fd, &data)) {
    close(fd); errno = EIO; return NULL;
  }
  flock(fd, LOCK_UN); close(fd);
  vp_core *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  p->path = strdup(path);
  if (!p->path) { free(p); return NULL; }
  p->device = st.st_dev; p->inode = st.st_ino;
  return p;
}
vp_core *vp_core_create(const char *dir, char *path, size_t capacity) {
  if (!dir || dir[0] != '/' || !path || capacity < strlen(dir) + 24) {
    errno = EINVAL; return NULL;
  }
  int d = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (d < 0) return NULL;
  struct stat st;
  if (fstat(d, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 077)) { close(d); errno = EACCES; return NULL; }
  close(d);
  snprintf(path, capacity, "%s/.shimmy-vpipe-XXXXXX", dir);
  int fd = mkstemp(path);
  if (fd < 0) return NULL;
  struct vp_disk data = {.magic = VP_MAGIC};
  int ok = fchmod(fd, 0600) == 0 && write_disk(fd, &data) == 0;
  close(fd);
  if (!ok) { unlink(path); errno = EIO; return NULL; }
  vp_core *p = vp_core_open(path);
  if (!p) unlink(path);
  return p;
}
void vp_core_close(vp_core *p) { if (p) { free(p->path); free(p); } }

static long transfer(vp_core *p, void *buffer, size_t length, int writing) {
  if (!p || (length && !buffer)) return -EINVAL;
  if (!length) return 0;
  int fd = lock_pipe(p, 1);
  if (fd < 0) return fd;
  struct vp_disk data;
  if (read_disk(fd, &data)) { unlock_pipe(fd); return -EIO; }
  size_t available = writing ? VP_CAPACITY - data.count : data.count;
  if (!available) { unlock_pipe(fd); return -EAGAIN; }
  if (length > available) length = available;
  for (size_t i = 0; i < length; i++) {
    if (writing) {
      data.bytes[data.write_at] = ((const unsigned char *)buffer)[i];
      data.write_at = (data.write_at + 1) % VP_CAPACITY;
    } else {
      ((unsigned char *)buffer)[i] = data.bytes[data.read_at];
      data.bytes[data.read_at] = 0;
      data.read_at = (data.read_at + 1) % VP_CAPACITY;
    }
  }
  data.count += writing ? (uint32_t)length : -(uint32_t)length;
  long result = write_disk(fd, &data) ? -EIO : (long)length;
  unlock_pipe(fd);
  return result;
}
long vp_core_write(vp_core *p, const void *bytes, size_t length) {
  return transfer(p, (void *)bytes, length, 1);
}
long vp_core_read(vp_core *p, void *bytes, size_t length) {
  return transfer(p, bytes, length, 0);
}
long vp_core_tee(vp_core *source, vp_core *destination, size_t length) {
  if (!source || !destination) return -EINVAL;
  if (source->device == destination->device && source->inode == destination->inode) return -EINVAL;
  if (!length) return 0;
  /* Stable inode order prevents simultaneous A→B and B→A deadlock. */
  vp_core *first = source, *second = destination;
  if (first->device > second->device ||
      (first->device == second->device && first->inode > second->inode)) {
    first = destination; second = source;
  }
  int a = lock_pipe(first, 1);
  if (a < 0) return a;
  int b = lock_pipe(second, 1);
  if (b < 0) { unlock_pipe(a); return b; }
  struct vp_disk in, out;
  int infd = source == first ? a : b, outfd = destination == first ? a : b;
  if (read_disk(infd, &in) || read_disk(outfd, &out)) {
    unlock_pipe(b); unlock_pipe(a); return -EIO;
  }
  size_t n = length;
  if (n > in.count) n = in.count;
  if (n > VP_CAPACITY - out.count) n = VP_CAPACITY - out.count;
  if (!n) { unlock_pipe(b); unlock_pipe(a); return -EAGAIN; }
  for (size_t i = 0; i < n; i++) {
    out.bytes[out.write_at] = in.bytes[(in.read_at + i) % VP_CAPACITY];
    out.write_at = (out.write_at + 1) % VP_CAPACITY;
  }
  out.count += (uint32_t)n;
  long result = write_disk(outfd, &out) ? -EIO : (long)n;
  unlock_pipe(b); unlock_pipe(a);
  return result;
}
static long state(vp_core *p, int space) {
  int fd = lock_pipe(p, 0);
  if (fd < 0) return fd;
  struct vp_disk data;
  long result = read_disk(fd, &data) ? -EIO :
                (long)(space ? VP_CAPACITY - data.count : data.count);
  unlock_pipe(fd);
  return result;
}
long vp_core_pending(vp_core *p) { return state(p, 0); }
long vp_core_space(vp_core *p) { return state(p, 1); }
