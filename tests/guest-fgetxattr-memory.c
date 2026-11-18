/* ARM64 raw ABI: fgetxattr snapshots its name and checks value output. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "FGETXATTR_MEMORY_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static const char readonly_name[] __attribute__((section(".text"))) = "user.shimmy-test";
static char static_out[8] = {'?', '?', '?', '?'};
void _start(void) {
  static const char path[] = "/tmp/data";
  long fd = sc4(56, -100, (long)path, 0, 0);
  if (fd < 0) done(1);
  if (sc4(10, fd, (long)readonly_name, 0, 0) != 3) done(2); /* size query */
  char stack_out[8] = {'?', '?', '?', '?'};
  if (sc4(10, fd, (long)readonly_name, (long)stack_out, sizeof stack_out) != 3) done(3);
  if (stack_out[0] != 'a' || stack_out[1] != 'b' || stack_out[2] != 'c' ||
      stack_out[3] != '?') done(4);
  if (sc4(10, fd, (long)readonly_name, (long)static_out, sizeof static_out) != 3) done(5);
  if (static_out[0] != 'a' || static_out[1] != 'b' || static_out[2] != 'c' ||
      static_out[3] != '?') done(6);
  stack_out[0] = '?';
  if (sc4(10, fd, (long)readonly_name, (long)stack_out, 2) != -34 ||
      stack_out[0] != '?') done(7);
  if (sc4(10, fd, (long)readonly_name, 1, 3) != -14) done(8);
  if (sc4(10, fd, (long)readonly_name, (long)readonly_name, 3) != -14 ||
      readonly_name[0] != 'u') done(9);
  if (sc4(10, fd, 1, (long)stack_out, 3) != -14) done(10);
  if (sc4(10, fd, (long)readonly_name, (long)static_out, 1024 * 1024 + 1) != 3 ||
      static_out[0] != 'a') done(11);
  if (sc4(10, -1, (long)readonly_name, (long)static_out, sizeof static_out) != -9)
    done(12);
  done(0);
}
