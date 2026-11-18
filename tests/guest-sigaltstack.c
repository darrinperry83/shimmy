/* ARM64 raw ABI: shadow state, transactional errors and process isolation. */
struct lx_stack { void *sp; int flags; int pad; unsigned long size; };
static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "GUEST_SIGALTSTACK_OK\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}
static char alt[16384];
void _start(void) {
  struct lx_stack old = {0};
  struct lx_stack next = {0, 0, 0, sizeof alt};
  next.sp = alt;
  if (sc3(132, 0, (long)&old, 0) || old.flags != 2) finish(1);
  if (sc3(132, 1, 0, 0) != -14) finish(2);
  if (sc3(132, (long)&next, 1, 0) != -14) finish(3);
  if (sc3(132, 0, (long)&old, 0) || old.flags != 2) finish(4);
  if (sc3(132, (long)&next, (long)&old, 0) || old.flags != 2) finish(5);
  if (sc3(132, 0, (long)&old, 0)) finish(6);
  if (old.sp != alt) finish(16);
  if (old.size != sizeof alt) finish(17);
  if (old.flags) finish(18);
  next.sp = alt + 64;
  if (sc3(132, (long)&next, 1, 0) != -14) finish(7);
  if (sc3(132, 0, (long)&old, 0) || old.sp != alt) finish(8);
  next.flags = 0x4000;
  if (sc3(132, (long)&next, 0, 0) != -22) finish(9);
  next.flags = 0; next.size = 100;
  if (sc3(132, (long)&next, 0, 0) != -12) finish(10);
  if (sc3(132, 0, (long)&old, 0) || old.sp != alt) finish(11);
  next.flags = 1; next.size = sizeof alt;
  if (sc3(132, (long)&next, 0, 0)) finish(12);
  if (sc3(132, 0, (long)&old, 0) || old.sp != alt + 64 || old.flags) finish(13);
  next.flags = 2;
  if (sc3(132, (long)&next, 0, 0)) finish(14);
  if (sc3(132, 0, (long)&old, 0) || old.sp || old.size || old.flags != 2) finish(15);
  finish(0);
}
