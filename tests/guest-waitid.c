/* Raw ARM64 waitid ABI and child-state probe. */
static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_WAITID_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
static unsigned char info[128];
static int field(int offset) { return *(int *)(info + offset); }
void _start(void) {
  if (sc5(95, 4, 0, (long)info, 4, 0) != -22) finish(1); /* idtype */
  if (sc5(95, 1, 0, (long)info, 4, 0) != -22) finish(2); /* PID 0 */
  if (sc5(95, 0, 0, (long)info, 0, 0) != -22) finish(3); /* no event mask */
  if (sc5(95, 0, 0, (long)info, 0x80 | 4, 0) != -22) finish(4);
  if (sc5(95, 0, 0, (long)info, 4, 1) != -95) finish(5); /* rusage unsupported */
  if (sc5(95, 3, 999999, (long)info, 4, 0) != -9) finish(6); /* invalid pidfd */
  long child = sc5(220, 17, 0, 0, 0, 0); /* clone(SIGCHLD) */
  if (child == 0) {
    struct { long sec, nsec; } delay = {1, 0};
    sc5(101, (long)&delay, 0, 0, 0, 0);
    sc5(93, 42, 0, 0, 0, 0);
    for (;;) {}
  }
  if (child < 0) finish(7);
  if (sc5(95, 1, child, 1, 4, 0) != -14) finish(8); /* bad output before reaping */
  for (int i = 0; i < 128; ++i) info[i] = 0xa5;
  if (sc5(95, 1, child, (long)info, 4 | 1, 0)) finish(9); /* WNOHANG */
  if (field(16) != 0 || field(0) != 0) finish(10);
  if (sc5(95, 1, child, (long)info, 4 | 0x1000000, 0)) finish(11); /* WNOWAIT */
  if (field(0) != 17 || field(8) != 1 || field(16) != child || field(24) != 42)
    finish(12);
  if (sc5(95, 1, child, (long)info, 4, 0)) finish(13);
  if (field(16) != child || field(24) != 42) finish(14);
  finish(0);
}
