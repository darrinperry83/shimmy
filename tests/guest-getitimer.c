typedef long i64;
struct timeval { i64 sec, usec; };
struct itimerval { struct timeval interval, value; };

static long call(long nr, long a0, long a1, long a2) {
  register long x0 __asm__("x0") = a0, x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

static __attribute__((noreturn)) void done(int status) {
  static const char ok[] = "GUEST_GETITIMER_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1);
  call(93, status, 0, 0);
  for (;;) { }
}

void _start(void) {
  struct itimerval v;
  for (int which = 0; which < 3; ++which) {
    v.interval.sec = v.interval.usec = v.value.sec = v.value.usec = -1;
    if (call(102, which, (long)&v, 0) != 0) done(1 + which);
    if (v.interval.sec < 0 || v.interval.usec < 0 || v.interval.usec >= 1000000 ||
        v.value.sec < 0 || v.value.usec < 0 || v.value.usec >= 1000000)
      done(4 + which);
  }
  if (call(102, 0, 0, 0) != -14) done(7); // EFAULT
  if (call(102, 0, 1, 0) != -14) done(8);
  if (call(102, -1, 0, 0) != -22) done(9); // EINVAL before copyout
  if (call(102, 3, (long)&v, 0) != -22) done(10);
  done(0);
}
