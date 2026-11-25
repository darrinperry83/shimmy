/* ARM64 raw ABI: active-console sysfs supports EOF and repeated rewind/read. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "VT_SYSFS_READ_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static const char path[] = "/sys/class/tty/tty0/active";
void _start(void) {
  long fd = sc3(56, -100, (long)path, 0);
  if (fd < 0) done(1);
  char buf[16];
  for (int i = 0; i < 3; i++) {
    if (sc3(62, fd, 0, 0) != 0) done(2);
    long n = sc3(63, fd, (long)buf, sizeof buf);
    if (n != 5 || buf[0]!='t' || buf[1]!='t' || buf[2]!='y' || buf[3]!='1' || buf[4]!='\n') done(3);
    if (sc3(63, fd, (long)buf, sizeof buf) != 0) done(4);
  }
  done(0);
}
