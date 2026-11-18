// libc-free ARM64 guest probe for Linux sched_get_priority_min(2).
static long call(long nr, long arg) {
  register long x0 __asm__("x0") = arg;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}

static __attribute__((noreturn)) void done(int status) {
  static const char ok[] = "GUEST_SCHED_PRIORITY_MIN_OK\n";
  if (!status) {
    register long x0 __asm__("x0") = 1;
    register long x1 __asm__("x1") = (long)ok;
    register long x2 __asm__("x2") = sizeof ok - 1;
    register long x8 __asm__("x8") = 64;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  }
  call(93, status);
  for (;;) {}
}

void _start(void) {
  static const long normal[] = {0, 3, 5, 6, 7};
  for (unsigned i = 0; i < sizeof normal / sizeof normal[0]; ++i)
    if (call(126, normal[i]) != 0) done(1);
  if (call(126, 1) != 1 || call(126, 2) != 1) done(2);
  if (call(126, 4) != -22 || call(126, 8) != -22 ||
      call(126, -1) != -22 || call(126, 0x40000000) != -22)
    done(3);
  // The syscall argument is int policy, so high register bits are ignored.
  if (call(126, 0x100000001L) != 1) done(4);
  done(0);
}
