/* ARM64 raw ABI: pwritev2 snapshots guest vectors before any host write. */
struct iov { const void *base; unsigned long len; };
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
  static const char ok[] = "PWRITEV2_MEMORY_OK\n";
  if (!code) sc6(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(93, code, 0, 0, 0, 0, 0);
  for (;;) {}
}
static const char path[] = "/tmp/data";
static char got[4];
#define WRITEV2(vec, n, pos, high, flags) sc6(287, fd, (vec), (n), (pos), (high), (flags))
static int contents(long fd, char a, char b, char c) {
  return sc6(67, fd, (long)got, 3, 0, 0, 0) == 3 &&
         got[0] == a && got[1] == b && got[2] == c;
}
void _start(void) {
  long fd = sc6(56, -100, (long)path, 2, 0, 0, 0);
  if (fd < 0 || !contents(fd, 'a', 'b', 'c')) done(1);
  struct iov bad = {(void *)1, 1};
  if (WRITEV2(1, 1, 0, 0, 0) != -14 || !contents(fd, 'a', 'b', 'c')) done(2);
  if (WRITEV2((long)&bad, 1, 0, 0, 0) != -14 || !contents(fd, 'a', 'b', 'c')) done(3);
  if (WRITEV2((long)&bad, 1, -1, 17, 0) != -14 ||
      sc6(62, fd, 0, 1, 0, 0, 0) != 0 || !contents(fd, 'a', 'b', 'c')) done(4);
  char payload = 'Z';
  struct iov good = {&payload, 1};
  if (WRITEV2((long)&good, 1, 1, 17, 0) != 1 ||
      sc6(62, fd, 0, 1, 0, 0, 0) != 0 || !contents(fd, 'a', 'Z', 'c')) done(5);
  if (WRITEV2((long)&good, 1, -1, 17, 0) != 1 ||
      sc6(62, fd, 0, 1, 0, 0, 0) != 1 || !contents(fd, 'Z', 'Z', 'c')) done(6);
  if (WRITEV2((long)&good, 1, 0, 0, 4) != -95 || !contents(fd, 'Z', 'Z', 'c')) done(7);
  done(0);
}
