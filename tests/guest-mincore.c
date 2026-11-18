// Libc-free ARM64 probe for bounded guest mincore and safe vector copyout.
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
  static const char ok[] = "GUEST_MINCORE_OK\n";
  if (!status) call(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  call(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  long map = call(222, 0, 32768, 3, 0x22, -1, 0);
  if (map < 0) done(1);
  *(volatile char*)map = 1;
  unsigned char vec[3] = {0xaa, 0xbb, 0xcc};
  if (call(232, map, 1, (long)vec, 0, 0, 0) != 0) done(2);
  if (!(vec[0] & 1) || vec[1] != 0xbb || vec[2] != 0xcc) done(3);
  if (call(232, map, 32768, (long)vec, 0, 0, 0) != 0) done(4);
  if (!(vec[0] & 1) || vec[2] != 0xcc) done(5);
  if (call(232, map + 1, 1, (long)vec, 0, 0, 0) != -22) done(6);
  if (call(232, 0, 16384, (long)vec, 0, 0, 0) != -12) done(7);
  if (call(232, map, 16384, 1, 0, 0, 0) != -14) done(8);
  if (call(232, map, 32 * 1024 * 1024, (long)vec, 0, 0, 0) != -95) done(9);
  long text_page = (long)&_start & ~16383L;
  if (call(232, text_page, 1, (long)vec, 0, 0, 0) != 0) done(12);
  if (call(215, map, 32768, 0, 0, 0, 0) != 0) done(10);
  if (call(232, map, 16384, (long)vec, 0, 0, 0) != -12) done(11);
  done(0);
}
