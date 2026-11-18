// AArch64 raw syscall 454: private U32 bitset wake of a classic futex waiter.
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

static volatile uint32_t word;
static volatile int ready, done;
static __attribute__((noreturn)) void finish(int status) {
  static const char ok[] = "futex_wake2 guest ABI PASS\n";
  if (!status) sc6(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}

__attribute__((noreturn)) void child_main(void) {
  ready = 1;
  long r = sc6(98, (long)&word, 9 | 128, 0, 0, 0, 4); // WAIT_BITSET|PRIVATE
  done = (r == 0) ? 1 : -1;
  sc6(93, 0, 0, 0, 0, 0, 0);
  for (;;) {}
}

static long spawn(long stack) {
  register long x0 __asm__("x0") = 0x10f00; // CLONE_VM|FS|FILES|SIGHAND|THREAD
  register long x1 __asm__("x1") = stack;
  register long x2 __asm__("x2") = 0, x3 __asm__("x3") = 0, x4 __asm__("x4") = 0;
  register long x8 __asm__("x8") = 220;
  __asm__ volatile("svc #0\ncbnz x0, 1f\nbl child_main\n1:"
                   : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
                   : "memory", "x30");
  return x0;
}

void _start(void) {
  const long flags = 2 | 128; // FUTEX2_SIZE_U32 | FUTEX2_PRIVATE
  if (sc6(454, (long)&word, 4, 1, flags, 0, 0) != 0) finish(1);
  if (sc6(454, (long)&word, 0, 1, flags, 0, 0) != -22) finish(2);
  if (sc6(454, (long)&word, 1L << 32, 1, flags, 0, 0) != -22) finish(3);
  if (sc6(454, (long)&word + 1, 4, 1, flags, 0, 0) != -22) finish(4);
  if (sc6(454, (long)&word, 4, 1, flags | 1, 0, 0) != -22) finish(5);
  if (sc6(454, (long)&word, 4, 1, 2, 0, 0) != -95) finish(6); // shared unsupported
  if (sc6(454, (long)&word, 4, 1, flags | 4, 0, 0) != -95) finish(7); // NUMA unsupported

  long stack = sc6(222, 0, 65536, 3, 0x22, -1, 0);
  if (stack < 0 || spawn(stack + 65536) <= 0) finish(8);
  unsigned spins = 0;
  while (!ready && ++spins < 100000000) {}
  if (!ready) finish(9);
  // The waiter may not yet have parked. A wrong mask cannot consume it;
  // retry the matching wake until it reports the single waiter it woke.
  for (spins = 0; spins < 1000000 && !done; spins++) {
    if (sc6(454, (long)&word, 2, 1, flags, 0, 0) != 0) finish(10);
    if (sc6(454, (long)&word, 4, 0, flags, 0, 0) != 0) finish(14);
    long woke = sc6(454, (long)&word, 4, 1, flags, 0, 0);
    if (woke < 0 || woke > 1) finish(11);
    if (woke == 1) break;
  }
  if (spins == 1000000) finish(12);
  spins = 0;
  while (!done && ++spins < 100000000) {}
  if (done != 1) finish(13);
  finish(0);
}
