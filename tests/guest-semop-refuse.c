/* AArch64 guest probe: semop must not touch a host SysV semaphore set. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_SEMOP_REFUSE_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  struct { unsigned short num; short op; short flags; } sop = {0, -1, 04000};
  /* Plausible IDs and invalid guest memory must all stop before Darwin semop. */
  if (sc3(193, 1, (long)&sop, 1) != -38) done(1);
  if (sc3(193, 12345, (long)&sop, 1) != -38) done(2);
  if (sc3(193, 1, 0, 1) != -38) done(3);
  if (sop.num != 0 || sop.op != -1 || sop.flags != 04000) done(4);
  done(0);
}
