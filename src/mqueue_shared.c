#include "mqueue_shared.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MQS_MAGIC = 0x534d5131u, MQS_VERSION = 1,
       MQS_MAX_MESSAGES = 32, MQS_MAX_MSG_SIZE = 8192,
       MQS_MAX_PRIORITY = 32767 };

struct mqs_header {
  uint32_t magic, version, max_messages, message_size;
  uint32_t count, reserved;
  uint64_t next_seq;
};

struct mqs_slot {
  uint32_t used, priority, length, reserved;
  uint64_t seq;
};

static int mqs_name(const char* name, char* out, size_t cap) {
  if (!name || name[0] != '/' || !name[1]) return EINVAL;
  size_t n = 0;
  for (const char* p = name + 1; *p; ++p) {
    if (*p == '/' || ++n > 250) return EINVAL;
  }
  if (n + 3 > cap) return ENAMETOOLONG;
  out[0] = 'q'; out[1] = '-';
  memcpy(out + 2, name + 1, n + 1);
  return 0;
}

static int mqs_dir(const char* dir) {
  if (!dir) { errno = EINVAL; return -1; }
  int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) < 0) { int err = errno; close(fd); errno = err; return -1; }
  if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) != 0) {
    close(fd); errno = EACCES; return -1;
  }
  return fd;
}

static int mqs_namespace_lock(int dirfd) {
  int fd = openat(dirfd, ".lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) < 0) { int err = errno; close(fd); errno = err; return -1; }
  if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) != 0) {
    close(fd); errno = EACCES; return -1;
  }
  if (flock(fd, LOCK_EX) < 0) { int err = errno; close(fd); errno = err; return -1; }
  return fd;
}

static int mqs_read(int fd, void* out, size_t length, off_t off) {
  unsigned char* p = out;
  while (length) {
    ssize_t n = pread(fd, p, length, off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return n < 0 ? errno : EIO;
    p += n; off += n; length -= (size_t)n;
  }
  return 0;
}

static int mqs_write(int fd, const void* in, size_t length, off_t off) {
  const unsigned char* p = in;
  while (length) {
    ssize_t n = pwrite(fd, p, length, off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return n < 0 ? errno : EIO;
    p += n; off += n; length -= (size_t)n;
  }
  return 0;
}

static off_t mqs_slot_off(const struct mqs_header* h, unsigned i) {
  return (off_t)sizeof *h + (off_t)i * (sizeof(struct mqs_slot) + h->message_size);
}

static off_t mqs_total_size(const struct mqs_header* h) {
  return mqs_slot_off(h, h->max_messages);
}

static int mqs_header_read(int fd, struct mqs_header* h) {
  int err = mqs_read(fd, h, sizeof *h, 0);
  if (err) return err;
  struct stat st;
  if (h->magic != MQS_MAGIC || h->version != MQS_VERSION ||
      !h->max_messages || h->max_messages > MQS_MAX_MESSAGES ||
      !h->message_size || h->message_size > MQS_MAX_MSG_SIZE ||
      h->count > h->max_messages || h->reserved) return EINVAL;
  if (fstat(fd, &st) < 0) return errno;
  if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 077) != 0 || st.st_size != mqs_total_size(h)) return EINVAL;
  return 0;
}

int mq_shared_open(const char* dir, const char* name, int create, int exclusive,
                   unsigned max_messages, unsigned message_size, int* out_fd) {
  if (!out_fd) return EFAULT;
  *out_fd = -1;
  char file[256];
  int err = mqs_name(name, file, sizeof file);
  if (err) return err;
  int dirfd = mqs_dir(dir);
  if (dirfd < 0) return errno;
  int lockfd = mqs_namespace_lock(dirfd);
  if (lockfd < 0) { err = errno; close(dirfd); return err; }
  int fd = -1, created = 0;
  if (create) {
    fd = openat(dirfd, file, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) created = 1;
    else if (errno != EEXIST || exclusive) err = errno;
  }
  if (fd < 0 && !err) {
    fd = openat(dirfd, file, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) err = errno;
  }
  if (!err && created) {
    if (!max_messages) max_messages = 10;
    if (!message_size) message_size = 8192;
    if (max_messages > MQS_MAX_MESSAGES || message_size > MQS_MAX_MSG_SIZE)
      err = EINVAL;
    else {
      struct mqs_header h = { .magic = MQS_MAGIC, .version = MQS_VERSION,
                              .max_messages = max_messages,
                              .message_size = message_size, .next_seq = 1 };
      if (ftruncate(fd, mqs_total_size(&h)) < 0) err = errno;
      else err = mqs_write(fd, &h, sizeof h, 0);
    }
    if (err) unlinkat(dirfd, file, 0);
  } else if (!err) {
    struct mqs_header h;
    err = mqs_header_read(fd, &h);
  }
  if (!err) *out_fd = fd;
  else if (fd >= 0) close(fd);
  flock(lockfd, LOCK_UN);
  close(lockfd);
  close(dirfd);
  return err;
}

