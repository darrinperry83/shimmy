/* ARM64 getcpu: current CPU must be in the guest's advertised affinity set. */
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_GETCPU_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1);
  sc(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  unsigned cpu = 999, node = 999;
  unsigned long mask = 0;
  if (sc(123, 0, 8, (long)&mask) != 8) done(1);
  if (sc(168, (long)&cpu, (long)&node, 0) != 0) done(2);
  if (cpu >= 32 || !(mask & (1UL << cpu)) || node != 0) done(3);
  if (sc(168, 0, 0, 0) != 0) done(4);
  node = 999;
  if (sc(168, 0, (long)&node, 0) != 0 || node != 0) done(5);
  if (sc(168, 1, 0, 0) != -14) done(6);
  if (sc(168, 0, 1, 0) != -14) done(7);
  if (sc(168, 0, 0, 1) != 0) done(8); // legacy cache pointer is ignored
  done(0);
}
