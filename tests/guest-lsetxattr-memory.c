/* ARM64 raw ABI: lsetxattr stages all guest input before link mutation. */
static long sc6(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "LSETXATTR_MEMORY_OK\n";
  if (!code) sc6(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(93, code, 0, 0, 0, 0, 0);
  for (;;) {}
}
static const char path[] __attribute__((section(".text"))) = "/tmp/link";
static const char name[] __attribute__((section(".text"))) = "user.shimmy-test";
static const char first[] __attribute__((section(".text"))) = "link";
static const char second[] __attribute__((section(".text"))) = "new!";
static char out[8];
#define GET_LINK() sc4(9, (long)path, (long)name, (long)out, sizeof out)
#define GET_TARGET() sc4(8, (long)path, (long)name, (long)out, sizeof out)
#define PUT(value, size, flags) sc6(6, (long)path, (long)name, (value), (size), (flags), 0)
void _start(void) {
  if (GET_LINK() != 4) done(1);
  if (out[0] != 'l') done(21);
  if (PUT((long)second, 4, 1) != -17) done(2); // CREATE on existing
  if (PUT(1, 4, 2) != -14) done(3);
  if (sc6(6, 1, (long)name, (long)second, 4, 2, 0) != -14) done(4);
  if (sc6(6, (long)path, 1, (long)second, 4, 2, 0) != -14) done(5);
  if (PUT((long)second, 4, 4) != -22) done(6);
  if (PUT((long)second, 4, 3) != -22) done(7);
  if (PUT((long)second, 65537, 2) != -7) done(8);
  if (GET_LINK() != 4 || out[0] != 'l') done(9);
  long p = sc6(222, 0, 32768, 3, 0x22, -1, 0);
  if (p < 0 || sc6(226, p + 16384, 16384, 0, 0, 0, 0) != 0) done(10);
  *((char *)(p + 16383)) = 'x';
  if (PUT(p + 16383, 2, 2) != -14) done(11); // later page protected
  if (GET_LINK() != 4 || out[0] != 'l') done(12);
  if (PUT((long)second, 4, 2) != 0) done(13);
  if (GET_LINK() != 4 || out[0] != 'n') done(14);
  if (GET_TARGET() != 6 || out[0] != 't') done(15); // target unchanged
  if (PUT((long)first, 4, 0) != 0 || GET_LINK() != 4 || out[0] != 'l') done(16);
  done(0);
}
