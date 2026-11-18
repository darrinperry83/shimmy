/* ARM64 umask: return, mode filtering, and fork parent isolation. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_UMASK_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  if (sc4(166, 022, 0, 0, 0) < 0) finish(1);
  if (sc4(166, 077, 0, 0, 0) != 022) finish(2);
  if (sc4(166, 022, 0, 0, 0) != 077) finish(3);
  if (sc4(166, 07777, 0, 0, 0) != 022) finish(10);
  if (sc4(166, 022, 0, 0, 0) != 0777) finish(11);
  long child = sc4(220, 17, 0, 0, 0); /* clone(SIGCHLD) */
  if (child == 0) {
    if (sc4(166, 077, 0, 0, 0) != 022) finish(4);
    static const char path[] = "/tmp/child";
    if (sc4(34, -100, (long)path, 0777, 0) != 0) finish(5);
    finish(0);
  }
  if (child < 0) finish(6);
  int status = -1;
  if (sc4(260, child, (long)&status, 0, 0) != child || status) finish(7);
  if (sc4(166, 0022, 0, 0, 0) != 0022) finish(8);
  static const char path[] = "/tmp/parent";
  if (sc4(34, -100, (long)path, 0777, 0) != 0) finish(9);
  finish(0);
}
