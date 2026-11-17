/* libc-free AArch64 probe for Shimmy's bounded sched_setattr implementation. */
#include <stdint.h>

void *memset(void *dst, int value, __SIZE_TYPE__ n) {
  unsigned char *p = dst;
  for (__SIZE_TYPE__ i = 0; i < n; i++) p[i] = (unsigned char)value;
  return dst;
}
void *memcpy(void *dst, const void *src, __SIZE_TYPE__ n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  for (__SIZE_TYPE__ i = 0; i < n; i++) d[i] = s[i];
  return dst;
}

struct sched_attr_v0 {
  uint32_t size, policy;
  uint64_t flags;
  int32_t nice;
  uint32_t priority;
  uint64_t runtime, deadline, period;
};
static long call4(long nr, long a0, long a1, long a2, long a3) {
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
__attribute__((noreturn)) static void done(int code) {
  call4(93, code, 0, 0, 0);
  for (;;) {}
}
__attribute__((noreturn)) void _start(void) {
  struct sched_attr_v0 a = {0};
  if (call4(275, 0, (long)&a, sizeof a, 0)) done(1);
  if (a.size != sizeof a || a.policy || a.nice < -20 || a.nice > 19) done(2);
  if (call4(274, 0, (long)&a, 0, 0)) done(3); /* Idempotent current state. */
  a.policy = 1;
  if (call4(274, 0, (long)&a, 0, 0) != -95) done(4);
  a.policy = 0; a.priority = 1;
  if (call4(274, 0, (long)&a, 0, 0) != -95) done(5);
  a.priority = 0; a.nice = 20;
  if (call4(274, 0, (long)&a, 0, 0) != -22) done(6);
  a.nice = 0;
  if (call4(274, 0, (long)&a, 1, 0) != -22) done(7);
  if (call4(274, 2000000000, (long)&a, 0, 0) != -3) done(8);
  a.size = 47;
  if (call4(274, 0, (long)&a, 0, 0) != -22) done(9);
  a.size = sizeof a;
  if (call4(275, 0, (long)&a, sizeof a, 0)) done(10);
  struct { struct sched_attr_v0 v0; uint64_t extension; } ext = {0};
  ext.v0 = a;
  ext.v0.size = sizeof ext;
  if (call4(274, 0, (long)&ext, 0, 0)) done(13);
  ext.extension = 1;
  if (call4(274, 0, (long)&ext, 0, 0) != -7) done(14);
  if (a.nice < 19) {
    a.nice++;
    if (call4(274, 0, (long)&a, 0, 0)) done(11);
    struct sched_attr_v0 result = {0};
    if (call4(275, 0, (long)&result, sizeof result, 0) || result.nice != a.nice)
      done(12);
  }
  static const char ok[] = "sched_setattr guest ABI PASS\n";
  call4(64, 1, (long)ok, sizeof ok - 1, 0);
  done(0);
}
