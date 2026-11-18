/* Disposable ARM64 truncate probe for checked path and length behavior. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_TRUNCATE_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char path[] = "/tmp/truncate-a";
  static const char missing[] = "/tmp/missing";
  static char unterminated[4096];
  for (int i = 0; i < 4096; ++i) unterminated[i] = 'x';
  if (sc3(45, 1, 3, 0) != -14) done(1);
  if (sc3(45, (long)path, -1, 0) != -22) done(2);
  if (sc3(45, (long)unterminated, 3, 0) != -36) done(3);
  if (sc3(45, (long)missing, 3, 0) != -2) done(4);
  if (sc3(45, (long)path, 3, 0) != 0) done(5);
  if (sc3(45, (long)path, 7, 0) != 0) done(6);
  done(0);
}
