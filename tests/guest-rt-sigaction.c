/* Disposable raw ARM64 rt_sigaction memory and disposition probe. */
typedef unsigned long long u64;
struct action { u64 handler, flags, restorer, mask; };
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_RT_SIGACTION_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static long sigaction(int sig, const struct action *act, struct action *old, long size) {
  return sc4(134, sig, (long)act, (long)old, size);
}
static struct action next, old, current, both;
void _start(void) {
  old.handler = ~0ULL;
  if (sigaction(10, 0, &old, 8) || old.handler != 0) finish(1);
  old.handler = ~0ULL;
  if (sigaction(10, 0, &old, 0) != -22 || old.handler != ~0ULL) finish(2);
  if (sigaction(10, 0, &old, 7) != -22 || old.handler != ~0ULL) finish(3);
  if (sigaction(10, 0, &old, 9) != -22 || old.handler != ~0ULL) finish(4);
  if (sigaction(0, 0, &old, 8) != -22 || sigaction(65, 0, &old, 8) != -22) finish(5);
  if (sigaction(10, (void *)1, &old, 8) != -14) finish(6);
  next.handler = 1; /* SIG_IGN, without requiring a guest signal frame. */
  next.flags = 0x1234;
  next.mask = 0x100;
  if (sigaction(10, &next, (void *)1, 8) != -14) finish(7);
  if (sigaction(10, 0, &current, 8) || current.handler != 0) finish(8);
  old.handler = ~0ULL;
  if (sigaction(10, &next, &old, 8) || old.handler != 0) finish(9);
  if (sigaction(10, 0, &current, 8) || current.handler != 1 ||
      current.flags != next.flags || current.mask != next.mask) finish(10);
  both.handler = 0;
  both.flags = 9;
  if (sigaction(10, &both, &both, 8) || both.handler != 1) finish(11);
  if (sigaction(10, 0, &current, 8) || current.handler != 0 || current.flags != 9) finish(12);
  finish(0);
}
