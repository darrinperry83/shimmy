/* AArch64 guest probe for checked chdir path and guest cwd. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_CHDIR_PATH_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static int eq(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}
void _start(void) {
  static const char base[] = "/a", child[] = "sub", marker[] = "marker";
  static const char empty[] = "";
  char cwd[64];
  if (sc4(49, 0, 0, 0, 0) != -14) done(1);
  if (sc4(49, 1, 0, 0, 0) != -14) done(2);
  if (sc4(49, (long)base, 0, 0, 0) != 0) done(3);
  if (sc4(17, (long)cwd, sizeof cwd, 0, 0) < 0 || !eq(cwd, "/a")) done(4);
  if (sc4(49, 1, 0, 0, 0) != -14) done(5);
  if (sc4(49, (long)empty, 0, 0, 0) != -2) done(6);
  if (sc4(17, (long)cwd, sizeof cwd, 0, 0) < 0 || !eq(cwd, "/a")) done(7);
  if (sc4(49, (long)child, 0, 0, 0) != 0) done(8);
  if (sc4(17, (long)cwd, sizeof cwd, 0, 0) < 0 || !eq(cwd, "/a/sub")) done(9);
  if (sc4(56, -100, (long)marker, 0, 0) < 0) done(10); /* relative openat */
  done(0);
}
