/* ARM64 raw ABI: fremovexattr snapshots its guest name before mutation. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "FREMOVEXATTR_NAME_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static const char path[] __attribute__((section(".text"))) = "/tmp/data";
static const char name[] __attribute__((section(".text"))) = "user.shimmy-test";
void _start(void) {
  long fd = sc4(56, -100, (long)path, 0, 0);
  if (fd < 0) done(1);
  if (sc4(10, fd, (long)name, 0, 0) != 3) done(2);
  if (sc4(16, fd, 1, 0, 0) != -14) done(3);
  if (sc4(10, fd, (long)name, 0, 0) != 3) done(4); /* no mutation */
  if (sc4(16, fd, (long)name, 0, 0) != 0) done(5);
  if (sc4(10, fd, (long)name, 0, 0) >= 0) done(6);
  if (sc4(16, -1, (long)name, 0, 0) != -9) done(7);
  done(0);
}
