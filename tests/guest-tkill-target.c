static long call(long nr, long a0, long a1, long a2) {
  register long x0 __asm__("x0") = a0, x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

static __attribute__((noreturn)) void done(int status) {
  static const char ok[] = "GUEST_TKILL_TARGET_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1);
  call(93, status, 0, 0);
  for (;;) { }
}

void _start(void) {
  long tid = call(178, 0, 0, 0);
  if (tid <= 0) done(1);
  if (call(130, tid, 0, 0) != 0) done(2);
  if (call(130, 0, 0, 0) != -22) done(3);
  if (call(130, -1, 0, 0) != -22) done(4);
  if (call(130, 0x7fffffff, 0, 0) != -3) done(5);
  if (call(130, 0x7fffffff, 1, 0) != -3) done(6);
  if (call(130, tid, 65, 0) != -22) done(7);
  done(0);
}
