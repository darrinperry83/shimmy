/* Raw ARM64 rt_sigpending ABI and checked-output probe. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_RT_SIGPENDING_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static unsigned long long pending = ~0ULL;
void _start(void) {
  if (sc3(136, (long)&pending, 7, 0) != -22 || pending != ~0ULL) finish(1);
  if (sc3(136, 0, 8, 0) != -14) finish(2);
  if (sc3(136, 1, 8, 0) != -14) finish(3);
  if (sc3(136, (long)&pending, 8, 0) || pending != 0) finish(4);
  finish(0);
}
