// AArch64 raw syscall 455: bounded private U32 futex2 wait behavior.
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
static unsigned char stack[65536] __attribute__((aligned(16)));
static const long flags = 2 | 128;
static struct { int64_t sec, nsec; } expired = {0, 0};
static struct { int64_t sec, nsec; } bad_nsec = {0, 1000000000};

static __attribute__((noreturn)) void finish(int status) {
  static const char ok[] = "futex_wait2 guest ABI PASS\n";
  if (!status) sc6(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}

__attribute__((noreturn)) void child_main(void) {
  ready = 1;
  long r = sc6(455, (long)&word, 0, 4, flags, 0, 1);
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
  if (sc6(455, (long)&word, 1, 4, flags, 0, 1) != -11) finish(1); // EAGAIN
  if (sc6(455, (long)&word, 0, 0, flags, 0, 1) != -22) finish(2);
  if (sc6(455, (long)&word, 1L << 32, 4, flags, 0, 1) != -22) finish(3);
  if (sc6(455, (long)&word, 0, 1L << 32, flags, 0, 1) != -22) finish(4);
  if (sc6(455, (long)&word + 1, 0, 4, flags, 0, 1) != -22) finish(5);
  if (sc6(455, 0, 0, 4, flags, 0, 1) != -14) finish(6);
  if (sc6(455, 0x100000, 0, 4, flags, 0, 1) != -14) finish(19);
  if (sc6(455, (long)&word, 0, 4, flags | 1, 0, 1) != -22) finish(7);
  if (sc6(455, (long)&word, 0, 4, 2, 0, 1) != -95) finish(8);
  if (sc6(455, (long)&word, 0, 4, flags | 4, 0, 1) != -95) finish(9);
  if (sc6(455, (long)&word, 0, 4, flags, (long)&bad_nsec, 1) != -22) finish(10);
  if (sc6(455, (long)&word, 0, 4, flags, 0x100000, 1) != -14) finish(20);
  if (sc6(455, (long)&word, 0, 4, flags, (long)&expired, 42) != -22) finish(11);
  if (sc6(455, (long)&word, 0, 4, flags, (long)&expired, 1) != -110) finish(12);

  if (spawn() <= 0) finish(13);
  unsigned spins = 0;
  while (!ready && ++spins < 100000000) {}
  if (!ready) finish(14);
  for (spins = 0; spins < 1000000 && !done; ++spins) {
    if (sc6(454, (long)&word, 2, 1, flags, 0, 0) != 0) finish(15);
    long woke = sc6(454, (long)&word, 4, 1, flags, 0, 0);
    if (woke < 0 || woke > 1) finish(16);
    if (woke == 1) break;
  }
  if (spins == 1000000) finish(17);
  spins = 0;
  while (!done && ++spins < 100000000) {}
  if (done != 1) finish(18);
  finish(0);
}
