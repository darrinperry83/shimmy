/* Disposable raw ARM64 rt_sigqueueinfo safety and signal-zero probe. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_RT_SIGQUEUEINFO_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static unsigned char info[128];
void _start(void) {
  long self = sc3(172, 0, 0, 0); /* Linux getpid */
  if (self <= 0) finish(1);
  if (sc3(138, self, 0, (long)info) != 0) finish(2);
  if (sc3(138, self, 0, 0) != -14) finish(3);
  if (sc3(138, self, 0, 1) != -14) finish(4);
  if (sc3(138, self, 65, (long)info) != -22) finish(5);
  if (sc3(138, 999999, 0, (long)info) != -3) finish(6);
  if (sc3(138, 0, 0, (long)info) != -95) finish(7);
  if (sc3(138, self, 10, (long)info) != -95) finish(8); /* must survive */
  if (sc3(138, self, 9, (long)info) != -95) finish(9);  /* no host SIGKILL */
  finish(0);
}
