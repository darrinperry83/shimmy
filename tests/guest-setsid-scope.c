/* ARM64 setsid probe: a successful new session returns the virtual PID. */
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code, int is_init) {
  static const char regular[] = "GUEST_SETSID_REGULAR_OK\n";
  static const char init[] = "GUEST_SETSID_INIT_OK\n";
  if (!code) {
    const char *message = is_init ? init : regular;
    sc(64, 1, (long)message, is_init ? sizeof init - 1 : sizeof regular - 1);
  }
  sc(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  long self = sc(172, 0, 0, 0);
  int is_init = self == 1;
  if (self <= 0) done(1, is_init);
  long sid = sc(157, 0, 0, 0);
  if (sid != self) done(2, is_init);
  if (sc(156, 0, 0, 0) != self || sc(156, self, 0, 0) != self)
    done(3, is_init);
  if (sc(155, 0, 0, 0) != self) done(4, is_init);
  if (sc(157, 0, 0, 0) != -1) done(5, is_init); /* EPERM: already a leader */
  done(0, is_init);
}
