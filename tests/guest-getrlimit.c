/* ARM64 getrlimit: checked output, truthful resources, NOFILE parity. */
struct limit { unsigned long long cur, max; };
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_GETRLIMIT_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  struct limit nofile = {0}, prlimit = {0};
  if (sc4(163, 7, (long)&nofile, 0, 0) != 0) done(1);
  if (!nofile.cur || nofile.cur > 4096 || nofile.max < nofile.cur) done(2);
  if (sc4(261, 0, 7, 0, (long)&prlimit) != 0) done(3);
  if (nofile.cur != prlimit.cur || nofile.max != prlimit.max) done(4);
  if (sc4(163, 7, 0, 0, 0) != -14) done(5);
  if (sc4(163, 7, 1, 0, 0) != -14) done(6);
  struct limit sentinel = {123, 456};
  if (sc4(163, 5, (long)&sentinel, 0, 0) != -95) done(7);
  if (sentinel.cur != 123 || sentinel.max != 456) done(8);
  if (sc4(163, 10, (long)&sentinel, 0, 0) != -95) done(9);
  if (sc4(163, 16, (long)&sentinel, 0, 0) != -22) done(10);
  if (sc4(163, -1, (long)&sentinel, 0, 0) != -22) done(11);
  if (sentinel.cur != 123 || sentinel.max != 456) done(12);
  done(0);
}
