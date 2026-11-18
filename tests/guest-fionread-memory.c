/* ARM64 raw ABI: ioctl(FIONREAD) writes through checked guest memory. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "FIONREAD_MEMORY_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static const int read_only_result __attribute__((section(".text"))) = 0;
static const char data[] __attribute__((section(".text"))) = "abc";
void _start(void) {
  int fds[2];
  if (sc3(59, (long)fds, 0, 0) != 0) done(1); // pipe2
  if (sc3(64, fds[1], (long)data, 3) != 3) done(2);
  if (sc3(29, fds[0], 0x541b, 1) != -14) done(3);
  if (sc3(29, fds[0], 0x541b, (long)&read_only_result) != -14) done(4);
  int available = -1;
  if (sc3(29, fds[0], 0x541b, (long)&available) != 0 || available != 3) done(5);
  char out[3];
  if (sc3(63, fds[0], (long)out, 2) != 2) done(6);
  if (sc3(29, fds[0], 0x541b, (long)&available) != 0 || available != 1) done(7);
  if (sc3(57, fds[0], 0, 0) != 0 || sc3(57, fds[1], 0, 0) != 0) done(8);
  done(0);
}
