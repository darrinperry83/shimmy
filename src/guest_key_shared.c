#include "guest_key_shared.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define GKS_MAGIC 0x52444b53u
#define GKS_VERSION 1u
#define GKS_FILE ".shimmy-guest-keys"
struct gks_header { uint32_t magic, version, next_serial, reserved; };
struct gks_record {
  uint32_t serial, owner, length, occupied, revoked;
  char description[GKS_MAX_DESCRIPTION + 1];
  unsigned char payload[GKS_MAX_PAYLOAD];
};
struct gks_namespace { int directory; };

static off_t record_offset(size_t index) {
  return (off_t)sizeof(struct gks_header) + (off_t)index * sizeof(struct gks_record);
}
static int full_read(int fd, void *out, size_t n, off_t offset) {
  unsigned char *p = out;
  while (n) {
    ssize_t got = pread(fd, p, n, offset);
    if (got <= 0) return -1;
    p += got; n -= (size_t)got; offset += got;
  }
  return 0;
}
static int full_write(int fd, const void *in, size_t n, off_t offset) {
  const unsigned char *p = in;
  while (n) {
    ssize_t put = pwrite(fd, p, n, offset);
    if (put <= 0) return -1;
    p += put; n -= (size_t)put; offset += put;
  }
  return 0;
}
static int valid_record(const struct gks_record *r) {
  return r->occupied <= 1 && r->revoked <= 1 &&
         r->length <= GKS_MAX_PAYLOAD &&
         memchr(r->description, 0, sizeof r->description) != NULL;
}
/* Open a fresh file description for each operation: inherited descriptors
 * after host fork must not share the same flock ownership. */
static int locked_file(gks_namespace *ns, int exclusive) {
  if (!ns) return -EINVAL;
  int fd = openat(ns->directory, GKS_FILE, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return -errno;
  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      st.st_nlink != 1 || (st.st_mode & 077)) { close(fd); return -EACCES; }
  if (flock(fd, exclusive ? LOCK_EX : LOCK_SH)) { int e = errno; close(fd); return -e; }
  struct gks_header h;
  if (st.st_size != record_offset(GKS_MAX_KEYS) ||
      full_read(fd, &h, sizeof h, 0) || h.magic != GKS_MAGIC ||
      h.version != GKS_VERSION || h.next_serial == 0 || h.next_serial > INT32_MAX) {
    flock(fd, LOCK_UN); close(fd); return -EIO;
  }
  return fd;
}
static void unlock_file(int fd) { flock(fd, LOCK_UN); close(fd); }

gks_namespace *gks_open(const char *group) {
  if (!group || group[0] != '/') { errno = EINVAL; return NULL; }
  int d = open(group, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (d < 0) return NULL;
  struct stat st;
  if (fstat(d, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 077)) { close(d); errno = EACCES; return NULL; }
  int fd = openat(d, GKS_FILE, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) { close(d); return NULL; }
  if (flock(fd, LOCK_EX)) { int e = errno; close(fd); close(d); errno = e; return NULL; }
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      st.st_nlink != 1 || (st.st_mode & 077)) {
    flock(fd, LOCK_UN); close(fd); close(d); errno = EACCES; return NULL;
  }
  if (st.st_size == 0) {
    struct gks_header h = { GKS_MAGIC, GKS_VERSION, 1, 0 };
    if (ftruncate(fd, record_offset(GKS_MAX_KEYS)) ||
        full_write(fd, &h, sizeof h, 0) || fsync(fd)) {
      int e = errno; flock(fd, LOCK_UN); close(fd); close(d); errno = e; return NULL;
    }
  }
  flock(fd, LOCK_UN); close(fd);
  gks_namespace *ns = malloc(sizeof *ns);
  if (!ns) { close(d); return NULL; }
  ns->directory = d;
  int check = locked_file(ns, 0);
  if (check < 0) { close(d); free(ns); errno = -check; return NULL; }
  unlock_file(check);
  return ns;
}
void gks_close(gks_namespace *ns) { if (ns) { close(ns->directory); free(ns); } }

