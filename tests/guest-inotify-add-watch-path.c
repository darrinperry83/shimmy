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
  if (sc3(28, fd, wd, 0) != 0) done(4);
  if (sc3(57, fd, 0, 0) != 0) done(5); // close
  done(0);
}
