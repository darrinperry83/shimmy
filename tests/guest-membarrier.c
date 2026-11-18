// Raw ARM64 probe: never claim barriers that the shim cannot perform.
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_MEMBARRIER_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  if (sc3(283, 0, 0, 0) != 0) finish(1);   // QUERY: no supported commands
  if (sc3(283, 0, 1, 0) != -22) finish(2); // QUERY flags must be zero
  if (sc3(283, 1, 0, 0) != -22) finish(3); // GLOBAL unavailable
  if (sc3(283, 16, 0, 0) != -22) finish(4); // PRIVATE_EXPEDITED unavailable
  if (sc3(283, 32, 0, 0) != -22) finish(5); // REGISTER_PRIVATE unavailable
  if (sc3(283, -1, 0, 0) != -22) finish(6); // invalid command
  finish(0);
}
