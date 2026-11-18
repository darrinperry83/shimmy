/* ARM64 raw ABI: unlinkat must check guest path before host mutation. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "UNLINKAT_PATH_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static const char file[] __attribute__((section(".text"))) = "/tmp/remove-file";
static const char dir[] __attribute__((section(".text"))) = "/tmp/remove-dir";
static char unterminated[4096];
void _start(void) {
  if (sc3(35, -100, 1, 0) != -14) done(1);
  if (sc3(35, -100, 0, 0x200) != -14) done(2);
  for (int i = 0; i < 4096; ++i) unterminated[i] = 'x';
  if (sc3(35, -100, (long)unterminated, 0) != -36) done(3);
  if (sc3(35, -100, (long)file, 0) != 0) done(4);
  if (sc3(35, -100, (long)dir, 0x200) != 0) done(5);
  done(0);
}
