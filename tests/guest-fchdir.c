/* ARM64 raw ABI: fchdir updates guest cwd and relative path resolution. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_FCHDIR_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static int eq(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}
void _start(void) {
  static const char a[] = "/a", b[] = "/b", marker[] = "marker";
  char cwd[64];
  long dir = sc4(56, -100, (long)a, 65536, 0); /* O_DIRECTORY */
  if (dir < 0) finish(1);
  if (sc4(49, (long)b, 0, 0, 0)) finish(2);
  if (sc4(17, (long)cwd, sizeof cwd, 0, 0) < 0 || !eq(cwd, b)) finish(3);
  if (sc4(50, dir, 0, 0, 0)) finish(4);
  if (sc4(17, (long)cwd, sizeof cwd, 0, 0) < 0 || !eq(cwd, a)) finish(5);
  long file = sc4(56, -100, (long)marker, 0, 0);
  if (file < 0) finish(6);
  if (sc4(50, file, 0, 0, 0) != -20) finish(7); /* ENOTDIR */
  if (sc4(50, -1, 0, 0, 0) != -9) finish(8); /* EBADF */
  if (sc4(17, (long)cwd, sizeof cwd, 0, 0) < 0 || !eq(cwd, a)) finish(9);
#ifdef OUTSIDE_FD
  if (sc4(50, OUTSIDE_FD, 0, 0, 0) >= 0) finish(11);
  if (sc4(17, (long)cwd, sizeof cwd, 0, 0) < 0 || !eq(cwd, a)) finish(12);
#endif
  if (sc4(57, file, 0, 0, 0) || sc4(57, dir, 0, 0, 0)) finish(10);
  finish(0);
}
