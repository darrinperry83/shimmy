/* ARM64 raw ABI: staged read output and no input consumption on bad output. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "READ_MEMORY_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static char static_out[3] = {'?', '?', '?'};
static const char readonly_text[] __attribute__((section(".text"))) = "Z";
static char oversized[1024 * 1024 + 1];
void _start(void) {
  int fd[2];
  if (sc3(59, (long)fd, 0x800, 0) != 0) done(1); /* nonblocking pipe */
  char bytes[] = "ABCD";
  if (sc3(64, fd[1], (long)bytes, 4) != 4) done(2);
  char stack_out[2] = {'?', '?'};
  if (sc3(63, fd[0], (long)stack_out, 2) != 2) done(3);
  if (stack_out[0] != 'A' || stack_out[1] != 'B') done(4);
  if (sc3(63, fd[0], (long)static_out, sizeof static_out) != 2) done(5);
  if (static_out[0] != 'C' || static_out[1] != 'D' || static_out[2] != '?') done(6);

  char q = 'Q', received = '?';
  if (sc3(64, fd[1], (long)&q, 1) != 1) done(7);
  if (sc3(63, fd[0], 1, 1) != -14) done(8);
  if (sc3(63, fd[0], (long)&received, 1) != 1 || received != 'Q') done(9);

  char r = 'R'; received = '?';
  if (sc3(64, fd[1], (long)&r, 1) != 1) done(10);
  if (sc3(63, fd[0], (long)readonly_text, 1) != -14) done(11);
  if (readonly_text[0] != 'Z') done(12);
  if (sc3(63, fd[0], (long)&received, 1) != 1 || received != 'R') done(13);

  if (sc3(63, fd[0], (long)oversized, sizeof oversized) != -95) done(14);
  if (sc3(63, -1, (long)&received, 1) != -9) done(15);
  if (sc3(63, fd[0], 1, 0) != 0) done(16);

  int eof[2];
  if (sc3(59, (long)eof, 0, 0) != 0) done(17);
  char xy[] = "XY";
  if (sc3(64, eof[1], (long)xy, 2) != 2) done(18);
  if (sc3(57, eof[1], 0, 0) != 0) done(19);
  static_out[0] = static_out[1] = static_out[2] = '?';
  if (sc3(63, eof[0], (long)static_out, 3) != 2) done(20);
  if (static_out[0] != 'X' || static_out[1] != 'Y' || static_out[2] != '?') done(21);
  if (sc3(63, eof[0], (long)static_out, 3) != 0) done(22);
  done(0);
}
