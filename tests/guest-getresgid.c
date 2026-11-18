/* Raw AArch64 getresgid probe for checked outputs and virtual saved GID. */
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_GETRESGID_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1);
  sc(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  unsigned r = 999, e = 999, s = 999;
  if (sc(150, (long)&r, (long)&e, (long)&s) != 0) done(1);
  if (r != (unsigned)sc(176, 0, 0, 0) || e != (unsigned)sc(177, 0, 0, 0)) done(2);
  if (sc(150, 0, (long)&e, (long)&s) != -14) done(3);
  if (sc(150, (long)&r, 0, (long)&s) != -14) done(4);
  if (sc(150, (long)&r, (long)&e, 0) != -14) done(5);
  if (sc(150, 1, (long)&e, (long)&s) != -14) done(6);
  if (sc(150, (long)&r, 1, (long)&s) != -14) done(7);
  if (sc(150, (long)&r, (long)&e, 1) != -14) done(8);
  if (r == 0 && e == 0 && s == 0) {
    /* In the RDUID0 guest, all three credential slots are independently modeled. */
    if (sc(149, 17, 18, 19) != 0) done(9);
    if (sc(150, (long)&r, (long)&e, (long)&s) != 0) done(10);
    if (r != 17 || e != 18 || s != 19) done(11);
  }
  done(0);
}
