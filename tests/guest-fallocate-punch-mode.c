/* Raw AArch64 fallocate mode validation against a disposable regular file. */
static long sc6(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "FALLOCATE_PUNCH_MODE_OK\n";
  if (!code) sc6(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(93, code, 0, 0, 0, 0, 0);
  for (;;) {}
}
static const char path[] = "/tmp/data";
static char buf[8];
static int bytes(long fd, const char *expect) {
  if (sc6(67, fd, (long)buf, 8, 0, 0, 0) != 8) return 0;
  for (int i = 0; i < 8; i++) if (buf[i] != expect[i]) return 0;
  return sc6(62, fd, 0, 2, 0, 0, 0) == 8;
}
void _start(void) {
  long fd = sc6(56, -100, (long)path, 2, 0, 0, 0);
  if (fd < 0 || !bytes(fd, "ABCDEFGH")) done(1);
  if (sc6(47, fd, 2, 2, 2, 0, 0) != -95 || !bytes(fd, "ABCDEFGH")) done(2);
  if (sc6(47, fd, 2 | 1 | 0x2000, 2, 2, 0, 0) != -95 ||
      !bytes(fd, "ABCDEFGH")) done(3);
  if (sc6(47, fd, 0x10 | 1, 2, 2, 0, 0) != 0) done(4);
  if (!bytes(fd, "AB\0\0EFGH")) done(5);
  done(0);
}
