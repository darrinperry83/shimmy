typedef long i64;
struct timespec { i64 sec, nsec; };
struct itimerspec { struct timespec interval, value; };

static long call(long nr, long a0, long a1, long a2, long a3) {
  register long x0 __asm__("x0") = a0, x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2, x3 __asm__("x3") = a3;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2),
                   "r"(x3), "r"(x8) : "memory");
  return x0;
}

static __attribute__((noreturn)) void done(int status) {
  static const char ok[] = "GUEST_TIMERFD_SETTIME_GUEST_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1, 0);
  call(93, status, 0, 0, 0);
  for (;;) { }
}

void _start(void) {
  long fd = call(85, 1, 0, 0, 0); // CLOCK_MONOTONIC
  if (fd < 0) done(1);
  struct itimerspec armed = {{0, 0}, {60, 0}}, old = {{-1, -1}, {-1, -1}};
  if (call(86, fd, 0, (long)&armed, (long)&old) != 0) done(2);
  if (old.interval.sec || old.interval.nsec || old.value.sec || old.value.nsec)
    done(3);
  if (call(86, fd, 0, 0, 0) != -14) done(4); // missing new value
  if (call(86, fd, 0, 1, 0) != -14) done(5); // bad new address
  struct itimerspec current;
  if (call(87, fd, (long)&current, 0, 0) != 0 ||
      current.value.sec <= 0 || current.value.sec > 60) done(6);
  struct itimerspec zero = {{0, 0}, {0, 0}};
  if (call(86, fd, 0, (long)&zero, 1) != -14) done(7); // post-update EFAULT
  if (call(87, fd, (long)&current, 0, 0) != 0 ||
      current.value.sec || current.value.nsec) done(8);
  if (call(86, fd, 4, (long)&zero, 0) != -22) done(9);
  if (call(86, -1, 0, (long)&zero, 0) != -9) done(10);
  done(0);
}
