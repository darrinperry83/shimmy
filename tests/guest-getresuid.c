/* Native AArch64 getresuid credential tuple and checked output probe. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_GETRESUID_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  unsigned r = 99, e = 99, s = 99;
#ifdef VIRTUAL_UID
  if (sc3(148, (long)&r, (long)&e, (long)&s) || r || e || s) done(1);
  if (sc3(148, 0, (long)&e, (long)&s) != -14) done(2);
  if (sc3(148, (long)&r, 1, (long)&s) != -14) done(3);
  if (sc3(148, (long)&r, (long)&e, 1) != -14) done(4);
  if (sc3(147, 1001, 1002, 1003) != 0) done(5);
  if (sc3(148, (long)&r, (long)&e, (long)&s) != 0) done(6);
  if (r != 1001 || e != 1002 || s != 1003) done(7);
#else
  if (sc3(148, (long)&r, (long)&e, (long)&s) != 0) done(8);
  if (r != (unsigned)sc3(174, 0, 0, 0) ||
      e != (unsigned)sc3(175, 0, 0, 0) || s == 99) done(9);
  if (sc3(148, 0, (long)&e, (long)&s) != -14) done(10);
#endif
  done(0);
}
