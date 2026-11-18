typedef unsigned long u64;

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
  static const char ok[] = "GUEST_MOVE_PAGES_QUERY_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  call(93, status, 0, 0, 0, 0, 0);
  for (;;) { }
}

void _start(void) {
  long page = call(222, 0, 16384, 3, 0x22, -1, 0);
  if (page < 0) done(1);
  *(volatile unsigned char *)page = 7;
  u64 pages[2] = {(u64)page, 0};
  int status[2] = {42, 42};
  if (call(239, 0, 2, (long)pages, 0, (long)status, 0) != 0) done(1);
  if (status[0] != 0) done(2);
  if (status[1] != -14) done(status[1] == 0 ? 15 : 16);
  if (call(239, 0, 1, 1, 0, (long)status, 0) != -14) done(3);
  if (call(239, 0, 1, (long)pages, 0, 1, 0) != -14) done(4);
  if (call(239, 0, 257, (long)pages, 0, (long)status, 0) != -95) done(5);
  if (call(239, 0, 1, (long)pages, (long)status, (long)status, 0) != -95) done(6);
  if (call(239, 0, 1, (long)pages, 0, (long)status, 4) != -22) done(7);
  done(0);
}
