/* ARM64 raw ABI: fsetxattr stages name and full value before fd mutation. */
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
  static const char ok[] = "FSETXATTR_MEMORY_OK\n";
  if (!code) sc6(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(93, code, 0, 0, 0, 0, 0);
  for (;;) {}
}
static const char path[] __attribute__((section(".text"))) = "/tmp/data";
static const char name[] __attribute__((section(".text"))) = "user.shimmy-test";
static const char next[] __attribute__((section(".text"))) = "new!";
static char out[8];
#define PUT(n, v, len, flags) sc6(7, fd, (n), (v), (len), (flags), 0)
#define GET() sc6(10, fd, (long)name, (long)out, sizeof out, 0, 0)
void _start(void) {
  long fd = sc6(56, -100, (long)path, 2, 0, 0, 0);
  if (fd < 0) done(1);
  if (GET() != 3 || out[0] != 'a') done(2);
  if (PUT((long)name, (long)next, 4, 1) != -17) done(3); // CREATE existing
  if (PUT(1, (long)next, 4, 2) != -14) done(4);
  if (PUT((long)name, 1, 4, 2) != -14) done(5);
  if (PUT((long)name, (long)next, 4, 4) != -22) done(6);
  if (PUT((long)name, (long)next, 4, 3) != -22) done(7);
  if (PUT((long)name, (long)next, 65537, 2) != -7) done(8);
  if (GET() != 3 || out[0] != 'a') done(9);
  long p = sc6(222, 0, 32768, 3, 0x22, -1, 0);
  if (p < 0 || sc6(226, p + 16384, 16384, 0, 0, 0, 0) != 0) done(10);
  *((char *)(p + 16383)) = 'x';
  if (PUT((long)name, p + 16383, 2, 2) != -14) done(11);
  if (GET() != 3 || out[0] != 'a') done(12);
  if (PUT((long)name, (long)next, 4, 2) != 0) done(13);
  if (GET() != 4 || out[0] != 'n' || out[3] != '!') done(14);
  if (sc6(7, -1, (long)name, (long)next, 4, 2, 0) != -9) done(15);
  done(0);
}
