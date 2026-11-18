/* ARM64 raw ABI: 8-byte kernel sigset and checked mask input. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_SIGNALFD4_MASK_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static unsigned long long usr2 = 1ULL << (12 - 1), term = 1ULL << (15 - 1);
void _start(void) {
  if (sc4(74, -1, 1, 8, 0) != -14) finish(1);
  if (sc4(74, -1, 0, 8, 0) != -14) finish(2);
  if (sc4(74, -1, (long)&usr2, 7, 0) != -22) finish(3);
  long fd = sc4(74, -1, (long)&usr2, 8, 0x80800);
  if (fd < 0) finish(4);
  if (sc4(74, fd, (long)&term, 8, 0) != fd) finish(5);
  if (sc4(74, fd, 1, 8, 0) != -14) finish(6);
  if (sc4(74, fd, (long)&term, 8, 1) != -22) finish(7);
  finish(0);
}
