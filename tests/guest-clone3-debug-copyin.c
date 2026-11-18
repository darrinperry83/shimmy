/* Native AArch64 clone3: checked input/output, then ordinary children. */
#include <stdint.h>
static long sc6(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "CLONE3_INPUT_OK\n";
  if (!code) sc6(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(93, code, 0, 0, 0, 0, 0);
  for (;;) {}
}
void _start(void) {
  if (sc6(435, 1, 64, 0, 0, 0, 0) != -14) done(1);
  uint64_t args[8];
  args[0] = 0; args[1] = 0; args[2] = 0; args[3] = 0;
  args[4] = 17; /* SIGCHLD */
  args[5] = 0; args[6] = 0; args[7] = 0;
  if (sc6(435, (long)args, 16, 0, 0, 0, 0) != -22) done(2);
  args[0] = 0x1000; /* CLONE_PIDFD */
  args[1] = 1;
  if (sc6(435, (long)args, sizeof args, 0, 0, 0, 0) != -14) done(6);
  if (sc6(260, -1, 0, 1, 0, 0, 0) != -10) done(7); /* no child */
  args[0] = 0x100000; /* CLONE_PARENT_SETTID */
  args[1] = 0;
  args[3] = 1;
  if (sc6(435, (long)args, sizeof args, 0, 0, 0, 0) != -14) done(8);
  args[0] = 0x1000000; /* CLONE_CHILD_SETTID */
  args[2] = 1;
  args[3] = 0;
  if (sc6(435, (long)args, sizeof args, 0, 0, 0, 0) != -14) done(9);
  args[0] = 0x200000; /* CLONE_CHILD_CLEARTID */
  if (sc6(435, (long)args, sizeof args, 0, 0, 0, 0) != -14) done(13);
  args[0] = 0; args[2] = 0;
  long child = sc6(435, (long)args, sizeof args, 0, 0, 0, 0);
  if (child == 0) sc6(93, 0, 0, 0, 0, 0, 0);
  if (child < 0) done(3);
  int status = -1;
  if (sc6(260, child, (long)&status, 0, 0, 0, 0) != child) done(4);
  if (status != 0) done(5);
  int pidfd = -1;
  args[0] = 0x1000; args[1] = (long)&pidfd;
  child = sc6(435, (long)args, sizeof args, 0, 0, 0, 0);
  if (child == 0) sc6(93, 0, 0, 0, 0, 0, 0);
  if (child < 0 || pidfd < 0) done(10);
  status = -1;
  if (sc6(260, child, (long)&status, 0, 0, 0, 0) != child || status != 0)
    done(11);
  if (sc6(57, pidfd, 0, 0, 0, 0, 0) != 0) done(12);
  done(0);
}
