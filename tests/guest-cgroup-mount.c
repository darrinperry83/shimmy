/* AArch64 guest probe for the systemd-261 cgroup2 mount sequence.
 *
 * systemd 261 mounts cgroup2 with mount_nofollow(): it opens the mountpoint as
 * an O_PATH|O_NOFOLLOW pin, then issues mount(2) against /proc/self/fd/<n>.
 * The shim's cgroup v2 tree is synthesized in-memory, so both the pin open and
 * the mount(2) must be no-op successes, after which the synthesized
 * cgroup.controllers file must be readable.
 */
static long sc5(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "GUEST_CGROUP_MOUNT_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
#define AT_FDCWD ((long)-100)
#define O_RDONLY   0
#define O_PATH     0x200000
#define O_DIRECTORY 0x4000
#define O_NOFOLLOW 0x8000
#define O_CLOEXEC  0x80000
/* MS_NOSUID|MS_NODEV|MS_NOEXEC, as systemd passes for cgroup2. */
#define CGROUP_MS_FLAGS 0xe
void _start(void) {
  static const char root[] = "/sys/fs/cgroup";
  static const char controllers[] = "/sys/fs/cgroup/cgroup.controllers";
  static const char fstype[] = "cgroup2";
  static const char opts[] = "nsdelegate,memory_recursiveprot";

  /* 1. mount_nofollow() pins the mountpoint with O_PATH|O_NOFOLLOW first. */
  long pin = sc5(56, AT_FDCWD, (long)root, O_PATH | O_NOFOLLOW | O_CLOEXEC, 0, 0);
  if (pin < 0) done(1);
  if (sc5(57, pin, 0, 0, 0, 0) != 0) done(2); /* close */

  /* 2. mount(2) for cgroup2 must be a no-op success. */
  if (sc5(40, (long)fstype, (long)root, (long)fstype, CGROUP_MS_FLAGS, (long)opts) != 0) done(3);

  /* 3. The synthesized cgroup.controllers must be readable after the mount. */
  long fd = sc5(56, AT_FDCWD, (long)controllers, O_RDONLY | O_CLOEXEC, 0, 0);
  if (fd < 0) done(4);
  char buf[64];
  long n = sc5(63, fd, (long)buf, sizeof buf - 1, 0, 0);
  if (n <= 0) done(5);
  buf[n] = 0;
  int has_cpu = 0;
  for (long i = 0; i + 3 <= n; i++)
    if (buf[i] == 'c' && buf[i + 1] == 'p' && buf[i + 2] == 'u') has_cpu = 1;
  if (!has_cpu) done(6);
  done(0);
}
