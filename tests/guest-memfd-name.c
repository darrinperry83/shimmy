/* ARM64 raw ABI: memfd_create must validate the guest name before mkstemp. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "MEMFD_NAME_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static const char name[] __attribute__((section(".text"))) = "shimmy-test";
static char overlong[250];
void _start(void) {
  if (sc3(279, 1, 0, 0) != -14) done(1);
  if (sc3(279, 0, 0, 0) != -14) done(2);
  for (int i = 0; i < 250; ++i) overlong[i] = 'a';
  if (sc3(279, (long)overlong, 0, 0) != -22) done(3);
  if (sc3(279, (long)name, 0x80000000ul, 0) != -22) done(4);
  long fd = sc3(279, (long)name, 1, 0); // valid static name, MFD_CLOEXEC
  if (fd < 0) done(5);
  static const char data[] = "ok";
  if (sc3(64, fd, (long)data, 2) != 2) done(6);
  if (sc3(57, fd, 0, 0) != 0) done(7);
  done(0);
}
