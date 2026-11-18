/* Linux ARM64 getgroups size and output validation for current synthetic list. */
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_GETGROUPS_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1);
  sc(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  unsigned groups[3] = {99, 99, 99};
  long count = sc(158, 0, 0, 0);
  if (count != 1) done(1);
  if (sc(158, 0, 1, 0) != 1) done(12); /* size=0 never dereferences pointer */
  if (sc(158, -1, (long)groups, 0) != -22) done(2);
  if (sc(158, 1, 1, 0) != -14) done(3);
  if (sc(158, 1, 0, 0) != -14) done(13);
  if (sc(158, 1, (long)groups, 0) != 1 || groups[0] != 0) done(4);
  if (sc(158, 2, (long)groups, 0) != 1) done(5);
  done(0);
}
