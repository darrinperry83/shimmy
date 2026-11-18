// Raw ARM64 guest probe for the per-range mlockall/munlockall subset.
// Linux mlockall = syscall 230, munlockall = 231.
static long call(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2),
                   "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int status) {
  static const char ok[] = "GUEST_MLOCKALL_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  call(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  // A mapped page so MCL_CURRENT has guest memory to lock beyond text/stack.
  long page = call(222, 0, 32768, 3, 0x22, -1, 0); // RW private anonymous mmap
  if (page < 0) done(1);
  *(volatile char*)page = 1;
  *(volatile char*)(page + 16384) = 2;

  if (call(230, 0x80000000L, 0, 0, 0, 0, 0) != -22) done(2); // unknown flag -> EINVAL
  if (call(230, 4, 0, 0, 0, 0, 0) != -95) done(3);           // MCL_ONFAULT -> EOPNOTSUPP
  if (call(230, 1 | 4, 0, 0, 0, 0, 0) != -95) done(4);        // combined ONFAULT
  if (call(230, 0, 0, 0, 0, 0, 0) != 0) done(5);              // zero flags: no-op
  if (call(230, 1, 0, 0, 0, 0, 0) != 0) done(6);              // MCL_CURRENT
  if (call(230, 1 | 2, 0, 0, 0, 0, 0) != 0) done(7);          // greetd: CURRENT|FUTURE
  if (call(230, 2, 0, 0, 0, 0, 0) != 0) done(8);              // MCL_FUTURE only

  // A mapping created under MCL_FUTURE must still map and be reachable; the
  // lock itself is a host-side pin with no guest query, so success is the
  // observable contract here.
  long p2 = call(222, 0, 16384, 3, 0x22, -1, 0);
  if (p2 < 0) done(9);
  *(volatile char*)p2 = 3;

  if (call(231, 0, 0, 0, 0, 0, 0) != 0) done(10);             // munlockall succeeds
  if (call(231, 0, 0, 0, 0, 0, 0) != 0) done(11);             // and is idempotent

  // Per-range lock/unlock still works after munlockall cleared the policy.
  if (call(228, p2, 16384, 0, 0, 0, 0) != 0) done(12);
  if (call(229, p2, 16384, 0, 0, 0, 0) != 0) done(13);

  if (call(215, page, 32768, 0, 0, 0, 0) != 0) done(14);
  if (call(215, p2, 16384, 0, 0, 0, 0) != 0) done(15);
  done(0);
}
