/* ARM64 raw ABI: mknodat must snapshot guest path before host creation. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "MKNODAT_PATH_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static const char regular[] __attribute__((section(".text"))) = "/tmp/regular";
static const char fifo[] __attribute__((section(".text"))) = "/tmp/fifo";
static char unterminated[4096];
void _start(void) {
  if (sc4(33, -100, 1, 0x8000 | 0600, 0) != -14) done(1);
  if (sc4(33, -100, 0, 0x8000 | 0600, 0) != -14) done(2);
  for (int i = 0; i < 4096; ++i) unterminated[i] = 'x';
  if (sc4(33, -100, (long)unterminated, 0x8000 | 0600, 0) != -36) done(3);
  if (sc4(33, -100, 1, 0xa000, 0) != -22) done(4); // invalid type precedence
  if (sc4(33, -100, (long)regular, 0x8000 | 0600, 0) != 0) done(5);
  if (sc4(33, -100, (long)fifo, 0x1000 | 0600, 0) != 0) done(6);
  if (sc4(33, -100, (long)regular, 0x8000 | 0600, 0) != -17) done(7);
  done(0);
}
