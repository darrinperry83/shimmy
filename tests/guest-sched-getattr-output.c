/* ARM64 raw ABI: sched_getattr stages its full declared guest output. */
static long sc6(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "SCHED_GETATTR_OUTPUT_OK\n";
  if (!code) sc6(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(93, code, 0, 0, 0, 0, 0);
  for (;;) {}
}
static const char readonly_out[] __attribute__((section(".text"))) = "R";
static unsigned char static_out[64];
void _start(void) {
  unsigned char stack_out[64];
  for (unsigned i = 0; i < 64; i++) static_out[i] = stack_out[i] = '?';
  if (sc6(275, 0, (long)static_out, 64, 0, 0, 0) != 0) done(1);
  if (*(unsigned *)(void *)static_out != 48 ||
      *(unsigned *)(void *)(static_out + 4) != 0) done(2);
  for (unsigned i = 48; i < 64; i++) if (static_out[i] != 0) done(3);
  if (sc6(275, 0, (long)stack_out, 48, 0, 0, 0) != 0 ||
      stack_out[48] != '?' || *(unsigned *)(void *)stack_out != 48) done(4);
  static_out[0] = '?';
  if (sc6(275, 0, 1, 48, 0, 0, 0) != -14) done(5);
  if (sc6(275, 0, (long)readonly_out, 48, 0, 0, 0) != -14 ||
      readonly_out[0] != 'R') done(6);
  long p = sc6(222, 0, 32768, 3, 0x22, -1, 0);
  if (p < 0 || sc6(226, p + 16384, 16384, 0, 0, 0, 0) != 0) done(7);
  ((char *)p)[16384 - 24] = '?';
  if (sc6(275, 0, p + 16384 - 24, 48, 0, 0, 0) != -14 ||
      ((char *)p)[16384 - 24] != '?') done(8);
  if (sc6(275, 0, (long)static_out, 47, 0, 0, 0) != -22 ||
      sc6(275, 0, (long)static_out, 4097, 0, 0, 0) != -22 ||
      sc6(275, 0, 0, 48, 0, 0, 0) != -22 ||
      sc6(275, 0, (long)static_out, 48, 1, 0, 0) != -22 ||
      static_out[0] != '?') done(9);
  if (sc6(275, 2000000000, (long)static_out, 48, 0, 0, 0) != -3) done(10);
  done(0);
}
