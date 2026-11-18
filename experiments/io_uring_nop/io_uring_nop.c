#include "io_uring_nop.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Prototype of a bounded NOP engine. No syscall dispatch calls these functions.
 * The hooks below model some descriptor operations, but the regular host fd
 * still has the wrong native poll/read/write semantics for Linux io_uring. */
struct iu_ring {
  int anchor_fd;
  int aliases[32];
  unsigned alias_count;
  void *sq, *cq, *sqes;
  size_t sq_len, cq_len, sqes_len;
  uint32_t entries, cq_entries;
};
static pthread_mutex_t iu_lock = PTHREAD_MUTEX_INITIALIZER;
static struct iu_ring iu_rings[8];
static pthread_once_t iu_fork_once = PTHREAD_ONCE_INIT;

static void iu_before_fork(void) { pthread_mutex_lock(&iu_lock); }
static void iu_parent_fork(void) { pthread_mutex_unlock(&iu_lock); }
static void iu_child_fork(void) { pthread_mutex_unlock(&iu_lock); }
static void iu_install_atfork(void) {
  pthread_atfork(iu_before_fork, iu_parent_fork, iu_child_fork);
}

static struct iu_ring *iu_find(int fd) {
  for (size_t i = 0; i < sizeof iu_rings / sizeof iu_rings[0]; i++) {
    struct iu_ring *r = &iu_rings[i];
    for (unsigned j = 0; j < r->alias_count; j++)
      if (r->aliases[j] == fd) {
        struct stat a, b;
        if (fstat(fd, &a) == 0 && fstat(r->anchor_fd, &b) == 0 &&
            a.st_dev == b.st_dev && a.st_ino == b.st_ino) return r;
      }
  }
  return NULL;
}

static void iu_release(struct iu_ring *r) {
  munmap(r->sq, r->sq_len); munmap(r->cq, r->cq_len);
  munmap(r->sqes, r->sqes_len);
  close(r->anchor_fd);
  memset(r, 0, sizeof *r);
}

static uint32_t *word(void *base, size_t off) { return (uint32_t *)((char *)base + off); }

int iu_nop_setup(uint32_t entries, struct iu_params *params) {
  if (!params || !entries || entries > IU_MAX_ENTRIES || (entries & (entries - 1)))
    return -EINVAL;
  /* This first slice has no setup flags or registered files. */
  if (params->flags || params->sq_thread_cpu || params->sq_thread_idle || params->wq_fd ||
      params->resv[0] || params->resv[1] || params->resv[2]) return -EOPNOTSUPP;
  pthread_once(&iu_fork_once, iu_install_atfork);
  pthread_mutex_lock(&iu_lock);
  struct iu_ring *r = NULL;
  for (size_t i = 0; i < sizeof iu_rings / sizeof iu_rings[0]; i++)
    if (iu_rings[i].alias_count == 0) { r = &iu_rings[i]; break; }
  if (!r) { pthread_mutex_unlock(&iu_lock); return -EMFILE; }
  char path[] = "/tmp/shimmy-iuring-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) { int err = -errno; pthread_mutex_unlock(&iu_lock); return err; }
  unlink(path);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  int anchor = fcntl(fd, F_DUPFD_CLOEXEC, 0);
  if (anchor < 0) {
    int err = -errno; close(fd); pthread_mutex_unlock(&iu_lock); return err;
  }
  size_t sq_len = 64 + entries * sizeof(uint32_t);
  size_t cq_len = 64 + (entries * 2) * IU_CQE_SIZE;
  size_t sqes_len = entries * IU_SQE_SIZE;
  if (ftruncate(fd, (off_t)(IU_OFF_SQES + sqes_len)) != 0) {
    int err = -errno; close(anchor); close(fd); pthread_mutex_unlock(&iu_lock); return err;
  }
  void *sq = mmap(NULL, sq_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IU_OFF_SQ_RING);
  void *cq = mmap(NULL, cq_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IU_OFF_CQ_RING);
  void *sqes = mmap(NULL, sqes_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IU_OFF_SQES);
  if (sq == MAP_FAILED || cq == MAP_FAILED || sqes == MAP_FAILED) {
    int err = -errno;
    if (sq != MAP_FAILED) munmap(sq, sq_len);
    if (cq != MAP_FAILED) munmap(cq, cq_len);
    if (sqes != MAP_FAILED) munmap(sqes, sqes_len);
    close(anchor); close(fd); pthread_mutex_unlock(&iu_lock); return err;
  }
  memset(params, 0, sizeof *params);
  params->sq_entries = entries;
  params->cq_entries = entries * 2;
  params->sq_off = (struct iu_sq_offsets){ .head=0, .tail=4, .ring_mask=8,
    .ring_entries=12, .flags=16, .dropped=20, .array=64 };
  params->cq_off = (struct iu_cq_offsets){ .head=0, .tail=4, .ring_mask=8,
    .ring_entries=12, .overflow=16, .cqes=64 };
  *word(sq, 8) = entries - 1;
  *word(sq, 12) = entries;
  *word(cq, 8) = entries * 2 - 1;
  *word(cq, 12) = entries * 2;
  *r = (struct iu_ring){ .anchor_fd=anchor, .aliases={fd}, .alias_count=1,
    .sq=sq, .cq=cq, .sqes=sqes,
    .sq_len=sq_len, .cq_len=cq_len, .sqes_len=sqes_len,
    .entries=entries, .cq_entries=entries * 2 };
  pthread_mutex_unlock(&iu_lock);
  return fd;
}

