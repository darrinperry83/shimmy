/* ARM64 raw ABI: sched_setattr checks all guest bytes before host nice. */
typedef unsigned int u32;
typedef unsigned long long u64;
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
struct attr { u32 size, policy; u64 flags; int nice; u32 priority;
              u64 runtime, deadline, period; };
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}
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
  static const char ok[] = "SCHED_SETATTR_INPUT_OK\n";
  if (!code) sc4(64, 1, (long)ok, sizeof ok - 1, 0);
  sc4(93, code, 0, 0, 0);
  for (;;) {}
}
static const struct attr unsupported __attribute__((section(".text"))) = {
  .size = sizeof(struct attr), .policy = 1,
};
void _start(void) {
  struct attr current = {0};
  if (sc4(275, 0, (long)&current, sizeof current, 0) != 0 ||
      current.size != sizeof current) done(1);
  if (sc4(274, 0, 1, 0, 0) != -14) done(2);
  if (sc4(274, 0, (long)&unsupported, 0, 0) != -95) done(3);
  if (sc4(274, 0, (long)&current, 0, 0) != 0) done(4); // no change
  current.size = 47;
  if (sc4(274, 0, (long)&current, 0, 0) != -22) done(5);
  current.size = 4097;
  if (sc4(274, 0, (long)&current, 0, 0) != -7) done(6);
  current.size = sizeof current;
  long p = sc6(222, 0, 32768, 3, 0x22, -1, 0);
  if (p < 0 || sc4(226, p + 16384, 16384, 0, 0) != 0) done(7);
  unsigned char *edge = (unsigned char *)(p + 16384 - sizeof current);
  const unsigned char *src = (const unsigned char *)&current;
  for (unsigned i = 0; i < sizeof current; i++) edge[i] = src[i];
  ((struct attr *)edge)->size = sizeof current + 8;
  if (sc4(274, 0, (long)edge, 0, 0) != -14) done(8); // extension protected
  edge = (unsigned char *)(p + 16384 - 4);
  *(u32 *)edge = sizeof current;
  if (sc4(274, 0, (long)edge, 0, 0) != -14) done(9); // base protected
  struct { struct attr base; u64 extra; } extended = {0};
  extended.base = current;
  extended.base.size = sizeof extended;
  extended.extra = 1;
  if (sc4(274, 0, (long)&extended, 0, 0) != -7) done(10);
  extended.extra = 0;
  if (sc4(274, 0, (long)&extended, 0, 0) != 0) done(11);
  struct attr after = {0};
  if (sc4(275, 0, (long)&after, sizeof after, 0) != 0 ||
      after.nice != current.nice) done(12);
  done(0);
}
