/* ARM64 raw ABI: failed dup3 must not tag another fd as a virtual device. */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "DUP3_METADATA_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static const char input[] = "/dev/input/event0";
static const char data[] = "/tmp/data";
static const char other[] = "/tmp/other";
static unsigned long statbuf[16];
static int type_is(long fd, unsigned int type) {
  if (sc4(80, fd, (long)statbuf, 0, 0) != 0) return 0;
  return ((unsigned int *)(void *)statbuf)[4] >> 12 == type;
}
void _start(void) {
  long device = sc4(56, -100, (long)input, 0, 0);
  long target = sc4(56, -100, (long)data, 0, 0);
  if (device < 0 || target < 0 || !type_is(device, 2) || !type_is(target, 8)) done(1);
  if (sc4(24, device, target, 1, 0) != -22 || !type_is(target, 8)) done(2);
  if (sc4(24, -1, target, 0, 0) != -9 || !type_is(target, 8)) done(3);
  if (sc4(24, device, device, 0, 0) != -22) done(4);
  if (sc4(57, device, 0, 0, 0) != 0) done(5);
  long reused = sc4(56, -100, (long)other, 0, 0);
  if (reused < 0 || !type_is(reused, 8)) done(6);
  long device2 = sc4(56, -100, (long)input, 0, 0);
  if (device2 < 0 || !type_is(device2, 2)) done(7);
  if (sc4(24, device2, target, 0x80000, 0) != target || !type_is(target, 2)) done(8);
  if (sc4(25, target, 1, 0, 0) != 1) done(9); /* F_GETFD / FD_CLOEXEC */
  done(0);
}
