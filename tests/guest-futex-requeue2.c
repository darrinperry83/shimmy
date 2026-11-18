// AArch64 raw syscall 456: private U32 zero-requeue subset.
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
static volatile uint32_t source, target;
static volatile int ready, done;
static unsigned char stack[65536] __attribute__((aligned(16)));
static struct waiter pair[2] = { {0, 0, 130, 0}, {0, 0, 130, 0} };

static __attribute__((noreturn)) void finish(int status) {
  static const char ok[] = "futex_requeue2 guest ABI PASS\n";
  if (!status) sc6(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}

__attribute__((noreturn)) void child_main(void) {
  ready = 1;
  long r = sc6(455, (long)&source, 0, 4, 130, 0, 1);
  done = r == 0 ? 1 : -1;
  sc6(93, 0, 0, 0, 0, 0, 0);
  for (;;) {}
}

static long spawn(void) {
  register long x0 __asm__("x0") = 0x10f00; // CLONE_VM|FS|FILES|SIGHAND|THREAD
  register long x1 __asm__("x1") = (long)(stack + sizeof stack);
  register long x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
  register long x4 __asm__("x4") = 0, x8 __asm__("x8") = 220;
  __asm__ volatile("svc #0\ncbnz x0, 1f\nbl child_main\n1:"
                   : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
                   : "memory", "x30");
  return x0;
}

void _start(void) {
  pair[0].uaddr = (long)&source;
  pair[1].uaddr = (long)&target;
  if (sc6(456, (long)pair, 0, 1, 0, 0, 0) != 0) finish(1);
  pair[0].val = 1;
  if (sc6(456, (long)pair, 0, 1, 0, 0, 0) != -11) finish(2);
  pair[0].val = 0;
  if (sc6(456, (long)pair, 1, 1, 0, 0, 0) != -22) finish(3);
  if (sc6(456, 0, 0, 1, 0, 0, 0) != -22) finish(4);
  if (sc6(456, 0x100000, 0, 1, 0, 0, 0) != -14) finish(5);
  if (sc6(456, (long)pair, 0, -1, 0, 0, 0) != -22) finish(6);
  if (sc6(456, (long)pair, 0, 1, -1, 0, 0) != -22) finish(7);
  if (sc6(456, (long)pair, 0, 1, 1, 0, 0) != -95) finish(8);
  pair[1].reserved = 1;
  if (sc6(456, (long)pair, 0, 1, 0, 0, 0) != -22) finish(9);
  pair[1].reserved = 0;
  pair[1].flags = 2;
  if (sc6(456, (long)pair, 0, 1, 0, 0, 0) != -22) finish(10);
  pair[1].flags = 130;

  if (spawn() <= 0) finish(11);
  unsigned spins = 0;
  while (!ready && ++spins < 100000000) {}
  if (!ready) finish(12);
  for (spins = 0; spins < 1000000 && !done; ++spins) {
    long woke = sc6(456, (long)pair, 0, 1, 0, 0, 0);
    if (woke < 0 || woke > 1) finish(13);
    if (woke == 1) break;
  }
  if (spins == 1000000) finish(14);
  spins = 0;
  while (!done && ++spins < 100000000) {}
  if (done != 1) finish(15);
  finish(0);
}
