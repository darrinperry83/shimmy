/* AArch64 guest probe for fchownat pathname checking. */
static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_FCHOWNAT_PATH_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char path[] = "/tmp/fchownat-path-file";
  static const char empty[] = "";
  if (sc5(54, -100, 1, 123, 456, 0) != -14) done(1);
  if (sc5(54, -100, 1, 123, 456, 0x1000) != -14) done(2);
#ifndef HOST_ONLY
  if (sc5(54, -100, (long)path, 123, 456, 0) != 0) done(3);
  long fd = sc5(56, -100, (long)path, 0, 0, 0); /* openat read-only */
  if (fd < 0) done(4);
  if (sc5(54, fd, (long)empty, -1, 457, 0x1000) != 0) done(5);
  /* Keep the pre-existing NULL + AT_EMPTY_PATH behavior. */
  if (sc5(54, fd, 0, -1, 458, 0x1000) != 0) done(6);
#endif
  done(0);
}
