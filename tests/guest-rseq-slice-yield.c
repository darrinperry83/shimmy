static long call(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
  register long x0 __asm__("x0") = a0, x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2, x3 __asm__("x3") = a3;
  register long x4 __asm__("x4") = a4, x5 __asm__("x5") = a5;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2),
                   "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}

static __attribute__((noreturn)) void done(int status) {
  static const char ok[] = "GUEST_RSEQ_SLICE_YIELD_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  call(93, status, 0, 0, 0, 0, 0);
  for (;;) { }
}

void _start(void) {
  // Registration remains unavailable: no scheduler slice can be granted.
  if (call(293, 0, 0, 0, 0, 0, 0) != -38) done(1);
  if (call(471, 0, 0, 0, 0, 0, 0) != 0) done(2);
  if (call(471, -1, -1, -1, -1, -1, -1) != 0) done(3);
  if (call(471, 123, 456, 789, 1011, 1213, 1415) != 0) done(4);
  done(0);
}
