/* Linux AArch64 setrlimit: checked inputs and bounded host resource mapping. */
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
struct limit { unsigned long long cur, max; };
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_SETRLIMIT_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1);
  sc(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  struct limit core = {0};
  if (sc(163, 4, (long)&core, 0) != 0) done(1);
  if (sc(164, 4, 1, 0) != -14) done(2); /* EFAULT */
  if (sc(164, 4, (long)&core, 0) != 0) done(3); /* no-op */
  if (sc(164, 10, (long)&core, 0) != -95) done(4); /* unsupported */
  if (sc(164, 5, (long)&core, 0) != -95) done(5); /* RSS aliases AS */
  if (sc(164, 16, (long)&core, 0) != -22) done(6); /* invalid index */
  struct limit invalid = {2, 1};
  if (sc(164, 4, (long)&invalid, 0) != -22) done(7);
  struct limit too_large = {0x8000000000000000ULL, 0x8000000000000000ULL};
  if (sc(164, 4, (long)&too_large, 0) != -95) done(8);
  struct limit nofile = {4097, 0xffffffffffffffffULL};
  if (sc(164, 7, (long)&nofile, 0) != -95) done(9);
  struct limit after = {0};
  if (sc(163, 4, (long)&after, 0) != 0 ||
      after.cur != core.cur || after.max != core.max) done(10);
  done(0);
}
