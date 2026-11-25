/* ARM64 raw ABI: #27 must check the guest path before registering a watch. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "INOTIFY_ADD_PATH_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static const char path[] __attribute__((section(".text"))) = "/watch";
void _start(void) {
  long fd = sc3(26, 0, 0, 0); // inotify_init1
  if (fd < 0) done(1);
  if (sc3(27, fd, 1, 2) != -14) done(2); // bad path, IN_MODIFY
  long wd = sc3(27, fd, (long)path, 2);
  if (wd != 1) done(3); // rejected call did not consume a watch ID
  // sd_event_add_inotify opens O_PATH, then watches /proc/self/fd/N.
  long target = sc3(56, -100, (long)path, 0x280000);
  if (target < 0 || target > 999999) done(6);
  static char alias[40] = "/proc/self/fd/";
  char digits[16]; int count = 0, pos = 14;
  long n = target;
  do { digits[count++] = '0' + n % 10; n /= 10; } while (n);
  while (count) alias[pos++] = digits[--count];
  alias[pos] = 0;
  if (sc3(27, fd, (long)alias, 2) != wd) done(7);
  if (sc3(57, target, 0, 0) != 0) done(8);
  if (sc3(27, fd, (long)alias, 2) != -2) done(9);
  if (sc3(28, fd, wd, 0) != 0) done(4);
  if (sc3(57, fd, 0, 0) != 0) done(5); // close
  done(0);
}
