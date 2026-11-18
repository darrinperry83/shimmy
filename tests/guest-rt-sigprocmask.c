/* Disposable raw ARM64 rt_sigprocmask ABI and state probe. */
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
  static const char ok[] = "GUEST_RT_SIGPROCMASK_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static unsigned long long old, current, mask;
static long sigmask(int how, const unsigned long long *set, unsigned long long *out, long size) {
  return sc4(135, how, (long)set, (long)out, size);
}
static unsigned long long readmask(void) {
  current = ~0ULL;
  if (sigmask(0, 0, &current, 8)) finish(90);
  return current;
}
void _start(void) {
  if (readmask() != 0) finish(1);
  old = ~0ULL;
  if (sigmask(0, 0, &old, 0) != -22 || old != ~0ULL) finish(2);
  if (sigmask(0, 0, &old, 7) != -22 || old != ~0ULL) finish(3);
  if (sigmask(0, 0, &old, 9) != -22 || old != ~0ULL) finish(4);
  mask = 1ULL << (10 - 1); /* Linux SIGUSR1 */
  if (sigmask(0, (void *)1, &old, 8) != -14 || readmask()) finish(5);
  if (sigmask(0, &mask, (void *)1, 8) != -14 || readmask()) finish(6);
  if (sigmask(3, &mask, &old, 8) != -22 || readmask()) finish(7);
  mask |= 1ULL << (9 - 1);  /* SIGKILL cannot be blocked. */
  mask |= 1ULL << (19 - 1); /* SIGSTOP cannot be blocked. */
  old = ~0ULL;
  if (sigmask(0, &mask, &old, 8) || old != 0 ||
      readmask() != (1ULL << (10 - 1))) finish(8);
  mask = 1ULL << (12 - 1); /* Linux SIGUSR2 */
  old = ~0ULL;
  if (sigmask(2, &mask, &old, 8) || old != (1ULL << (10 - 1)) ||
      readmask() != mask) finish(9);
  old = ~0ULL;
  if (sigmask(1, &mask, &old, 8) || old != mask || readmask()) finish(10);
  finish(0);
}
