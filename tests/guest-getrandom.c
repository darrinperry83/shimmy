/* Raw ARM64 getrandom ABI and bounded secure-mode probe. */
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_GETRANDOM_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static unsigned char bytes[512];
void _start(void) {
  for (int i = 0; i < 512; ++i) bytes[i] = 0xa5;
  if (sc3(278, (long)bytes, 32, 0) != 32) finish(1);
  int changed = 0;
  for (int i = 0; i < 32; ++i) changed |= bytes[i] != 0xa5;
  if (!changed || bytes[32] != 0xa5) finish(2);
  if (sc3(278, (long)bytes, 256, 1) != 256) finish(3); /* NONBLOCK */
  if (sc3(278, (long)bytes, 512, 0) != 256) finish(4); /* valid short read */
  if (bytes[256] != 0xa5) finish(5);
  if (sc3(278, 0, 0, 0) != 0) finish(6);
  if (sc3(278, 0, 1, 0) != -14) finish(7); /* EFAULT */
  if (sc3(278, 1, 16, 0) != -14) finish(8); /* unowned address */
  if (sc3(278, (long)bytes, 16, 8) != -22) finish(9); /* bad flags */
  if (sc3(278, (long)bytes, 16, 2) != -95) finish(10); /* RANDOM */
  if (sc3(278, (long)bytes, 16, 4) != -95) finish(11); /* INSECURE */
  if (sc3(278, (long)bytes, 16, 6) != -22) finish(12); /* conflicting sources */
  finish(0);
}
