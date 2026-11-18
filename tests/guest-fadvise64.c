// libc-free ARM64 guest probe for the bounded fadvise64 contract.
static long call(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2),
                   "r"(x3), "r"(x8) : "memory");
  return x0;
}

static __attribute__((noreturn)) void done(int status) {
  static const char ok[] = "GUEST_FADVISE64_NORMAL_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1, 0);
  call(93, status, 0, 0, 0);
  for (;;) {}
}

void _start(void) {
  static const char path[] = "/bin/probe";
  long fd = call(56, -100, (long)path, 0, 0);
  if (fd < 0) done(1);
  if (call(223, fd, 0, 0, 0) != 0) done(2);
  if (call(223, fd, 17, 512, 0) != 0) done(3);
  if (call(223, fd, -1, 0, 0) != -22 ||
      call(223, fd, 0, -1, 0) != -22) done(4);
  if (call(223, fd, 0, 0, 6) != -22) done(5);
  for (int advice = 1; advice <= 5; ++advice)
    if (call(223, fd, 0, 4096, advice) != -95) done(6);
  if (call(223, -1, 0, 0, 0) != -9) done(7);
  int pipefd[2] = {-1, -1};
  if (call(59, (long)pipefd, 0, 0, 0) != 0) done(8);
  if (call(223, pipefd[0], 0, 0, 0) != -29) done(9);
  if (call(223, pipefd[1], -1, 0, 6) != -29) done(10);
  if (call(57, fd, 0, 0, 0) != 0) done(11);
  if (call(223, fd, 0, 0, 0) != -9) done(12);
  done(0);
}
