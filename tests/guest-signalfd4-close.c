/* ARM64 raw ABI: guest signalfd close retires only its owned descriptor. */
static long sc6(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e;
  register long x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_SIGNALFD4_CLOSE_OK\n";
  if (!code) sc6(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(93, code, 0, 0, 0, 0, 0);
  for (;;) {}
}
struct epoll_event { unsigned events, pad; unsigned long data; };
static unsigned long long usr2 = 1ULL << (12 - 1);
void _start(void) {
  long epfd = sc6(20, 0, 0, 0, 0, 0, 0);
  if (epfd < 0) finish(1);
  long fd = sc6(74, -1, (long)&usr2, 8, 0x80800, 0, 0);
  if (fd < 0) finish(2);
  struct epoll_event ev = {1, 0, 0x74};
  if (sc6(21, epfd, 1, fd, (long)&ev, 0, 0) != 0) finish(3);
  if (sc6(57, fd, 0, 0, 0, 0, 0) != 0) finish(4);
  if (sc6(74, fd, (long)&usr2, 8, 0, 0, 0) != -9) finish(5);
  char record[128];
  if (sc6(63, fd, (long)record, sizeof record, 0, 0, 0) != -9) finish(6);
  // The close route must also remove the epoll registration. An absent entry
  // returns ENOENT rather than leaving a stale watcher on a recycled fd.
  if (sc6(21, epfd, 2, fd, 0, 0, 0) != -2) finish(7);
  for (int i = 0; i < 20; i++) {
    fd = sc6(74, -1, (long)&usr2, 8, 0x80800, 0, 0);
    if (fd < 0) finish(8);
    if (sc6(57, fd, 0, 0, 0, 0, 0) != 0) finish(9);
  }
  if (sc6(57, epfd, 0, 0, 0, 0, 0) != 0) finish(10);
  finish(0);
}
