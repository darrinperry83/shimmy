/* ARM64 wait4: validate arguments before reaping and scope waits to guest children. */
static long sc(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_WAIT4_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1, 0);
  sc(93, code, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  int status = -1;
  static long usage[18];
  if (sc(260, 2147483647, (long)&status, 1, 0) != -10) done(1);
  if (sc(260, -1, (long)&status, 4, 0) != -22) done(2);
  if (sc(260, 0, (long)&status, 1, 0) != -95) done(3);
  long child = sc(220, 17, 0, 0, 0); /* clone(SIGCHLD) */
  if (child == 0) {
    static const long pause[2] = {0, 100000000};
    sc(101, (long)pause, 0, 0, 0);
    done(42);
  }
  if (child < 0) done(4);
  if (sc(260, child, 1, 1, 0) != -14) done(5);
  if (sc(260, child, (long)&status, 1, 1) != -14) done(6);
  if (sc(260, -1, (long)&status, 0, (long)usage) != child) done(7);
  if (status != (42 << 8)) done(8);
  if (sc(260, child, (long)&status, 1, 0) != -10) done(9);
  long second = sc(220, 17, 0, 0, 0);
  if (!second) done(7);
  if (second < 0) done(10);
  status = -1;
  if (sc(260, second, (long)&status, 0, 0) != second || status != (7 << 8)) done(11);
  done(0);
}
