// guest-batch1-syscalls.c — no-libc arm64 guest exercising the five batch-1 syscalls:
//   rseq(293), epoll_pwait2(441), pkey_alloc(289), pkey_free(290), memfd_secret(447).
#include <stdint.h>

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
    static const char ok[] = "BATCH1_SYSCALLS_OK\n";
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

// Linux struct rseq (32 bytes).
struct rseq {
  uint32_t cpu_id_start, cpu_id;
  uint64_t rseq_cs;
  uint32_t flags, node_id, mm_cid, pad;
};

// Linux struct epoll_event (16 bytes).
struct epoll_event {
  uint32_t events, __pad;
  uint64_t data;
};

// Linux struct timespec (two 64-bit words).
struct timespec64 { int64_t sec, nsec; };

enum {
  EINVAL = 22,
  EBUSY  = 16,
  EFAULT = 14,
  EPOLL_CTL_ADD = 1,
  EPOLLIN = 1,
  RSEQ_FLAG_UNREGISTER = 1,
};

void _start(void) {
  // ---------------------------------------------------------------- rseq(293)
  struct rseq r;
  fill(&r, 0xAB, sizeof r);
  if (sc(293, (long)&r, 32, 0, 0xABCD0001UL, 0, 0) != 0) fail(10);
  if (r.cpu_id_start != 0 || r.cpu_id != 0 || r.rseq_cs != 0) fail(11);
  // Already registered: a second register (different sig) must be EBUSY.
  if (sc(293, (long)&r, 32, 0, 0xDEAD0002UL, 0, 0) != -EBUSY) fail(12);
  // Unregister with the wrong signature must be EINVAL.
  if (sc(293, 0, 0, RSEQ_FLAG_UNREGISTER, 0x0000BEEFUL, 0, 0) != -EINVAL) fail(13);
  // Correct unregister succeeds.
  if (sc(293, 0, 0, RSEQ_FLAG_UNREGISTER, 0xABCD0001UL, 0, 0) != 0) fail(14);
  // Now unregistered: another unregister must be EINVAL.
  if (sc(293, 0, 0, RSEQ_FLAG_UNREGISTER, 0xABCD0001UL, 0, 0) != -EINVAL) fail(15);
  // Bad pointer / short length rejected.
  if (sc(293, (long)&r, 31, 0, 0x12340001UL, 0, 0) != -EINVAL) fail(16);
  if (sc(293, 0, 32, 0, 0x12340001UL, 0, 0) != -EINVAL) fail(17);

  // ---------------------------------------------------------- epoll_pwait2(441)
  long ep = sc(20, 0, 0, 0, 0, 0, 0);              // epoll_create1(0)
  if (ep < 0) fail(20);
  int fds[2] = {-1, -1};
  if (sc(59, (long)fds, 0, 0, 0, 0, 0) != 0) fail(21);  // pipe2
  struct epoll_event interest = { EPOLLIN, 0, UINT64_C(0x123456789abcdef0) };
  if (sc(21, ep, EPOLL_CTL_ADD, fds[0], (long)&interest, 0, 0) != 0) fail(22);

  static const char one = 'x';
  if (sc(64, fds[1], (long)&one, 1, 0, 0, 0) != 1) fail(23);  // write -> ready

  struct epoll_event out = { 0, 0, UINT64_C(0x5555555555555555) };
  struct timespec64 ts = { 0, 0 };
  long n = sc(441, ep, (long)&out, 1, (long)&ts, 0, 0);       // poll (fires now)
  if (n != 1) fail(24);
  if (!(out.events & EPOLLIN) || out.data != interest.data) fail(25);

  // Drain, then a real 20ms timeout must fire with zero ready events.
  char c;
  if (sc(63, fds[0], (long)&c, 1, 0, 0, 0) != 1) fail(26);
  out = (struct epoll_event){ 0, 0, UINT64_C(0x5555555555555555) };
  ts.sec = 0; ts.nsec = 20000000;
  n = sc(441, ep, (long)&out, 1, (long)&ts, 0, 0);
  if (n != 0) fail(27);

  // ------------------------------------------------- pkey_alloc(289)/free(290)
  long k0 = sc(289, 0, 0, 0, 0, 0, 0);
  if (k0 != 0) fail(30);
  long k1 = sc(289, 0, 0, 0, 0, 0, 0);
  if (k1 != 1) fail(31);
  if (sc(289, 1, 0, 0, 0, 0, 0) != -EINVAL) fail(32);         // bad flags
  if (sc(289, 0, 0x4, 0, 0, 0, 0) != -EINVAL) fail(33);       // bad rights
  if (sc(290, 99, 0, 0, 0, 0, 0) != -EINVAL) fail(34);        // out of range
  if (sc(290, k0, 0, 0, 0, 0, 0) != 0) fail(35);
  if (sc(290, k0, 0, 0, 0, 0, 0) != -EINVAL) fail(36);        // double free
  if (sc(290, k1, 0, 0, 0, 0, 0) != 0) fail(37);
  if (sc(289, 0, 0, 0, 0, 0, 0) != 0) fail(38);               // reused lowest

  // -------------------------------------------------------- memfd_secret(447)
  long mfd = sc(447, 0, 0, 0, 0, 0, 0);
  if (mfd < 0) fail(40);
  static const char payload[] = "secret-payload";
  if (sc(68, mfd, (long)payload, sizeof payload - 1, 4096, 0, 0) != (long)(sizeof payload - 1))
    fail(41);                                                  // pwrite64
  char rbuf[16];
  fill(rbuf, 0, sizeof rbuf);
  if (sc(67, mfd, (long)rbuf, sizeof payload - 1, 4096, 0, 0) != (long)(sizeof payload - 1))
    fail(42);                                                  // pread64
  for (unsigned long i = 0; i < sizeof payload - 1; i++)
    if (rbuf[i] != payload[i]) fail(43);
  // MFD_HUGETLB (0x4) is valid for memfd_create but not for memfd_secret.
  if (sc(447, 0x4, 0, 0, 0, 0, 0) != -EINVAL) fail(44);

  finish(0);
}
