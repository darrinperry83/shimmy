/* ARM64 raw ABI: unsupported saved UID must not mutate host credentials. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_SETRESUID_SAVED_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static unsigned before[3], after[3];
void _start(void) {
  if (sc3(148, (long)&before[0], (long)&before[1], (long)&before[2])) finish(1);
  if (sc3(147, -1, -1, (long)(before[2] ^ 1U)) != -95) finish(2);
  if (sc3(148, (long)&after[0], (long)&after[1], (long)&after[2])) finish(3);
  if (before[0] != after[0] || before[1] != after[1] || before[2] != after[2]) finish(4);
  if (sc3(147, -1, -1, -1)) finish(5);
  if (sc3(148, (long)&after[0], (long)&after[1], (long)&after[2])) finish(6);
  if (before[0] != after[0] || before[1] != after[1] || before[2] != after[2]) finish(7);
  finish(0);
}
