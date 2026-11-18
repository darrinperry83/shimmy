// Libc-free ARM64 probe for bounded guest msync on a disposable mapped file.
static long call(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2),
                   "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int status) {
  static const char ok[] = "GUEST_MSYNC_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  call(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  static const char path[] = "/data";
  long fd = call(56, -100, (long)path, 2, 0, 0, 0);
  if (fd < 0) done(1);
  long map = call(222, 0, 32768, 3, 1, fd, 0);
  if (map < 0) done(2);
  *(volatile char*)map = 'Q';
  if (call(227, map, 1, 4, 0, 0, 0) != 0) done(3); // MS_SYNC
  if (call(227, map, 1, 1, 0, 0, 0) != 0) done(4); // MS_ASYNC
  if (call(227, map, 1, 0, 0, 0, 0) != 0) done(5); // no I/O
  if (call(227, map, 1, 5, 0, 0, 0) != -22) done(6); // conflicting sync modes
  if (call(227, map, 1, 8, 0, 0, 0) != -22) done(7); // unknown flags
  if (call(227, map + 1, 1, 4, 0, 0, 0) != -22) done(8); // alignment
  if (call(227, 0x100000000L, 16384, 4, 0, 0, 0) != -12) done(9);
  if (call(227, map, 1, 2, 0, 0, 0) != -95) done(10); // invalidate unsupported
  if (call(227, map, 32 * 1024 * 1024, 4, 0, 0, 0) != -95) done(11);
  if (call(227, 0, 0, 4, 0, 0, 0) != 0) done(12);
  if (call(215, map, 32768, 0, 0, 0, 0) != 0) done(13);
  if (call(227, map, 16384, 4, 0, 0, 0) != -12) done(14);
  done(0);
}
