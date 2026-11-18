/* ARM64 guest probe for truthful setfsgid query/no-change subset. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_SETFSGID_NOCHANGE_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  if (sc3(177, 0, 0, 0) != 0) done(1);              /* getegid */
  if (sc3(152, -1, 0, 0) != 0) done(2);             /* query */
  if (sc3(152, 0, 0, 0) != 0) done(3);              /* no change */
  if (sc3(152, 5, 0, 0) != -95) done(4);            /* unsupported change */
  if (sc3(177, 0, 0, 0) != 0) done(5);
  if (sc3(144, 5, 0, 0) != 0) done(6);              /* virtual setgid */
  if (sc3(177, 0, 0, 0) != 5) done(7);
  if (sc3(152, -1, 0, 0) != 5) done(8);
  if (sc3(152, 5, 0, 0) != 5) done(9);
  if (sc3(152, 6, 0, 0) != -95) done(10);
  if (sc3(177, 0, 0, 0) != 5) done(11);
  done(0);
}
