// Verify that robust-list registrations belong to guest threads, not processes.
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

static volatile int ready, release_child, child_done;
static unsigned long child_list[3];

static __attribute__((noreturn)) void end(int status) {
  static const char ok[] = "get_robust_list guest thread PASS\n";
  if (!status) sc6(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(93, status, 0, 0, 0, 0, 0);
  for (;;) {}
}

__attribute__((noreturn)) void child_main(void) {
  unsigned long head = 42, length = 42;
  if (sc6(100, 0, (long)&head, (long)&length, 0, 0, 0) || head || length != 24) end(21);
  if (sc6(99, (long)child_list, 24, 0, 0, 0, 0)) end(22);
  ready = 1;
  while (!release_child) {}
  child_done = 1;
  sc6(93, 0, 0, 0, 0, 0, 0);
  for (;;) {}
}

static long spawn(long stack) {
  register long x0 __asm__("x0") = 0x10f00; // CLONE_VM|FS|FILES|SIGHAND|THREAD
  register long x1 __asm__("x1") = stack;
  register long x2 __asm__("x2") = 0;
  register long x3 __asm__("x3") = 0;
  register long x4 __asm__("x4") = 0;
  register long x8 __asm__("x8") = 220;
  __asm__ volatile("svc #0\ncbnz x0, 1f\nbl child_main\n1:"
                   : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
                   : "memory", "x30");
  return x0;
}

void _start(void) {
  unsigned long parent_list[3] = {0};
  if (sc6(99, (long)parent_list, 24, 0, 0, 0, 0)) end(1);
  long stack = sc6(222, 0, 65536, 3, 0x22, -1, 0);
  if (stack < 0) end(2);
  long tid = spawn(stack + 65536);
  if (tid <= 0) end(3);
  unsigned spins = 0;
  while (!ready && ++spins < 100000000) {}
  if (!ready) end(4);
  unsigned long head = 0, length = 0;
  if (sc6(100, tid, (long)&head, (long)&length, 0, 0, 0) ||
      head != (unsigned long)child_list || length != 24) end(5);
  if (sc6(100, 0, (long)&head, (long)&length, 0, 0, 0) ||
      head != (unsigned long)parent_list) end(6);
  release_child = 1;
  spins = 0;
  while (!child_done && ++spins < 100000000) {}
  if (!child_done) end(7);
  end(0);
}
