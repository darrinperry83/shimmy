/* ARM64 getpgid scope probe: no unrelated host PID is a guest lookup key. */
#ifndef HOST_PID
#error HOST_PID must identify the host-side runner
#endif
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_GETPGID_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  long self = sc3(172, 0, 0, 0); /* getpid */
  if (self <= 0) finish(1);
  long before = sc3(155, 0, 0, 0);
  if (before != -95 && before != self) finish(2);
  if (sc3(155, -1, 0, 0) != -22) finish(3);
  if (sc3(155, HOST_PID, 0, 0) != -95) finish(4);
  if (sc3(155, 999999, 0, 0) != -95) finish(5);
  if (sc3(154, 0, 0, 0) != 0) finish(6); /* become own host group */
  if (sc3(155, 0, 0, 0) != self) finish(7);
  if (sc3(155, self, 0, 0) != self) finish(8);
  finish(0);
}
