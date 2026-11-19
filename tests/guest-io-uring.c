// guest-io-uring.c — no-libc arm64 guest exercising the shim's synchronous
// io_uring fallback: io_uring_setup -> mmap the SQ/CQ/SQES rings -> submit a NOP
// and a pipe WRITE -> read back the CQEs -> plus process_mrelease sanity checks.
#include <stdint.h>

typedef unsigned long u64;
typedef unsigned int  u32;
typedef int           s32;
typedef uint8_t       u8;
typedef uint16_t      u16;

static long sc(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                   : "memory");
  return x0;
}

static __attribute__((noreturn)) void finish(int code) {
  if (!code) {
    static const char ok[] = "IO_URING_OK\n";
    sc(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  }
  sc(93, code, 0, 0, 0, 0, 0);
  for (;;) {}
}

static void fail(int code) { finish(code); }

static void fill(void *p, unsigned char v, unsigned long n) {
  unsigned char *b = (unsigned char *)p;
  for (unsigned long i = 0; i < n; i++) b[i] = v;
}

// Linux uapi (aarch64 LP64) — must match io_uring_shim.c.
struct io_sqring_offsets {
  u32 head, tail, ring_mask, ring_entries, flags, dropped, array, resv1;
  u64 user_addr;
};
struct io_cqring_offsets {
  u32 head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1;
  u64 user_addr;
};
struct io_uring_params {
  u32 sq_entries, cq_entries, flags, sq_thread_cpu, sq_thread_idle, features, wq_fd;
  u32 resv[3];
  struct io_sqring_offsets sq_off;
  struct io_cqring_offsets cq_off;
};
struct io_uring_sqe {
  u8  opcode, flags;
  u16 ioprio;
  s32 fd;
  u64 off;
  u64 addr;
  u32 len;
  u32 rw_flags;
  u64 user_data;
  u64 pad[3];
};
struct io_uring_cqe { u64 user_data; s32 res; u32 flags; };

#define IORING_OFF_SQ_RING 0ULL
#define IORING_OFF_CQ_RING 0x8000000ULL
#define IORING_OFF_SQES    0x10000000ULL
#define IORING_OP_NOP      0
#define IORING_OP_WRITE    23

#define MAP_SHARED   0x01
#define MAP_POPULATE 0x008000
#define PROT_READ    1
#define PROT_WRITE   2

#define EINVAL 22
#define EBADF  9

void _start(void) {
  // ---------------------------------------------------------- io_uring_setup
  struct io_uring_params p;
  fill(&p, 0, sizeof p);
  long fd = sc(425, 8, (long)&p, 0, 0, 0, 0);
  if (fd < 0) fail(10);
  if (p.sq_entries != 8) fail(11);
  if (p.cq_entries != 16) fail(12);
  if (!(p.features & 1)) fail(13);           // IORING_FEAT_SINGLE_MMAP

  // ---------------------------------------------------------------- mmap rings
  long sq_sz   = (long)p.sq_off.array + (long)p.sq_entries * 4;
  long cq_sz   = (long)p.cq_off.cqes + (long)p.cq_entries * 16;
  long sqes_sz = (long)p.sq_entries * 64;

  char *sq_ring = (char *)sc(222, 0, sq_sz, PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
  if ((long)sq_ring < 0) fail(20);
  char *cq_ring = (char *)sc(222, 0, cq_sz, PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
  if ((long)cq_ring < 0) fail(21);
  char *sqes = (char *)sc(222, 0, sqes_sz, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
  if ((long)sqes < 0) fail(22);

  volatile u32 *sq_head  = (volatile u32 *)(sq_ring + p.sq_off.head);
  volatile u32 *sq_tail  = (volatile u32 *)(sq_ring + p.sq_off.tail);
  volatile u32 *sq_mask  = (volatile u32 *)(sq_ring + p.sq_off.ring_mask);
  volatile u32 *sq_array = (volatile u32 *)(sq_ring + p.sq_off.array);
  volatile u32 *cq_head  = (volatile u32 *)(cq_ring + p.cq_off.head);
  volatile u32 *cq_tail  = (volatile u32 *)(cq_ring + p.cq_off.tail);
  volatile u32 *cq_mask  = (volatile u32 *)(cq_ring + p.cq_off.ring_mask);
  struct io_uring_cqe *cqes = (struct io_uring_cqe *)(cq_ring + p.cq_off.cqes);

  if (*sq_mask != 7) fail(23);               // sq_entries - 1
  if (*cq_mask != 15) fail(24);              // cq_entries - 1

  // ------------------------------------------------------------------- NOP
  struct io_uring_sqe *s = (struct io_uring_sqe *)sqes;
  fill(s, 0, 64);
  s[0].opcode = IORING_OP_NOP;
  s[0].fd = -1;
  s[0].user_data = UINT64_C(0x1122334455667788);

  u32 tail = *sq_tail;
  sq_array[tail & *sq_mask] = 0;             // SQE index 0
  *sq_tail = tail + 1;

  if (sc(426, fd, 1, 1, 0, 0, 0) != 1) fail(30);   // io_uring_enter

  if (*cq_tail != 1) fail(31);
  u32 chead = *cq_head;
  struct io_uring_cqe cqe = cqes[chead & *cq_mask];
  if (cqe.res != 0) fail(32);
  if (cqe.user_data != UINT64_C(0x1122334455667788)) fail(33);
  *cq_head = chead + 1;
  if (*sq_head != 1) fail(34);

  // ------------------------------------------------------- WRITE to a pipe
  int fds[2] = {-1, -1};
  if (sc(59, (long)fds, 0, 0, 0, 0, 0) != 0) fail(40);   // pipe2
  static const char msg[] = "hello";
  fill(s, 0, 64);
  s[0].opcode = IORING_OP_WRITE;
  s[0].fd = fds[1];
  s[0].addr = (u64)(long)msg;
  s[0].len = 5;
  s[0].user_data = UINT64_C(0xfeedfacecafebeef);

  tail = *sq_tail;
  sq_array[tail & *sq_mask] = 0;
  *sq_tail = tail + 1;

  if (sc(426, fd, 1, 1, 0, 0, 0) != 1) fail(41);
  if (*cq_tail != 2) fail(42);
  chead = *cq_head;
  cqe = cqes[chead & *cq_mask];
  if (cqe.res != 5) fail(43);
  if (cqe.user_data != UINT64_C(0xfeedfacecafebeef)) fail(44);
  *cq_head = chead + 1;

  char rbuf[8];
  fill(rbuf, 0, sizeof rbuf);
  if (sc(63, fds[0], (long)rbuf, 5, 0, 0, 0) != 5) fail(45);
  for (int i = 0; i < 5; i++) if (rbuf[i] != msg[i]) fail(46);

  // -------------------------------------------------------- process_mrelease
  if (sc(448, -1, 1, 0, 0, 0, 0) != -EINVAL) fail(50);   // bad flags
  if (sc(448, -1, 0, 0, 0, 0, 0) != -EBADF) fail(51);    // bad pidfd
  long mypid = sc(172, 0, 0, 0, 0, 0, 0);                // getpid
  long pidfd = sc(434, mypid, 0, 0, 0, 0, 0);            // pidfd_open
  if (pidfd < 0) fail(52);
  if (sc(448, pidfd, 0, 0, 0, 0, 0) != 0) fail(53);      // valid pidfd -> 0

  finish(0);
}
