/* ARM64 raw ABI: prctl name and PDEATHSIG pointers must stay guest-checked. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
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
  static const char ok[] = "PRCTL_MEMORY_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static const char name[] __attribute__((section(".text"))) = "shimmy";
static const char name15[15] __attribute__((section(".text"))) =
    {'f','i','f','t','e','e','n','b','y','t','e','n','a','m','e'};
static char out[16];
void _start(void) {
  if (sc3(167, 16, (long)out, 0) != 0 || out[0] != 'g') done(1);
  if (sc3(167, 15, 1, 0) != -14) done(2);
  if (sc3(167, 16, (long)out, 0) != 0 || out[0] != 'g') done(3);
  long p = sc6(222, 0, 32768, 3, 0x22, -1, 0);
  if (p < 0 || sc3(226, p + 16384, 16384, 0) != 0) done(4);
  *((char *)(p + 16383)) = 'x';
  if (sc3(167, 15, p + 16383, 0) != -14) done(5);
  if (sc3(167, 16, (long)out, 0) != 0 || out[0] != 'g') done(6);
  if (sc3(167, 15, (long)name, 0) != 0) done(7);
  if (sc3(167, 16, (long)out, 0) != 0 || out[0] != 's' || out[5] != 'y' || out[6])
    done(8);
  if (sc3(167, 16, (long)name, 0) != -14) done(9);
  if (sc3(167, 16, 1, 0) != -14) done(10);
  if (sc3(167, 2, (long)out, 0) != 0) done(11);
  if (sc3(167, 2, 1, 0) != -14) done(12);
  if (sc3(167, 2, (long)name, 0) != -14) done(13);
  if (sc3(167, 15, (long)name15, 0) != 0) done(14);
  if (sc3(167, 16, (long)out, 0) != 0 || out[14] != 'e' || out[15]) done(15);
  done(0);
}
