/* AArch64 guest probe for checked openat pathname. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_OPENAT_INPUT_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char path[] = "/tmp/fixture", created[] = "/tmp/created";
  static const char empty[] = "", symlink[] = "/tmp/fixture-link";
  if (sc4(56, -100, 0, 0, 0) != -14) done(1);
  if (sc4(56, -100, 1, 0, 0) != -14) done(2);
  if (sc4(56, -100, (long)empty, 0, 0) != -2) done(3);
  long fd = sc4(56, -100, (long)path, 0, 0);
  if (fd < 0) done(4);
  char b = 0;
  if (sc4(63, fd, (long)&b, 1, 0) != 1 || b != 'F') done(5);
  // Arch AArch64 asm/fcntl.h defines O_NOFOLLOW as octal 0100000 (0x8000).
  if (sc4(56, -100, (long)symlink, 0x8000, 0) != -40) done(6); /* ELOOP */
  if (sc4(56, -100, (long)path, 0x8000, 0) < 0) done(7);
  fd = sc4(56, -100, (long)created, 0x41, 0600); /* O_CREAT | O_WRONLY */
  if (fd < 0) done(8);
  done(0);
}
