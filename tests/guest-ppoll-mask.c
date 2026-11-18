// Libc-free ARM64 probe for ppoll's checked buffers and signal-mask boundary.
typedef unsigned long u64;
struct pollfd { int fd; short events, revents; };
struct timespec { long sec, nsec; };
static long call(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2),
                   "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int status) {
  static const char ok[] = "GUEST_PPOLL_MASK_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  call(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  struct timespec zero = {0, 0}, bad = {0, 1000000000};
  u64 mask = 0;
  int pipefd[2];
  if (call(59, (long)pipefd, 0, 0, 0, 0, 0)) done(1);
  struct pollfd p = {pipefd[0], 1, 0};
  // An unsupported temporary mask must not consume the ready event.
  static const char byte = 'x';
  if (call(64, pipefd[1], (long)&byte, 1, 0, 0, 0) != 1) done(2);
  if (call(73, (long)&p, 1, (long)&zero, (long)&mask, 7, 0) != -22) done(3);
  if (call(73, (long)&p, 1, (long)&zero, 1, 8, 0) != -14) done(4);
  if (call(73, (long)&p, 1, (long)&zero, (long)&mask, 8, 0) != -95) done(5);
  if (call(73, (long)&p, 1, (long)&zero, 0, 0, 0) != 1 || !(p.revents & 1)) done(6);
  if (call(73, (long)&p, 1, (long)&bad, 0, 0, 0) != -22) done(7);
  if (call(73, 1, 1, (long)&zero, 0, 0, 0) != -14) done(8);
  if (call(73, (long)&p, 4097, (long)&zero, 0, 0, 0) != -22) done(9);
  if (call(73, 0, 0, (long)&zero, 0, 0, 0) != 0) done(10);
  call(57, pipefd[0], 0, 0, 0, 0, 0);
  call(57, pipefd[1], 0, 0, 0, 0, 0);
  done(0);
}
