/* ARM64 raw ABI: getcwd copies a NUL-terminated guest path through checked memory. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GETCWD_MEMORY_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static char static_out[16] = {'?', '?', '?', '?', '?'};
static const char readonly_text[] __attribute__((section(".text"))) = "R";
void _start(void) {
  static const char path[] = "/tmp";
  if (sc3(49, (long)path, 0, 0) != 0) done(1);
  char stack_out[16] = {'?', '?', '?', '?', '?'};
  if (sc3(17, (long)stack_out, sizeof stack_out, 0) != 5) done(2);
  if (stack_out[0] != '/' || stack_out[1] != 't' || stack_out[2] != 'm' ||
      stack_out[3] != 'p' || stack_out[4] != 0 || stack_out[5] != 0) done(3);
  if (sc3(17, (long)static_out, sizeof static_out, 0) != 5) done(4);
  if (static_out[0] != '/' || static_out[1] != 't' || static_out[2] != 'm' ||
      static_out[3] != 'p' || static_out[4] != 0 || static_out[5] != 0) done(5);
  stack_out[0] = '?';
  if (sc3(17, (long)stack_out, 4, 0) != -34 || stack_out[0] != '?') done(6);
  if (sc3(17, 1, sizeof stack_out, 0) != -14) done(7);
  if (sc3(17, (long)readonly_text, sizeof stack_out, 0) != -14 ||
      readonly_text[0] != 'R') done(8);
  if (sc3(17, 0, sizeof stack_out, 0) != -14) done(9);
  done(0);
}
