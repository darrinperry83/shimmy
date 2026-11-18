/* ARM64 raw ABI: pread64 uses checked guest output and keeps file offset. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "PREAD64_CHECKED_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static char static_out[5] = {'?', '?', '?', '?', '?'};
static const char readonly_text[] __attribute__((section(".text"))) = "R";
static char oversized[1024 * 1024 + 1];
void _start(void) {
  static const char path[] = "/tmp/data";
  long fd = sc4(56, -100, (long)path, 0, 0); /* openat O_RDONLY */
  if (fd < 0) done(1);
  if (sc4(62, fd, 3, 0, 0) != 3) done(2); /* SEEK_SET */
  char stack_out[4] = {'?', '?', '?', '?'};
  if (sc4(67, fd, (long)stack_out, 4, 2) != 4) done(3);
  if (stack_out[0] != '2' || stack_out[1] != '3' ||
      stack_out[2] != '4' || stack_out[3] != '5') done(4);
  if (sc4(62, fd, 0, 1, 0) != 3) done(5); /* pread did not move offset */

  if (sc4(67, fd, (long)static_out, 5, 7) != 3) done(6); /* short EOF */
  if (static_out[0] != '7' || static_out[1] != '8' ||
      static_out[2] != '9' || static_out[3] != '?' || static_out[4] != '?') done(7);
  if (sc4(62, fd, 0, 1, 0) != 3) done(8);

  if (sc4(67, fd, (long)readonly_text, 1, 0) != -14) done(9);
  if (readonly_text[0] != 'R') done(10);
  if (sc4(67, fd, 1, 1, 0) != -14) done(11);
  if (sc4(67, fd, 0, 1, 0) != -14) done(12);
  if (sc4(62, fd, 0, 1, 0) != 3) done(13);
  if (sc4(67, fd, (long)oversized, sizeof oversized, 0) != -95) done(14);
  if (sc4(67, fd, (long)stack_out, 1, -1) != -22) done(15);
  if (sc4(67, fd, (long)stack_out, 2, 0x7fffffffffffffffL) != -22) done(16);
  if (sc4(67, -1, (long)stack_out, 1, 0) != -9) done(17);
  if (sc4(67, fd, 0, 0, 0) != 0) done(18);
  done(0);
}
