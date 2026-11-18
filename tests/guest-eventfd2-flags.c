/* Native AArch64 eventfd2 flag admission and supported fd flags. */
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "EVENTFD2_FLAGS_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1);
  sc(93, code, 0, 0);
  for (;;) {}
}
void _start(void) {
  if (sc(19, 0, 0x4, 0) != -22) done(1);            /* unknown */
  if (sc(19, 0, 0x80004, 0) != -22) done(2);      /* known + unknown */
  if (sc(19, 2, 1, 0) != -95) done(3);            /* semaphore unsupported */
  if (sc(19, 2, 0x801, 0) != -95) done(4);        /* with nonblock */
  long plain = sc(19, 0, 0, 0);
  if (plain < 0) done(5);
  if (sc(25, plain, 1, 0) != 0) done(6);          /* F_GETFD */
  if (sc(25, plain, 3, 0) & 0x800) done(7);      /* F_GETFL O_NONBLOCK */
  if (sc(57, plain, 0, 0) != 0) done(8);
  long flagged = sc(19, 0, 0x800 | 0x80000, 0);
  if (flagged < 0) done(9);
  if (sc(25, flagged, 1, 0) != 1) done(10);
  if (!(sc(25, flagged, 3, 0) & 0x800)) done(11);
  if (sc(57, flagged, 0, 0) != 0) done(12);
  done(0);
}
