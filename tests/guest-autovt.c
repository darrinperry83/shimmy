/* ARM64 raw ABI: VT_WAITACTIVE blocks until VT_ACTIVATE switches to the requested console.
 *
 * Sequence:
 *   1. open /dev/tty0 (the shim's current-console alias -> a pty slave owned by vt.c).
 *   2. spawn a CLONE_THREAD child that sleeps ~200ms, records `activated = 1`, then
 *      ioctl(VT_ACTIVATE, 2).
 *   3. the main thread blocks in ioctl(VT_WAITACTIVE, 2) and asserts it returns 0, and
 *      that it did NOT return before the child performed the activate (activated == 1).
 *   4. VT_GETSTATE must report v_active == 2.
 */
static long sc(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}

#define SYS_openat  56
#define SYS_ioctl   29
#define SYS_write   64
#define SYS_exit    93
#define SYS_nanosleep 101
#define SYS_clone   220

#define AT_FDCWD    (-100)
#define O_RDWR      2

#define VT_ACTIVATE    0x5606
#define VT_WAITACTIVE  0x5607
#define VT_GETSTATE    0x5603

static volatile int activated, ready;
static int tty_fd = -1;
static unsigned char stack[65536] __attribute__((aligned(16)));

static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "AUTOVT_GUEST_OK\n";
  static const char fail[] = "AUTOVT_GUEST_FAIL\n";
  const char* m = code ? fail : ok;
  unsigned long n = code ? (sizeof fail - 1) : (sizeof ok - 1);
  sc(SYS_write, 1, (long)m, (long)n, 0);
  sc(SYS_exit, code, 0, 0, 0);
  for (;;) {}
}

__attribute__((noreturn)) void child_main(void) {
  struct timespec { long long tv_sec, tv_nsec; } ts = { 0, 200000000LL };
  ready = 1;
  sc(SYS_nanosleep, (long)&ts, 0, 0, 0);
  activated = 1;
  sc(SYS_ioctl, tty_fd, VT_ACTIVATE, 2, 0);
  sc(SYS_exit, 0, 0, 0, 0);
  for (;;) {}
}

static long spawn(void) {
  register long x0 __asm__("x0") = 0x10f00; /* CLONE_VM|FS|FILES|SIGHAND|THREAD */
  register long x1 __asm__("x1") = (long)(stack + sizeof stack);
  register long x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
  register long x4 __asm__("x4") = 0, x8 __asm__("x8") = SYS_clone;
  __asm__ volatile("svc #0\ncbnz x0, 1f\nbl child_main\n1:"
                   : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
                   : "memory", "x30");
  return x0;
}

void _start(void) {
  tty_fd = (int)sc(SYS_openat, AT_FDCWD, (long)"/dev/tty0", O_RDWR, 0);
  if (tty_fd < 0) finish(1);

  if (spawn() <= 0) finish(2);
  unsigned spins = 0;
  while (!ready && ++spins < 100000000) {}
  if (!ready) finish(3);

  /* At this point the child is still sleeping: the activate has NOT happened yet. */
  long r = sc(SYS_ioctl, tty_fd, VT_WAITACTIVE, 2, 0);
  if (r != 0) finish(4);              /* must return 0 on wake, not an error */

  /* It must not have returned before the child performed VT_ACTIVATE. */
  if (!activated) finish(5);

  /* The shim must now report console 2 as the active one. */
  struct vt_stat { unsigned short v_active, v_signal, v_state; } st;
  if (sc(SYS_ioctl, tty_fd, VT_GETSTATE, (long)&st, 0) != 0) finish(6);
  if (st.v_active != 2) finish(7);

  finish(0);
}
