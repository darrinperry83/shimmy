/* ARM64 raw ABI: lremovexattr removes only the symlink's attribute. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "LREMOVEXATTR_MEMORY_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static const char path[] __attribute__((section(".text"))) = "/tmp/link";
static const char name[] __attribute__((section(".text"))) = "user.shimmy-test";
static char value[8];
void _start(void) {
  if (sc4(9, (long)path, (long)name, (long)value, sizeof value) != 4 || value[0] != 'l')
    done(1);
  /* Neither invalid address may remove the attribute. */
  if (sc4(15, 1, (long)name, 0, 0) != -14) done(2);
  if (sc4(15, (long)path, 1, 0, 0) != -14) done(3);
  if (sc4(9, (long)path, (long)name, (long)value, sizeof value) != 4) done(4);
  if (sc4(15, (long)path, (long)name, 0, 0) != 0) done(5);
  if (sc4(9, (long)path, (long)name, (long)value, sizeof value) >= 0) done(6);
  if (sc4(8, (long)path, (long)name, (long)value, sizeof value) != 6 || value[0] != 't')
    done(7); /* target still has its attribute */
  done(0);
}
