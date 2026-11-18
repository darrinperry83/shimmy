/* ARM64 raw ABI: host-identity saved-GID refusal must not change credentials. */
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_SETRESGID_SAVED_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1);
  sc(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  unsigned before[3], after[3];
  if (sc(150, (long)&before[0], (long)&before[1], (long)&before[2])) done(1);
  unsigned different_saved = before[2] ^ 1U;
  if (sc(149, -1, -1, different_saved) != -95) done(2);
  if (sc(150, (long)&after[0], (long)&after[1], (long)&after[2])) done(3);
  if (before[0] != after[0] || before[1] != after[1] || before[2] != after[2]) done(4);
  /* Even a request combining real/effective changes must be refused first. */
  if (sc(149, before[0] ^ 1U, before[1] ^ 1U, different_saved) != -95) done(5);
  if (sc(150, (long)&after[0], (long)&after[1], (long)&after[2])) done(6);
  if (before[0] != after[0] || before[1] != after[1] || before[2] != after[2]) done(7);
  if (sc(149, -1, -1, -1)) done(8);
  if (sc(150, (long)&after[0], (long)&after[1], (long)&after[2])) done(9);
  if (before[0] != after[0] || before[1] != after[1] || before[2] != after[2]) done(10);
  done(0);
}
