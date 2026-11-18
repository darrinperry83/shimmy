/* ARM64 renameat2: checked paths and Linux flag combinations. */
#define AT_FDCWD -100L
static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_RENAMEAT2_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char a[] = "/tmp/a", b[] = "/tmp/b";
  static const char c[] = "/tmp/c", missing[] = "/tmp/missing";
  if (sc5(276, AT_FDCWD, 1, AT_FDCWD, (long)b, 0) != -14) done(1);
  if (sc5(276, AT_FDCWD, (long)a, AT_FDCWD, 1, 0) != -14) done(2);
  if (sc5(276, AT_FDCWD, 1, AT_FDCWD, (long)b, 0x4000) != -22) done(3);
  if (sc5(276, AT_FDCWD, (long)a, AT_FDCWD, (long)b, 3) != -22) done(4);
  if (sc5(276, AT_FDCWD, (long)a, AT_FDCWD, (long)b, 4) != -22) done(5);
  if (sc5(276, AT_FDCWD, (long)a, AT_FDCWD, (long)b, 1) != -17) done(6);
  if (sc5(276, AT_FDCWD, (long)a, AT_FDCWD, (long)b, 2) != 0) done(7);
  if (sc5(276, AT_FDCWD, (long)b, AT_FDCWD, (long)missing, 2) != -2) done(8);
  if (sc5(276, AT_FDCWD, (long)a, AT_FDCWD, (long)c, 0) != 0) done(9);
  done(0);
}
