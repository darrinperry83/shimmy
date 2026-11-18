/* ARM64 raw ABI: pipe2 checks its guest output and preserves fd flags/data. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "PIPE2_CHECKED_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static const int readonly_fds[2] __attribute__((section(".text"))) = {-1, -1};
void _start(void) {
  int fds[2] = {-1, -1};
  if (sc4(59, (long)fds, 0, 0, 0) != 0) done(1);
  int first_read = fds[0], first_write = fds[1];
  if (first_read < 0 || first_write < 0 || first_read == first_write) done(2);
  if (sc4(57, first_read, 0, 0, 0) != 0 ||
      sc4(57, first_write, 0, 0, 0) != 0) done(3);
  for (int i = 0; i < 64; i++)
    if (sc4(59, 1, 0, 0, 0) != -14) done(4);
  if (sc4(59, 0, 0, 0, 0) != -14) done(5);
  if (sc4(59, (long)readonly_fds, 0, 0, 0) != -14) done(6);
  if (sc4(59, (long)fds, 0x4000, 0, 0) != -95) done(7); /* O_DIRECT */
  if (sc4(59, (long)fds, 0x40000000, 0, 0) != -22) done(8);
  if (sc4(59, (long)fds, 0x80800, 0, 0) != 0) done(9);
  if (fds[0] != first_read || fds[1] != first_write) done(10); /* no leaked fds */
  if (sc4(25, fds[0], 1, 0, 0) != 1 || sc4(25, fds[1], 1, 0, 0) != 1)
    done(11); /* FD_CLOEXEC */
  char c = 0;
  if (sc4(63, fds[0], (long)&c, 1, 0) != -11) done(12); /* O_NONBLOCK */
  c = 'P';
  if (sc4(64, fds[1], (long)&c, 1, 0) != 1) done(13);
  c = 0;
  if (sc4(63, fds[0], (long)&c, 1, 0) != 1 || c != 'P') done(14);
  if (sc4(57, fds[0], 0, 0, 0) != 0 ||
      sc4(57, fds[1], 0, 0, 0) != 0) done(15);
  done(0);
}
