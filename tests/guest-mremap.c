// Libc-free ARM64 probe for mremap argument validation and mapped shrink.
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
  static const char ok[] = "GUEST_MREMAP_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  call(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  const long page = 16384;
  long map = call(222, 0, page * 2, 3, 0x22, -1, 0);
  if (map < 0) done(1);
  *(volatile char *)map = 0x39;
  if (call(216, map, page * 2, page * 2, 0, 0, 0) != map) done(2);
  if (call(216, map + 1, page * 2, page, 0, 0, 0) != -22) done(3);
  if (call(216, map, page * 2, page, 2, 0, 0) != -95) done(4);
  if (call(216, map, page * 2, page, 4, 0, 0) != -95) done(5);
  if (call(216, map, 0, page, 0, 0, 0) != -95) done(6);
  if (call(216, map, page * 2, 0, 0, 0, 0) != -22) done(7);
  if (call(216, map, page * 2, -1L, 0, 0, 0) != -22) done(8);
  if (call(216, map + page * 4, page, page, 0, 0, 0) != -14) done(9);
  if (call(216, map, page * 2, page, 0, 0, 0) != map) done(10);
  if (*(volatile char *)map != 0x39) done(11);
  if (call(216, map, page * 2, page, 0, 0, 0) != -14) done(12);
  if (call(215, map, page, 0, 0, 0, 0) != 0) done(13);
  done(0);
}
