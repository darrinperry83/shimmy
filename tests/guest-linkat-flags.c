/* AArch64 linkat: Linux flag validation before any filesystem mutation. */
#define AT_FDCWD -100L
#define AT_SYMLINK_FOLLOW 0x400L
#define AT_EMPTY_PATH 0x1000L

static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_LINKAT_FLAGS_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char src[] = "/tmp/source";
  static const char sym[] = "/tmp/source-sym";
  static const char bad[] = "/tmp/bad-link";
  static const char empty[] = "/tmp/empty-link";
  static const char plain[] = "/tmp/plain-link";
  static const char followed[] = "/tmp/followed-link";
  if (sc5(37, AT_FDCWD, (long)src, AT_FDCWD, (long)bad, 0x20000000) != -22) done(1);
  if (sc5(37, AT_FDCWD, 1, AT_FDCWD, (long)bad, 0x20000000) != -22) done(2);
  if (sc5(37, AT_FDCWD, (long)src, AT_FDCWD, (long)empty, AT_EMPTY_PATH) != -95) done(3);
  if (sc5(37, AT_FDCWD, (long)sym, AT_FDCWD, (long)plain, 0) != 0) done(4);
  if (sc5(37, AT_FDCWD, (long)sym, AT_FDCWD, (long)followed, AT_SYMLINK_FOLLOW) != 0) done(5);
  done(0);
}
