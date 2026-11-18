/* Linux AArch64 kill(2) selectors: guest PID 1 must map to self, never host 1. */
#ifndef HOST_PID
#error HOST_PID must name the unrelated live host test runner
#endif
static long sc(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void finish(int code) {
  static const char regular[] = "GUEST_KILL_REGULAR_OK\n";
  static const char init[] = "GUEST_KILL_INIT_OK\n";
  if (!code) {
    const char *s = sc(172,0,0,0) == 1 ? init : regular;
    sc(64,1,(long)s,s == init ? sizeof init-1 : sizeof regular-1);
  }
  sc(93,code,0,0);
  for (;;) {}
}
void _start(void) {
  long self = sc(172,0,0,0);
  if (self <= 0 || self == HOST_PID) finish(1);
  if (sc(129,self,0,0) != 0) finish(2);
  if (sc(129,HOST_PID,0,0) != -3) finish(3);
  if (sc(129,2147483647L,0,0) != -3) finish(4);
  if (sc(129,-1,15,0) != -95) finish(5); /* never send SIGTERM to host/self */
  if (sc(129,0,15,0) != -95) finish(6);
  if (sc(129,-self,15,0) != -95) finish(7);
  long child = sc(220,17,0,0);          /* clone(SIGCHLD) */
  if (!child) {
    static const long one_second[2] = {1, 0};
    for (;;) sc(101,(long)one_second,0,0); /* remain a live known child */
  }
  if (child < 0) finish(8);
  if (sc(129,child,0,0) != 0) finish(9);
  if (sc(129,child,9,0) != 0) finish(10);
  int status = 0;
  if (sc(260,child,(long)&status,0) != child) finish(11);
  finish(0);
}
