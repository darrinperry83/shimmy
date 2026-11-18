typedef long i64;

static long call(long nr, long a0, long a1, long a2) {
  register long x0 __asm__("x0") = a0, x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

static __attribute__((noreturn)) void done(int status) {
  static const char ok[] = "GUEST_TIMERFD_CREATE_REUSE_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1);
  call(93, status, 0, 0);
  for (;;) { }
}

void _start(void) {
  if (call(85, 99, 0, 0) != -22) done(1); // invalid clock
  if (call(85, 1, 1, 0) != -22) done(2);  // invalid flags
  long fd[32];
  for (int i = 0; i < 32; ++i) {
    fd[i] = call(85, 1, 0, 0);
    if (fd[i] < 0) done(3);
  }
  if (call(85, 1, 0, 0) != -24) done(4); // live capacity reached
  if (call(57, fd[0], 0, 0) != 0) done(5);
  fd[0] = call(85, 1, 0, 0);
  if (fd[0] < 0) done(6);                 // freed slot still works
  for (int i = 0; i < 32; ++i)
    if (call(57, fd[i], 0, 0) != 0) done(7);
  for (int i = 0; i < 40; ++i) {
    long next = call(85, 1, 0, 0);
    if (next < 0 || call(57, next, 0, 0) != 0) done(8);
  }
  done(0);
}
