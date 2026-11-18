#include "io_uring_nop.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Prototype of a bounded NOP engine. No syscall dispatch calls these functions.
 * A regular host fd still has the wrong poll/read/dup/fork semantics for Linux
 * io_uring, so this module must not be advertised to a guest yet. */
struct iu_ring {
  int fd;
  void *sq, *cq, *sqes;
  size_t sq_len, cq_len, sqes_len;
  uint32_t entries, cq_entries;
};
static pthread_mutex_t iu_lock = PTHREAD_MUTEX_INITIALIZER;
static struct iu_ring iu_rings[8];

static uint32_t *word(void *base, size_t off) { return (uint32_t *)((char *)base + off); }

int iu_nop_setup(uint32_t entries, struct iu_params *params) {
  if (!params || !entries || entries > IU_MAX_ENTRIES || (entries & (entries - 1)))
    return -EINVAL;
  /* This first slice has no setup flags or registered files. */
  if (params->flags || params->sq_thread_cpu || params->sq_thread_idle || params->wq_fd ||
      params->resv[0] || params->resv[1] || params->resv[2]) return -EOPNOTSUPP;
  pthread_mutex_lock(&iu_lock);
  struct iu_ring *r = NULL;
  for (size_t i = 0; i < sizeof iu_rings / sizeof iu_rings[0]; i++)
    if (iu_rings[i].fd == 0) { r = &iu_rings[i]; break; }
  if (!r) { pthread_mutex_unlock(&iu_lock); return -EMFILE; }
  char path[] = "/tmp/shimmy-iuring-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) { int err = -errno; pthread_mutex_unlock(&iu_lock); return err; }
  unlink(path);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  size_t sq_len = 64 + entries * sizeof(uint32_t);
  size_t cq_len = 64 + (entries * 2) * IU_CQE_SIZE;
  size_t sqes_len = entries * IU_SQE_SIZE;
  if (ftruncate(fd, (off_t)(IU_OFF_SQES + sqes_len)) != 0) {
    int err = -errno; close(fd); pthread_mutex_unlock(&iu_lock); return err;
  }
  void *sq = mmap(NULL, sq_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IU_OFF_SQ_RING);
  void *cq = mmap(NULL, cq_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IU_OFF_CQ_RING);
  void *sqes = mmap(NULL, sqes_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IU_OFF_SQES);
  if (sq == MAP_FAILED || cq == MAP_FAILED || sqes == MAP_FAILED) {
    int err = -errno;
    if (sq != MAP_FAILED) munmap(sq, sq_len);
    if (cq != MAP_FAILED) munmap(cq, cq_len);
    if (sqes != MAP_FAILED) munmap(sqes, sqes_len);
    close(fd); pthread_mutex_unlock(&iu_lock); return err;
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
  *r = (struct iu_ring){ .fd=fd, .sq=sq, .cq=cq, .sqes=sqes,
    .sq_len=sq_len, .cq_len=cq_len, .sqes_len=sqes_len,
    .entries=entries, .cq_entries=entries * 2 };
  pthread_mutex_unlock(&iu_lock);
  return fd;
}

int iu_nop_enter(int fd, uint32_t submit, uint32_t min_complete, uint32_t flags) {
  if (flags & ~1u) return -EOPNOTSUPP; /* only GETEVENTS is meaningful */
  pthread_mutex_lock(&iu_lock);
  struct iu_ring *r = NULL;
  for (size_t i = 0; i < sizeof iu_rings / sizeof iu_rings[0]; i++)
    if (iu_rings[i].fd == fd) { r = &iu_rings[i]; break; }
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
  int found = 0;
  for (size_t i = 0; i < sizeof iu_rings / sizeof iu_rings[0]; i++)
    if (iu_rings[i].fd == fd) found = 1;
  if (found && supported && count) { memset(supported, 0, count); supported[0] = 1; }
  pthread_mutex_unlock(&iu_lock);
  return found ? 0 : -EBADF;
}

void iu_nop_forget(int fd) {
  pthread_mutex_lock(&iu_lock);
  for (size_t i = 0; i < sizeof iu_rings / sizeof iu_rings[0]; i++) {
    struct iu_ring *r = &iu_rings[i];
    if (r->fd != fd) continue;
    munmap(r->sq, r->sq_len); munmap(r->cq, r->cq_len);
    munmap(r->sqes, r->sqes_len);
    memset(r, 0, sizeof *r);
    break;
  }
  pthread_mutex_unlock(&iu_lock);
}