int iu_nop_enter(int fd, uint32_t submit, uint32_t min_complete, uint32_t flags) {
  if (flags & ~1u) return -EOPNOTSUPP; /* only GETEVENTS is meaningful */
  pthread_mutex_lock(&iu_lock);
  struct iu_ring *r = iu_find(fd);
  if (!r) { pthread_mutex_unlock(&iu_lock); return -EBADF; }
  uint32_t sh = __atomic_load_n(word(r->sq, 0), __ATOMIC_RELAXED);
  uint32_t st = __atomic_load_n(word(r->sq, 4), __ATOMIC_ACQUIRE);
  uint32_t ch = __atomic_load_n(word(r->cq, 0), __ATOMIC_ACQUIRE);
  uint32_t ct = __atomic_load_n(word(r->cq, 4), __ATOMIC_RELAXED);
  if (st - sh > r->entries || ct - ch > r->cq_entries || submit > r->entries) {
    pthread_mutex_unlock(&iu_lock); return -EINVAL;
  }
  uint32_t count = submit < st - sh ? submit : st - sh;
  if (count > r->cq_entries - (ct - ch)) count = r->cq_entries - (ct - ch);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t idx = ((uint32_t *)((char *)r->sq + 64))[(sh + i) & (r->entries - 1)];
    if (idx >= r->entries) { pthread_mutex_unlock(&iu_lock); return -EINVAL; }
    const unsigned char *sqe = (const unsigned char *)r->sqes + idx * IU_SQE_SIZE;
    unsigned char *cqe = (unsigned char *)r->cq + 64 + ((ct + i) & (r->cq_entries - 1)) * IU_CQE_SIZE;
    uint64_t user_data;
    memcpy(&user_data, sqe + 32, sizeof user_data);
    int32_t result = (sqe[0] == 0 && sqe[1] == 0) ? 0 : -EOPNOTSUPP;
    uint32_t cqe_flags = 0;
    memcpy(cqe, &user_data, sizeof user_data);
    memcpy(cqe + 8, &result, sizeof result);
    memcpy(cqe + 12, &cqe_flags, sizeof cqe_flags);
  }
  __atomic_store_n(word(r->cq, 4), ct + count, __ATOMIC_RELEASE);
  __atomic_store_n(word(r->sq, 0), sh + count, __ATOMIC_RELEASE);
  uint32_t available = ct + count - ch;
  pthread_mutex_unlock(&iu_lock);
  if ((flags & 1u) && available < min_complete) return -EAGAIN;
  return (int)count;
}

