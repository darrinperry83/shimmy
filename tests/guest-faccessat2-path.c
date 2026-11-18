/* ARM64 raw ABI: faccessat2 must snapshot its guest pathname. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "FACCESSAT2_PATH_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static const char path[] __attribute__((section(".text"))) = "/tmp/file";
static const char empty[] __attribute__((section(".text"))) = "";
void _start(void) {
  if (sc4(439, -100, 1, 0, 0) != -14) done(1);
  if (sc4(439, -100, 1, 0, 0x1000) != -14) done(2);
  if (sc4(439, -100, 0, 0, 0x1000) != -14) done(3);
  if (sc4(439, -100, (long)path, 0, 0) != 0) done(4);
  long fd = sc4(56, -100, (long)path, 0, 0); // openat O_RDONLY
  if (fd < 0) done(5);
  if (sc4(439, fd, (long)empty, 0, 0x1000) != 0) done(6);
  if (sc4(57, fd, 0, 0, 0) != 0) done(7);
  done(0);
}
