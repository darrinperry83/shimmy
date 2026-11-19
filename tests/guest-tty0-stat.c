/* ARM64 raw ABI: /dev/tty0 must stat as a Linux VC char device (4:0) through both
 * statx(2) and newfstatat(2), and pass access(F_OK) via faccessat(2). This is what
 * getty@.service's ConditionPathExists=/dev/tty0 depends on; before the shim synthesized
 * the node in the stat path, statx/newfstatat returned ENOENT and getty never started.
 */
static long sc4(long nr, long a, long b, long c, long d) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}

static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x8) : "memory");
  return x0;
}

#define SYS_statx        291
#define SYS_newfstatat   79
#define SYS_faccessat    48
#define SYS_write        64
#define SYS_exit         93
#define AT_FDCWD         (-100)
#define STATX_BASIC_STATS 0x7ffU
#define S_IFMT           0170000U
#define S_IFCHR          0020000U

/* Linux struct statx, 256 bytes: stx_mode at 28 (u16), stx_rdev_major at 128, minor at 132. */
static unsigned long sx[32];
/* Linux struct stat (aarch64), 128 bytes: st_mode at 16 (u32), st_rdev at 32 (u64). */
static unsigned long st[16];

static __attribute__((noreturn)) void finish(int code) {
  static const char ok[] = "TTY0_STAT_OK\n";
  static const char fail[] = "TTY0_STAT_FAIL\n";
  const char* m = code ? fail : ok;
  unsigned long n = code ? (sizeof fail - 1) : (sizeof ok - 1);
  sc4(SYS_write, 1, (long)m, n, 0);
  sc4(SYS_exit, code, 0, 0, 0);
  for (;;) {}
}

static unsigned short sx_mode(void) { return *(unsigned short*)((char*)sx + 28); }
static unsigned sx_rmajor(void) { return *(unsigned*)((char*)sx + 128); }
static unsigned sx_rminor(void) { return *(unsigned*)((char*)sx + 132); }
static unsigned st_mode(void)  { return *(unsigned*)((char*)st + 16); }
static unsigned long st_rdev(void) { return *(unsigned long*)((char*)st + 32); }

void _start(void) {
  /* statx: char device, major 4, minor 0. */
  if (sc5(SYS_statx, AT_FDCWD, (long)"/dev/tty0", 0, STATX_BASIC_STATS, (long)sx) != 0)
    finish(1);
  if ((sx_mode() & S_IFMT) != S_IFCHR) finish(2);
  if (sx_rmajor() != 4 || sx_rminor() != 0) finish(3);

  /* newfstatat: char device, rdev == makedev(4, 0) == 0x400. */
  if (sc4(SYS_newfstatat, AT_FDCWD, (long)"/dev/tty0", (long)st, 0) != 0)
    finish(4);
  if ((st_mode() & S_IFMT) != S_IFCHR) finish(5);
  if (st_rdev() != 0x400UL) finish(6);

  /* faccessat(F_OK): what systemd's ConditionPathExists actually issues via access(2). */
  if (sc4(SYS_faccessat, AT_FDCWD, (long)"/dev/tty0", 0, 0) != 0)
    finish(7);

  finish(0);
}
