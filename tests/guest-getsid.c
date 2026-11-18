/* Raw ARM64 getsid probe. HOST_PID is an unrelated live host process. */
#ifndef HOST_PID
#error HOST_PID must name the test runner
#endif
static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static long sc3(long nr, long a, long b, long c) { return sc5(nr, a, b, c, 0, 0); }
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_GETSID_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  long self = sc3(172, 0, 0, 0);
  if (self <= 1 || self == HOST_PID) finish(1);
  if (sc3(156, HOST_PID, 0, 0) != -3) finish(2); /* no host PID probing */
  if (sc3(156, -1, 0, 0) != -3) finish(3);
  long initial = sc3(156, 0, 0, 0);
  if (initial != -95 && initial != self) finish(4);
  if (sc3(156, self, 0, 0) != initial) finish(5);
  if (sc3(157, 0, 0, 0) != self) finish(6); /* become a session leader */
  if (sc3(156, 0, 0, 0) != self || sc3(156, self, 0, 0) != self) finish(7);
  long child = sc3(220, 17, 0, 0); /* clone(SIGCHLD) */
  if (child == 0) {
    long own = sc3(172, 0, 0, 0);
    if (sc3(156, 0, 0, 0) != self || sc3(156, own, 0, 0) != self) finish(8);
    finish(0);
  }
  if (child < 0) finish(9);
  int status = -1;
  if (sc3(260, child, (long)&status, 0) != child || status) finish(10);
  if (sc3(156, child, 0, 0) != -95) finish(11); /* known child: not modeled */
  finish(0);
}