int mq_shared_unlink(const char* dir, const char* name) {
  char file[256];
  int err = mqs_name(name, file, sizeof file);
  if (err) return err;
  int dirfd = mqs_dir(dir);
  if (dirfd < 0) return errno;
  int lockfd = mqs_namespace_lock(dirfd);
  if (lockfd < 0) { err = errno; close(dirfd); return err; }
  if (unlinkat(dirfd, file, 0) < 0) err = errno;
  flock(lockfd, LOCK_UN);
  close(lockfd);
  close(dirfd);
  return err;
}

static int mqs_lock(int fd, struct mqs_header* h) {
  if (flock(fd, LOCK_EX) < 0) return errno;
  int err = mqs_header_read(fd, h);
  if (err) flock(fd, LOCK_UN);
  return err;
}

int mq_shared_send(int fd, const void* data, size_t length, unsigned priority) {
  if (fd < 0 || (length && !data)) return EFAULT;
  if (priority > MQS_MAX_PRIORITY) return EINVAL;
  struct mqs_header h;
  int err = mqs_lock(fd, &h);
  if (err) return err;
  if (length > h.message_size) err = EMSGSIZE;
  else if (h.count == h.max_messages) err = EAGAIN;
  else {
    int found = 0;
    for (unsigned i = 0; i < h.max_messages; ++i) {
      struct mqs_slot slot;
      off_t off = mqs_slot_off(&h, i);
      err = mqs_read(fd, &slot, sizeof slot, off);
      if (err) break;
      if (!slot.used) {
        found = 1;
        if (length) err = mqs_write(fd, data, length, off + sizeof slot);
        if (!err) {
          slot = (struct mqs_slot){ .used = 1, .priority = priority,
                                     .length = (uint32_t)length, .seq = h.next_seq++ };
          err = mqs_write(fd, &slot, sizeof slot, off);
        }
        if (!err) { ++h.count; err = mqs_write(fd, &h, sizeof h, 0); }
        break;
      }
    }
    if (!err && !found) err = EIO;
  }
  flock(fd, LOCK_UN);
  return err;
}

int mq_shared_receive(int fd, void* data, size_t capacity,
                      size_t* length, unsigned* priority) {
  if (fd < 0 || !length || (capacity && !data)) return EFAULT;
  struct mqs_header h;
  int err = mqs_lock(fd, &h);
  if (err) return err;
  if (capacity < h.message_size) err = EMSGSIZE;
  else if (!h.count) err = EAGAIN;
  else {
    unsigned best = h.max_messages;
    struct mqs_slot chosen = {0};
    for (unsigned i = 0; i < h.max_messages; ++i) {
      struct mqs_slot slot;
      err = mqs_read(fd, &slot, sizeof slot, mqs_slot_off(&h, i));
      if (err) break;
      if (slot.used && (slot.length > h.message_size ||
                        slot.priority > MQS_MAX_PRIORITY || !slot.seq)) {
        err = EIO; break;
      }
      if (slot.used && (best == h.max_messages || slot.priority > chosen.priority ||
                        (slot.priority == chosen.priority && slot.seq < chosen.seq))) {
        best = i; chosen = slot;
      }
    }
    if (!err && best == h.max_messages) err = EIO;
    if (!err) {
      off_t off = mqs_slot_off(&h, best);
      if (chosen.length) err = mqs_read(fd, data, chosen.length, off + sizeof chosen);
      if (!err) {
        unsigned char zeros[256] = {0};
        for (size_t pos = 0; pos < chosen.length && !err;) {
          size_t n = chosen.length - pos < sizeof zeros ? chosen.length - pos : sizeof zeros;
          err = mqs_write(fd, zeros, n, off + sizeof chosen + (off_t)pos);
          pos += n;
        }
        if (!err) {
          struct mqs_slot empty = {0};
          err = mqs_write(fd, &empty, sizeof empty, off);
        }
        if (!err) { --h.count; err = mqs_write(fd, &h, sizeof h, 0); }
        if (!err) {
          *length = chosen.length;
          if (priority) *priority = chosen.priority;
        }
      }
    }
  }
  flock(fd, LOCK_UN);
  return err;
}

int mq_shared_status(int fd, unsigned* count, unsigned* max_messages,
                     unsigned* message_size, int* readable, int* writable) {
  if (fd < 0) return EBADF;
  struct mqs_header h;
  int err = mqs_lock(fd, &h);
  if (err) return err;
  if (count) *count = h.count;
  if (max_messages) *max_messages = h.max_messages;
  if (message_size) *message_size = h.message_size;
  if (readable) *readable = h.count != 0;
  if (writable) *writable = h.count < h.max_messages;
  flock(fd, LOCK_UN);
  return 0;
}
