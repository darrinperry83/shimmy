/* AArch64 raw ABI: write from stack, static data and executable ELF bytes. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "WRITE_MEMORY_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static char static_data[] = "D";
static const char executable_data[] __attribute__((section(".text"))) = "J";
static char oversized[1024 * 1024 + 1];
void _start(void) {
  char stack_data[] = "S";
  if (sc3(64, 1, (long)stack_data, 1) != 1) done(1);
  if (sc3(64, 1, (long)static_data, 1) != 1) done(2);
  if (sc3(64, 1, (long)executable_data, 1) != 1) done(3);
  if (sc3(64, 1, 1, 1) != -14) done(4);       /* no host write on bad memory */
  if (sc3(64, 1, 0, 1) != -14) done(5);
  if (sc3(64, 1, (long)oversized, sizeof oversized) != -95) done(6);
  if (sc3(64, 1, 1, 0) != 0) done(7);        /* zero length uses host fd path */
  if (sc3(64, -1, 1, 0) != -9) done(8);      /* zero length still checks fd */
  done(0);
}
