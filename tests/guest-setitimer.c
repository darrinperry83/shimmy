typedef long i64;
struct timeval { i64 sec, usec; };
struct itimerval { struct timeval interval, value; };

static long call(long nr, long a0, long a1, long a2) {
  register long x0 __asm__("x0") = a0, x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

static __attribute__((noreturn)) void done(int status) {
  static const char ok[] = "GUEST_SETITIMER_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1);
  call(93, status, 0, 0);
  for (;;) { }
}

void _start(void) {
  struct itimerval armed = {{0, 0}, {60, 0}}, old = {{-1, -1}, {-1, -1}};
  // A long virtual CPU timer cannot fire during this short probe.
  if (call(103, 1, (long)&armed, 0) != 0) done(1);
  if (call(103, 1, 0, (long)&old) != 0) done(2); // NULL disarms
  if (old.value.sec <= 0 || old.value.sec > 60 || old.value.usec < 0 ||
      old.value.usec >= 1000000) done(3);
  if (call(102, 1, (long)&old, 0) != 0 || old.value.sec != 0 ||
      old.value.usec != 0) done(4);

  armed.value.usec = 1000000;
  if (call(103, 1, (long)&armed, 0) != -22) done(5); // bad microseconds
  armed.value.usec = 0;
  armed.interval.sec = -1;
  if (call(103, 1, (long)&armed, 0) != -22) done(6); // negative seconds
  armed.interval.sec = 0;
  if (call(103, 1, 1, 0) != -14) done(7); // bad new-value address
  if (call(103, 9, (long)&armed, 0) != -22) done(8); // invalid selector
  if (call(103, 1, (long)&armed, 1) != -14) done(9); // bad old-value address
  if (call(103, 1, 0, 0) != 0) done(10); // disarm after copyout failure
  done(0);
}
