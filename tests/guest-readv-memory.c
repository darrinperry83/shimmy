/* AArch64 raw ABI: checked readv outputs and no consumption on bad ranges. */
struct iovec { void* base; unsigned long len; };
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "READV_MEMORY_OK\n";
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
  struct iovec all[] = {{stack_out, 2}, {static_out, 3}};
  if (sc3(65, fd[0], (long)all, 2) != 4) done(3);
  if (stack_out[0] != 'A' || stack_out[1] != 'B' ||
      static_out[0] != 'C' || static_out[1] != 'D' || static_out[2] != '?') done(4);

  char q = 'Q', received = '?';
  if (sc3(64, fd[1], (long)&q, 1) != 1) done(5);
  struct iovec bad[] = {{stack_out, 1}, {(void*)1, 1}};
  if (sc3(65, fd[0], (long)bad, 2) != -14) done(6);
  if (sc3(63, fd[0], (long)&received, 1) != 1 || received != 'Q') done(7);

  char r = 'R'; received = '?';
  if (sc3(64, fd[1], (long)&r, 1) != 1) done(8);
  struct iovec ro[] = {{(void*)readonly_text, 1}};
  if (sc3(65, fd[0], (long)ro, 1) != -14) done(9);
  if (readonly_text[0] != 'Z') done(10);
  if (sc3(63, fd[0], (long)&received, 1) != 1 || received != 'R') done(11);

  if (sc3(65, fd[0], 1, 1) != -14) done(12);
  if (sc3(65, fd[0], 0, 1025) != -22) done(13);
  struct iovec large[] = {{oversized, sizeof oversized}};
  if (sc3(65, fd[0], (long)large, 1) != -95) done(14);

  int eof[2];
  if (sc3(59, (long)eof, 0, 0) != 0) done(15);
  char xy[] = "XY";
  if (sc3(64, eof[1], (long)xy, 2) != 2) done(16);
  if (sc3(57, eof[1], 0, 0) != 0) done(17);
  stack_out[0] = stack_out[1] = '?';
  static_out[0] = static_out[1] = static_out[2] = '?';
  if (sc3(65, eof[0], (long)all, 2) != 2) done(18);
  if (stack_out[0] != 'X' || stack_out[1] != 'Y' ||
      static_out[0] != '?' || static_out[1] != '?') done(19);
  done(0);
}
