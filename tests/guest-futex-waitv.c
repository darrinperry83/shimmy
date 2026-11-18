// ARM64 raw syscall 449: one-element private U32 futex_waitv subset.
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

struct waiter { uint64_t val, uaddr; uint32_t flags, reserved; };
static volatile uint32_t word;
static volatile int ready, done;
static unsigned char stack[65536] __attribute__((aligned(16)));
static struct waiter w = {0, 0, 2 | 128, 0};
static struct { int64_t sec, nsec; } expired = {0, 0};
static struct { int64_t sec, nsec; } bad_nsec = {0, 1000000000};

static __attribute__((noreturn)) void finish(int status) {
  static const char ok[] = "futex_waitv guest ABI PASS\n";
  if (!status) sc6(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}

__attribute__((noreturn)) void child_main(void) {
  ready = 1;
  long r = sc6(449, (long)&w, 1, 0, 0, 1, 0);
  done = r == 0 ? 1 : -1;
  sc6(93, 0, 0, 0, 0, 0, 0);
  for (;;) {}
}

static long spawn(void) {
  register long x0 __asm__("x0") = 0x10f00;
  register long x1 __asm__("x1") = (long)(stack + sizeof stack);
  register long x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
  register long x4 __asm__("x4") = 0, x8 __asm__("x8") = 220;
  __asm__ volatile("svc #0\ncbnz x0, 1f\nbl child_main\n1:"
                   : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
                   : "memory", "x30");
  return x0;
}

void _start(void) {
  w.uaddr = (uint64_t)&word;
  if (sc6(449, 0, 1, 0, 0, 1, 0) != -22) finish(1);
  if (sc6(449, (long)&w, 0, 0, 0, 1, 0) != -22) finish(2);
  if (sc6(449, (long)&w, 129, 0, 0, 1, 0) != -22) finish(3);
  if (sc6(449, (long)&w, 1, 1, 0, 1, 0) != -22) finish(4);
  if (sc6(449, 0x100000, 1, 0, 0, 1, 0) != -14) finish(5);
  if (sc6(449, (long)&w, 2, 0, 0, 1, 0) != -95) finish(6);
  w.reserved = 1;
  if (sc6(449, (long)&w, 1, 0, 0, 1, 0) != -22) finish(7);
  w.reserved = 0;
  w.val = 1;
  if (sc6(449, (long)&w, 1, 0, 0, 1, 0) != -11) finish(8);
  w.val = 0;
  w.flags = 2;
  if (sc6(449, (long)&w, 1, 0, 0, 1, 0) != -95) finish(9);
  w.flags = 2 | 128;
  if (sc6(449, (long)&w, 1, 0, (long)&bad_nsec, 1, 0) != -22) finish(10);
  if (sc6(449, (long)&w, 1, 0, (long)&expired, 42, 0) != -22) finish(11);
  if (sc6(449, (long)&w, 1, 0, (long)&expired, 1, 0) != -110) finish(12);

  if (spawn() <= 0) finish(13);
  unsigned spins = 0;
  while (!ready && ++spins < 100000000) {}
  if (!ready) finish(14);
  for (spins = 0; spins < 1000000 && !done; ++spins) {
    long woke = sc6(454, (long)&word, 0xffffffffu, 1, 2 | 128, 0, 0);
    if (woke < 0 || woke > 1) finish(15);
    if (woke == 1) break;
  }
  if (spins == 1000000) finish(16);
  spins = 0;
  while (!done && ++spins < 100000000) {}
  if (done != 1) finish(17);
  finish(0);
}
