#ifndef SHIMMY_IO_URING_NOP_H
#define SHIMMY_IO_URING_NOP_H

#include <stdint.h>
#include <stddef.h>

/* Linux aarch64 UAPI layout. This backend is not wired to guest syscalls yet. */
struct iu_sq_offsets {
  uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array, resv1;
  uint64_t user_addr;
};
struct iu_cq_offsets {
  uint32_t head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1;
  uint64_t user_addr;
};
struct iu_params {
  uint32_t sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle;
  uint32_t features, wq_fd, resv[3];
  struct iu_sq_offsets sq_off;
  struct iu_cq_offsets cq_off;
};

enum { IU_MAX_ENTRIES = 64, IU_SQE_SIZE = 64, IU_CQE_SIZE = 16 };
#define IU_OFF_SQ_RING UINT64_C(0)
#define IU_OFF_CQ_RING UINT64_C(0x8000000)
#define IU_OFF_SQES UINT64_C(0x10000000)

/* Returns a private, unlinked host fd; params are populated on success. */
int iu_nop_setup(uint32_t entries, struct iu_params *params);
/* fd is a ring returned by setup. Consumes at most submit NOP SQEs. */
int iu_nop_enter(int fd, uint32_t submit, uint32_t min_complete, uint32_t flags);
/* Only one operation is supported: IORING_OP_NOP (0). */
int iu_nop_probe(int fd, uint8_t *supported, uint32_t count);
/* Prototype fd hooks. Guest dup/close/close_range must call these when wired. */
int iu_nop_dup(int fd, int cloexec);
int iu_nop_close(int fd);
/* Enforces the three Linux mmap offsets/lengths and shared, non-exec access. */
void *iu_nop_mmap(int fd, uint64_t off, size_t len, int prot, int flags);
/* Returns a CLOEXEC duplicate of private backing fd after the same checks.
 * A future guest mmap dispatcher can pass it to gmem_mmap, then close it. */
int iu_nop_mmap_backing_fd(int fd, uint64_t off, size_t len, int prot, int flags);
/* CQ readiness for a future poll/epoll interceptor; no host fd poll claim. */
short iu_nop_poll_revents(int fd, short requested);
void iu_nop_forget(int fd);

#endif
