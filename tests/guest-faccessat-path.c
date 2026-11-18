/* AArch64 guest probe for checked faccessat pathname. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_FACCESSAT_PATH_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char dir[] = "/a", file[] = "/a/marker";
  static const char rel[] = "marker", missing[] = "/a/missing", empty[] = "";
  if (sc4(48, -100, 0, 0, 0) != -14) done(1);
  if (sc4(48, -100, 1, 0, 0) != -14) done(2);
  if (sc4(48, -100, (long)empty, 0, 0) != -2) done(3);
  if (sc4(48, -100, (long)file, 0, 0x200) != 0) done(4);
  if (sc4(48, -100, (long)file, 4, 0) != 0) done(5); /* R_OK */
  if (sc4(48, -100, (long)missing, 0, 0) != -2) done(6);
  long dfd = sc4(56, -100, (long)dir, 65536, 0); /* O_DIRECTORY */
  if (dfd < 0) done(7);
  if (sc4(48, dfd, (long)rel, 0, 0) != 0) done(8);
  done(0);
}
