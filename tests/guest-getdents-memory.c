/* ARM64 raw ABI: getdents64 must not advance a cursor for bad guest output. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GETDENTS_MEMORY_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static char static_out[512];
static const char readonly_text[] __attribute__((section(".text"))) = "R";
static int same(const char* a, const char* b, long n) {
  for (long i = 0; i < n; i++) if (a[i] != b[i]) return 0;
  return 1;
}
static void check_pair(const char* path, int base) {
  long fd = sc4(56, -100, (long)path, 0, 0);
  long control = sc4(56, -100, (long)path, 0, 0);
  if (fd < 0 || control < 0) done(base + 1);
  if (sc4(61, fd, 1, 256, 0) != -14) done(base + 2);
  if (sc4(61, fd, (long)readonly_text, 256, 0) != -14) done(base + 3);
  if (sc4(61, fd, (long)static_out, 1, 0) != -22) done(base + 4);
  char stack_out[512];
  long got = sc4(61, fd, (long)stack_out, sizeof stack_out, 0);
  long expected = sc4(61, control, (long)static_out, sizeof static_out, 0);
  if (got <= 0 || got != expected || !same(stack_out, static_out, got)) done(base + 5);
  if (sc4(61, fd, (long)stack_out, sizeof stack_out, 0) < 0) done(base + 6);
  sc4(57, fd, 0, 0, 0);
  sc4(57, control, 0, 0, 0);
}
void _start(void) {
  static const char real[] = "/tmp/testdir";
  static const char synthetic[] = "/proc";
  check_pair(real, 0);
  check_pair(synthetic, 10);
  if (sc4(61, -1, (long)static_out, sizeof static_out, 0) != -9) done(21);
  done(0);
}