int iu_nop_probe(int fd, uint8_t *supported, uint32_t count) {
  pthread_mutex_lock(&iu_lock);
  int found = iu_find(fd) != NULL;
  if (found && supported && count) { memset(supported, 0, count); supported[0] = 1; }
  pthread_mutex_unlock(&iu_lock);
  return found ? 0 : -EBADF;
}

void iu_nop_forget(int fd) {
  (void)iu_nop_close(fd);
}

int iu_nop_dup(int fd, int cloexec) {
  pthread_mutex_lock(&iu_lock);
  struct iu_ring *r = iu_find(fd);
  if (!r) { pthread_mutex_unlock(&iu_lock); return -EBADF; }
  if (r->alias_count >= 32) { pthread_mutex_unlock(&iu_lock); return -EMFILE; }
  int alias = fcntl(fd, cloexec ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
  if (alias >= 0) r->aliases[r->alias_count++] = alias;
  int result = alias < 0 ? -errno : alias;
  pthread_mutex_unlock(&iu_lock);
  return result;
}

int iu_nop_close(int fd) {
  pthread_mutex_lock(&iu_lock);
  struct iu_ring *r = iu_find(fd);
  if (!r) { pthread_mutex_unlock(&iu_lock); return -EBADF; }
  unsigned pos = 0;
  while (pos < r->alias_count && r->aliases[pos] != fd) pos++;
  if (pos == r->alias_count) { pthread_mutex_unlock(&iu_lock); return -EBADF; }
  int result = close(fd) == 0 ? 0 : -errno;
  if (result == 0) {
    r->aliases[pos] = r->aliases[--r->alias_count];
    if (!r->alias_count) iu_release(r);
  }
  pthread_mutex_unlock(&iu_lock);
  return result;
}

void *iu_nop_mmap(int fd, uint64_t off, size_t len, int prot, int flags) {
  pthread_mutex_lock(&iu_lock);
  struct iu_ring *r = iu_find(fd);
  if (!r) { errno = EBADF; pthread_mutex_unlock(&iu_lock); return MAP_FAILED; }
  size_t need;
  if (off == IU_OFF_SQ_RING) need = r->sq_len;
  else if (off == IU_OFF_CQ_RING) need = r->cq_len;
  else if (off == IU_OFF_SQES) need = r->sqes_len;
  else { errno = EINVAL; pthread_mutex_unlock(&iu_lock); return MAP_FAILED; }
  size_t page = (size_t)getpagesize();
  size_t rounded = (need + page - 1) & ~(page - 1);
  if (len < need || len > rounded || !(flags & MAP_SHARED) ||
      (flags & MAP_PRIVATE) || (prot & PROT_EXEC) || !(prot & PROT_READ)) {
    errno = EINVAL; pthread_mutex_unlock(&iu_lock); return MAP_FAILED;
  }
  void *p = mmap(NULL, len, prot, MAP_SHARED, fd, (off_t)off);
  pthread_mutex_unlock(&iu_lock);
  return p;
}

short iu_nop_poll_revents(int fd, short requested) {
  pthread_mutex_lock(&iu_lock);
  struct iu_ring *r = iu_find(fd);
  if (!r) { pthread_mutex_unlock(&iu_lock); return POLLNVAL; }
  uint32_t head = __atomic_load_n(word(r->cq, 0), __ATOMIC_ACQUIRE);
  uint32_t tail = __atomic_load_n(word(r->cq, 4), __ATOMIC_ACQUIRE);
  short result = (head != tail && (requested & POLLIN)) ? POLLIN : 0;
  pthread_mutex_unlock(&iu_lock);
  return result;
}
