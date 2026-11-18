/* Native AArch64 epoll_create1 flags and CLOEXEC probe. */
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "EPOLL_CREATE_FLAGS_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1);
  sc(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  if (sc(20, 1, 0, 0) != -22) done(1);
  if (sc(20, 0x80001, 0, 0) != -22) done(2);
  if (sc(20, -1, 0, 0) != -22) done(3);
  long plain = sc(20, 0, 0, 0);
  if (plain < 0) done(4);
  if (sc(25, plain, 1, 0) != 0) done(5); /* F_GETFD */
  if (sc(57, plain, 0, 0) != 0) done(6);
  long cloexec = sc(20, 0x80000, 0, 0);
  if (cloexec < 0) done(7);
  if (sc(25, cloexec, 1, 0) != 1) done(8);
  if (sc(57, cloexec, 0, 0) != 0) done(9);
  done(0);
}
