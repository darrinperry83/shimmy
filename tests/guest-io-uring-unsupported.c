// Libc-free ARM64 regression: the incomplete prototype is not guest-visible.
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
  static const char ok[] = "GUEST_IO_URING_UNSUPPORTED_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  call(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static char params[120];
  if (call(425, 2, (long)params, 0, 0, 0, 0) != -38) done(1);
  if (call(426, -1, 0, 0, 0, 0, 0) != -38) done(2);
  if (call(427, -1, 8, 0, 0, 0, 0) != -38) done(3);
  done(0);
}
