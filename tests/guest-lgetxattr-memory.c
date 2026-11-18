/* ARM64 raw ABI: lgetxattr reads link inode with checked guest addresses. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "LGETXATTR_MEMORY_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static const char readonly_path[] __attribute__((section(".text"))) = "/tmp/link";
static const char readonly_name[] __attribute__((section(".text"))) = "user.shimmy-test";
static char static_out[8] = {'?', '?', '?', '?', '?'};
void _start(void) {
  if (sc4(9, (long)readonly_path, (long)readonly_name, 0, 0) != 4) done(1);
  char stack_out[8] = {'?', '?', '?', '?', '?'};
  if (sc4(9, (long)readonly_path, (long)readonly_name, (long)stack_out, sizeof stack_out) != 4)
    done(2);
  if (stack_out[0] != 'l' || stack_out[1] != 'i' || stack_out[2] != 'n' ||
      stack_out[3] != 'k' || stack_out[4] != '?') done(3);
  if (sc4(8, (long)readonly_path, (long)readonly_name, (long)static_out,
          sizeof static_out) != 6 || static_out[0] != 't') done(4); /* follows target */
  if (sc4(9, (long)readonly_path, (long)readonly_name, (long)static_out,
          sizeof static_out) != 4 || static_out[0] != 'l') done(5);
  stack_out[0] = '?';
  if (sc4(9, (long)readonly_path, (long)readonly_name, (long)stack_out, 2) != -34 ||
      stack_out[0] != '?') done(6);
  if (sc4(9, (long)readonly_path, (long)readonly_name, 1, 4) != -14) done(7);
  if (sc4(9, (long)readonly_path, (long)readonly_name, (long)readonly_path, 4) != -14)
    done(8);
  if (sc4(9, 1, (long)readonly_name, (long)stack_out, 4) != -14) done(9);
  if (sc4(9, (long)readonly_path, 1, (long)stack_out, 4) != -14) done(10);
  if (sc4(9, (long)readonly_path, (long)readonly_name, (long)static_out,
          1024 * 1024 + 1) != 4 || static_out[0] != 'l') done(11);
  done(0);
}
