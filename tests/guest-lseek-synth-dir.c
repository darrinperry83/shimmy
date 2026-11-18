/* ARM64 raw ABI: lseek must reposition the synthetic getdents64 cursor. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "LSEEK_SYNTH_DIR_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static unsigned char first[4096], again[4096], tail[4096];
static int same(const unsigned char* a, const unsigned char* b, long n) {
  for (long i = 0; i < n; i++) if (a[i] != b[i]) return 0;
  return 1;
}
void _start(void) {
  static const char proc[] = "/proc";
  long dir = sc4(56, -100, (long)proc, 0x10000, 0); /* O_DIRECTORY */
  if (dir < 0) done(1);
  if (sc4(62, dir, 0, 1, 0) != 0) done(2); /* SEEK_CUR before entries */
  long n = sc4(61, dir, (long)first, sizeof first, 0);
  if (n < 48 || n > (long)sizeof first) done(3);
  unsigned short reclen = *(unsigned short*)(first + 16);
  long first_cookie = *(long*)(first + 8);
  if (reclen < 24 || reclen >= n || first_cookie != 2) done(4);
  if (sc4(62, dir, 0, 0, 0) != 0) done(5); /* SEEK_SET rewind */
  long again_n = sc4(61, dir, (long)again, sizeof again, 0);
  if (again_n != n || !same(first, again, n)) done(6);
  if (sc4(62, dir, first_cookie, 0, 0) != first_cookie) done(7);
  if (sc4(62, dir, 0, 1, 0) != first_cookie) done(8);
  long tail_n = sc4(61, dir, (long)tail, sizeof tail, 0);
  if (tail_n != n - reclen || !same(first + reclen, tail, tail_n)) done(9);
  if (sc4(62, dir, 1, 0, 0) != -22) done(10); /* no emitted cookie 1 */
  if (sc4(57, dir, 0, 0, 0) != 0) done(11);

  static const char path[] = "/regular";
  static const char abc[] = "ABC";
  long fd = sc4(56, -100, (long)path, 0x242, 0600);
  if (fd < 0) done(12);
  if (sc4(64, fd, (long)abc, 3, 0) != 3) done(13);
  if (sc4(62, fd, 1, 0, 0) != 1) done(14);
  char ch = 0;
  if (sc4(63, fd, (long)&ch, 1, 0) != 1 || ch != 'B') done(15);
  if (sc4(62, fd, 0, 1, 0) != 2) done(16);
  if (sc4(62, fd, 0, 2, 0) != 3) done(17);
  if (sc4(57, fd, 0, 0, 0) != 0) done(18);
  done(0);
}
