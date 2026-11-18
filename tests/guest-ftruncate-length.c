/* ARM64 ftruncate probe for negative length precedence and file effects. */
#define AT_FDCWD (-100L)
#define O_RDWR 2L
#define F_ADD_SEALS 1033L
#define F_SEAL_SHRINK 2L
#define MFD_ALLOW_SEALING 2L
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_FTRUNCATE_LENGTH_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char path[] = "/tmp/ftruncate-a";
  static const char memname[] = "sealed-ftruncate-test";
  long fd = sc4(56, AT_FDCWD, (long)path, O_RDWR, 0);
  if (fd < 0) done(1);
  if (sc4(46, fd, -1, 0, 0) != -22) done(2);
  if (sc4(46, -1, -1, 0, 0) != -22) done(3);
  if (sc4(46, fd, 3, 0, 0) != 0) done(4);
  if (sc4(46, fd, 7, 0, 0) != 0) done(5);
  if (sc4(46, -1, 7, 0, 0) != -9) done(6);
  long sealed = sc4(279, (long)memname, MFD_ALLOW_SEALING, 0, 0);
  if (sealed < 0) done(7);
  if (sc4(46, sealed, 16, 0, 0) != 0) done(8);
  if (sc4(25, sealed, F_ADD_SEALS, F_SEAL_SHRINK, 0) != 0) done(9);
  if (sc4(46, sealed, -1, 0, 0) != -22) done(10);
  if (sc4(46, sealed, 8, 0, 0) != -1) done(11);
  sc4(57, sealed, 0, 0, 0);
  sc4(57, fd, 0, 0, 0);
  done(0);
}