long gks_insert_user(gks_namespace *ns, uint32_t owner, const char *description,
                     const void *payload, size_t length) {
  if (!description || (length && !payload)) return -EINVAL;
  size_t dlen = strnlen(description, GKS_MAX_DESCRIPTION + 1);
  if (!dlen || dlen > GKS_MAX_DESCRIPTION || length > GKS_MAX_PAYLOAD) return -EINVAL;
  int fd = locked_file(ns, 1);
  if (fd < 0) return fd;
  struct gks_header h;
  if (full_read(fd, &h, sizeof h, 0)) { unlock_file(fd); return -EIO; }
  long result = -ENOSPC;
  for (size_t i = 0; i < GKS_MAX_KEYS; i++) {
    struct gks_record r;
    if (full_read(fd, &r, sizeof r, record_offset(i)) || !valid_record(&r)) { result = -EIO; break; }
    if (r.occupied) continue;
    if (h.next_serial == INT32_MAX) break;
    uint32_t serial = h.next_serial++;
    if (full_write(fd, &h, sizeof h, 0) || fsync(fd)) { result = -EIO; break; }
    memset(&r, 0, sizeof r);
    r.serial = serial; r.owner = owner; r.length = (uint32_t)length; r.occupied = 1;
    memcpy(r.description, description, dlen + 1);
    if (length) memcpy(r.payload, payload, length);
    result = full_write(fd, &r, sizeof r, record_offset(i)) || fsync(fd) ? -EIO : (long)serial;
    break;
  }
  unlock_file(fd);
  return result;
}

long gks_find_user(gks_namespace *ns, uint32_t caller, const char *description) {
  if (!description) return -EINVAL;
  size_t dlen = strnlen(description, GKS_MAX_DESCRIPTION + 1);
  if (!dlen || dlen > GKS_MAX_DESCRIPTION) return -EINVAL;
  int fd = locked_file(ns, 0);
  if (fd < 0) return fd;
  long result = -ENOENT;
  for (size_t i = 0; i < GKS_MAX_KEYS; i++) {
    struct gks_record r;
    if (full_read(fd, &r, sizeof r, record_offset(i)) || !valid_record(&r)) { result = -EIO; break; }
    if (r.occupied && !r.revoked && r.owner == caller && !strcmp(r.description, description)) {
      result = r.serial; break;
    }
  }
  unlock_file(fd);
  return result;
}

static long use_serial(gks_namespace *ns, uint32_t caller, int32_t serial,
                       void *buffer, size_t capacity, int revoke) {
  if (serial <= 0 || (!revoke && capacity && !buffer)) return -EINVAL;
  int fd = locked_file(ns, revoke);
  if (fd < 0) return fd;
  long result = -ENOENT;
  for (size_t i = 0; i < GKS_MAX_KEYS; i++) {
    struct gks_record r;
    if (full_read(fd, &r, sizeof r, record_offset(i)) || !valid_record(&r)) { result = -EIO; break; }
    if (!r.occupied || r.serial != (uint32_t)serial) continue;
    if (r.owner != caller) result = -EACCES;
    else if (r.revoked) result = -ENOENT;
    else if (revoke) {
      memset(r.payload, 0, sizeof r.payload); r.length = 0; r.revoked = 1;
      result = full_write(fd, &r, sizeof r, record_offset(i)) || fsync(fd) ? -EIO : 0;
    } else if (capacity < r.length) result = -ERANGE;
    else { if (r.length) memcpy(buffer, r.payload, r.length); result = r.length; }
    break;
  }
  unlock_file(fd);
  return result;
}
long gks_read(gks_namespace *ns, uint32_t caller, int32_t serial, void *buffer, size_t capacity) {
  return use_serial(ns, caller, serial, buffer, capacity, 0);
}
long gks_revoke(gks_namespace *ns, uint32_t caller, int32_t serial) {
  return use_serial(ns, caller, serial, NULL, 0, 1);
}
