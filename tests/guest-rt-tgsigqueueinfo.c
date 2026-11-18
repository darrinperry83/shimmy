/* Disposable raw ARM64 rt_tgsigqueueinfo target and signal-zero probe. */
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
  static const char ok[] = "GUEST_RT_TGSIGQUEUEINFO_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static unsigned char info[128];
void _start(void) {
  long pid = sc4(172, 0, 0, 0, 0), tid = sc4(178, 0, 0, 0, 0);
  if (pid <= 0 || tid <= 0) finish(1);
  if (sc4(240, pid, tid, 0, (long)info) != 0) finish(2);
  if (sc4(240, pid, tid, 0, 0) != -14) finish(3);
  if (sc4(240, pid, tid, 0, 1) != -14) finish(4);
  if (sc4(240, pid, tid, 65, (long)info) != -22) finish(5);
  if (sc4(240, 0, tid, 0, (long)info) != -22) finish(6);
  if (sc4(240, pid, 0, 0, (long)info) != -22) finish(7);
  if (sc4(240, 999999, tid, 0, (long)info) != -3) finish(8);
  if (sc4(240, pid, 999999, 0, (long)info) != -3) finish(9);
  if (sc4(240, pid, tid, 10, (long)info) != -95) finish(10);
  if (sc4(240, pid, tid, 9, (long)info) != -95) finish(11); /* no host SIGKILL */
  finish(0);
}
