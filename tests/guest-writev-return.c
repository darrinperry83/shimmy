/* AArch64 raw syscall probe: writev returns bytes written with tracing too. */
struct iovec { void* base; unsigned long len; };
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "WRITEV_RETURN_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  char first[] = "ab", second[] = "cde";
  struct iovec parts[2] = {{first, 2}, {second, 3}};
  if (sc3(66, 1, (long)parts, 2) != 5) done(1);
  done(0);
}
