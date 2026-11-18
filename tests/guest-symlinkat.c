/* Disposable ARM64 probe for symlinkat's guest string contract. */
#define AT_FDCWD (-100L)
static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_SYMLINKAT_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char target[] = "../ordinary-target";
  static const char link[] = "/tmp/symlink-a";
  static const char longlink[] = "/tmp/symlink-too-long";
  static char readback[64];
  static char unterminated[4096];
  for (int i = 0; i < 4096; ++i) unterminated[i] = 'x';
  if (sc5(36, 1, AT_FDCWD, (long)link, 0, 0) != -14) done(1);
  if (sc5(36, (long)target, AT_FDCWD, 1, 0, 0) != -14) done(2);
  if (sc5(36, (long)unterminated, AT_FDCWD, (long)longlink, 0, 0) != -36) done(3);
  if (sc5(36, (long)target, AT_FDCWD, (long)link, 0, 0) != 0) done(4);
  if (sc5(36, (long)target, AT_FDCWD, (long)link, 0, 0) != -17) done(5);
  long got = sc5(78, AT_FDCWD, (long)link, (long)readback, sizeof readback, 0);
  if (got != (long)(sizeof target - 1)) done(6);
  for (long i = 0; i < got; ++i) if (readback[i] != target[i]) done(7);
  done(0);
}
