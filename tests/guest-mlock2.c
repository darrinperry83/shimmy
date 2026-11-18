// libc-free ARM64 guest probe for the bounded mlock2 eager-lock subset.
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
  static const char ok[] = "GUEST_MLOCK2_EAGER_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  call(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}
static long lock(long addr, long len, long flags) {
  return call(284, addr, len, flags, 0, 0, 0);
}

void _start(void) {
  long page = call(222, 0, 32768, 3, 0x22, -1, 0);
  if (page < 0) done(1);
  *(volatile char*)page = 1;
  *(volatile char*)(page + 16384) = 2;
  if (lock(page, 16384, 0) != 0) done(2);
  if (call(229, page, 16384, 0, 0, 0, 0) != 0) done(3);
  if (lock(page + 1, 16384, 0) != 0) done(4); // Linux rounds both pages
  if (call(229, page, 32768, 0, 0, 0, 0) != 0) done(5);
  if (lock(page, 16384, 1) != -95) done(6); // deferred lock unsupported
  if (lock(page, 16384, 2) != -22) done(7); // invalid flag
  if (lock(page, 2 * 1024 * 1024, 0) != -95) done(8); // scope bound
  if (lock(0x100000000L, 16384, 0) != -12) done(9);
  if (lock(0, 0, 0) != 0 || lock(0, 0, 1) != 0) done(10);
  if (call(215, page, 32768, 0, 0, 0, 0) != 0) done(11);
  if (lock(page, 16384, 0) != -12) done(12);
  done(0);
}
