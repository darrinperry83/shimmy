#include "io_uring_nop.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/wait.h>

static uint32_t *at32(void *p, uint32_t off) { return (uint32_t *)((char *)p + off); }

int main(void) {
  struct iu_params p = {0};
  assert(sizeof p == 120);
  assert(iu_nop_setup(3, &p) == -EINVAL);
  p.flags = 1;
  assert(iu_nop_setup(4, &p) == -EOPNOTSUPP);
  memset(&p, 0, sizeof p);
  int fd = iu_nop_setup(4, &p);
  assert(fd >= 0);
  assert(p.sq_entries == 4 && p.cq_entries == 8 && p.features == 0);
  assert(iu_nop_mmap(fd, 1, 4096, PROT_READ | PROT_WRITE, MAP_SHARED) == MAP_FAILED);
  assert(errno == EINVAL);
  assert(iu_nop_mmap(fd, IU_OFF_SQ_RING, 4096, PROT_READ | PROT_EXEC, MAP_SHARED) == MAP_FAILED);
  assert(errno == EINVAL);
  assert(iu_nop_mmap(fd, IU_OFF_SQ_RING, 16, PROT_READ | PROT_WRITE, MAP_SHARED) == MAP_FAILED);
  assert(errno == EINVAL);
  void *sq = iu_nop_mmap(fd, IU_OFF_SQ_RING, 4096, PROT_READ | PROT_WRITE, MAP_SHARED);
  void *cq = iu_nop_mmap(fd, IU_OFF_CQ_RING, 4096, PROT_READ | PROT_WRITE, MAP_SHARED);
  void *sqes = iu_nop_mmap(fd, IU_OFF_SQES, 4096, PROT_READ | PROT_WRITE, MAP_SHARED);
  assert(sq != MAP_FAILED && cq != MAP_FAILED && sqes != MAP_FAILED);
  assert(*at32(sq, p.sq_off.ring_mask) == 3);
  assert(*at32(cq, p.cq_off.ring_entries) == 8);
  uint8_t supported[4] = {8,8,8,8};
  assert(iu_nop_probe(fd, supported, 4) == 0);
  assert(supported[0] == 1 && supported[1] == 0);
  assert(iu_nop_enter(fd, 0, 0, 0) == 0);
  assert(iu_nop_poll_revents(fd, POLLIN) == 0);
  for (uint32_t n = 0; n < 12; n++) {
    uint32_t tail = *at32(sq, p.sq_off.tail);
    uint32_t idx = tail & 3;
    ((uint32_t *)((char *)sq + p.sq_off.array))[idx] = idx;
    unsigned char *entry = (unsigned char *)sqes + idx * IU_SQE_SIZE;
    memset(entry, 0, IU_SQE_SIZE);
    uint64_t data = UINT64_C(0xabc000) + n;
    memcpy(entry + 32, &data, sizeof data);
    *at32(sq, p.sq_off.tail) = tail + 1;
    assert(iu_nop_enter(fd, 1, 1, 1) == 1);
    assert(*at32(sq, p.sq_off.head) == tail + 1);
    uint32_t head = *at32(cq, p.cq_off.head);
    uint32_t ctail = *at32(cq, p.cq_off.tail);
    assert(ctail == head + 1);
    assert(iu_nop_poll_revents(fd, POLLIN) == POLLIN);
    unsigned char *cqe = (unsigned char *)cq + p.cq_off.cqes + (head & 7) * IU_CQE_SIZE;
    uint64_t got_data; int32_t res; uint32_t flags;
    memcpy(&got_data, cqe, sizeof got_data);
    memcpy(&res, cqe + 8, sizeof res);
    memcpy(&flags, cqe + 12, sizeof flags);
    assert(got_data == data && res == 0 && flags == 0);
    *at32(cq, p.cq_off.head) = head + 1;
    assert(iu_nop_poll_revents(fd, POLLIN) == 0);
  }
  assert(iu_nop_enter(fd, 0, 1, 1) == -EAGAIN);
  assert(iu_nop_enter(fd, 0, 0, 2) == -EOPNOTSUPP);
  int alias = iu_nop_dup(fd, 0);
  assert(alias >= 0 && alias != fd);
  assert((fcntl(alias, F_GETFD) & FD_CLOEXEC) == 0);
  assert(iu_nop_close(fd) == 0);
  assert(iu_nop_enter(fd, 0, 0, 0) == -EBADF);
  assert(iu_nop_enter(alias, 0, 0, 0) == 0);
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    int ok = iu_nop_enter(alias, 0, 0, 0) == 0 && iu_nop_close(alias) == 0;
    _exit(ok ? 0 : 1);
  }
  int status = -1;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(iu_nop_enter(alias, 0, 0, 0) == 0);
  munmap(sq, 4096); munmap(cq, 4096); munmap(sqes, 4096);
  assert(iu_nop_close(alias) == 0);
  assert(iu_nop_enter(alias, 0, 0, 0) == -EBADF);
  puts("io_uring NOP host backend: mappings, CQ readiness, dup, close, fork OK");
}
