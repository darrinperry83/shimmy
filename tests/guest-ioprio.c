/* AArch64 guest probe for ioprio_set/ioprio_get (syscall 30/31). */
static long sc(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "IOPRIO_GUEST_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc(93, code, 0, 0, 0, 0);
  for (;;) {}
}
#define IOPRIO_CLASS_RT    1
#define IOPRIO_CLASS_BE    2
#define IOPRIO_CLASS_IDLE  3
#define PRIO(c, d) ((c) << 13 | (d))
void _start(void) {
  long pid = sc(172, 0, 0, 0, 0, 0);   /* getpid */
  /* BE and IDLE at any priority are accepted no-ops (BE priority is a nice hint). */
  if (sc(30, 1, pid, PRIO(IOPRIO_CLASS_BE, 0), 0, 0) != 0) done(1);
  if (sc(30, 1, pid, PRIO(IOPRIO_CLASS_IDLE, 0), 0, 0) != 0) done(2);
  if (sc(30, 1, pid, PRIO(IOPRIO_CLASS_BE, 7), 0, 0) != 0) done(6);
  /* RT is a real-time scheduling promise the shim cannot keep. */
  if (sc(30, 1, pid, PRIO(IOPRIO_CLASS_RT, 0), 0, 0) != -95) done(3);  /* -EOPNOTSUPP */
  /* get returns the default (0). */
  if (sc(31, 1, pid, 0, 0, 0) != 0) done(5);
  done(0);
}
