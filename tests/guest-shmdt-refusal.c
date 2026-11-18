/* ARM64 raw syscall probe: shmdt must not detach any host mapping. */
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
__attribute__((noreturn)) static void done(int code) {
  static const char ok[] = "SHMDT_REFUSAL_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1);
  sc(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  static volatile long marker = 0x5a17;
  if (sc(197, 0, 0, 0) != -38) done(1);
  if (sc(197, (long)&marker, 0, 0) != -38) done(2);
  if (marker != 0x5a17) done(3);
  done(0);
}
