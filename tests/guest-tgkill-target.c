static long call(long nr, long a0, long a1, long a2) {
  register long x0 __asm__("x0") = a0, x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

static __attribute__((noreturn)) void done(int status) {
  static const char ok[] = "GUEST_TGKILL_TARGET_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1);
  call(93, status, 0, 0);
  for (;;) { }
}

void _start(void) {
  long tgid = call(172, 0, 0, 0), tid = call(178, 0, 0, 0);
  if (tgid <= 0 || tid <= 0) done(1);
  if (call(131, tgid, tid, 0) != 0) done(2);
  if (call(131, 0, tid, 0) != -22) done(3);
  if (call(131, tgid, 0, 0) != -22) done(4);
  if (call(131, -1, tid, 0) != -22) done(5);
  if (call(131, tgid, -1, 0) != -22) done(6);
  if (call(131, tgid + 1, tid, 0) != -3) done(7);
  if (call(131, tgid, 0x7fffffff, 0) != -3) done(8);
  if (call(131, tgid, 0x7fffffff, 1) != -3) done(9);
  if (call(131, tgid, tid, 65) != -22) done(10);
  done(0);
}
