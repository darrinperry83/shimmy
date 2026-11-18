// Minimal AArch64 Linux ABI probe: no libc silently registers its own list.
#include <stdint.h>

static long sc3(long nr, long a, long b, long c) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}
static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}

static void finish(int code) {
  static const char ok[] = "get_robust_list guest ABI PASS\n";
  if (!code) sc3(64, 1, (long)ok, sizeof ok - 1);
  sc3(93, code, 0, 0);
  for (;;) {}
}

void _start(void) {
  unsigned long head = 42, length = 42;
  long first = sc3(100, 0, (long)&head, (long)&length);
  if (first) finish(10);
  if (head) finish(11);
  if (length != 24) finish(12);

  unsigned long list[3] = {0};
  if (sc3(99, (long)list, 8, 0) != -22) finish(2);
  if (sc3(99, (long)list, 24, 0)) finish(3);
  head = length = 0;
  if (sc3(100, 0, (long)&head, (long)&length) || head != (unsigned long)list || length != 24) finish(4);

  long tid = sc3(178, 0, 0, 0);
  head = 0;
  if (sc3(100, tid, (long)&head, (long)&length) || head != (unsigned long)list) finish(5);
  if (sc3(100, 999999, (long)&head, (long)&length) != -3) finish(6);
  if (sc3(100, 0, 1, (long)&length) != -14) finish(7);
  if (sc3(100, 0, (long)&head, 0) != -14) finish(8);

  // A forked process starts without its parent's per-thread registration.
  long child = sc5(220, 17, 0, 0, 0, 0); // clone(SIGCHLD), no CLONE_VM
  if (child == 0) {
    head = length = 42;
    if (sc3(100, 0, (long)&head, (long)&length) || head || length != 24) finish(9);
    sc3(93, 0, 0, 0);
    for (;;) {}
  }
  if (child < 0) finish(13);
  int status = -1;
  if (sc5(260, child, (long)&status, 0, 0, 0) != child || status != 0) finish(14);
  finish(0);
}
