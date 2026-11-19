/* AArch64 guest probe for no-op cgroup v2 process attachment.
 *
 * The shim's /sys/fs/cgroup tree is synthesized in-memory and enforces no
 * resource limits, so "moving" a process into a cgroup is a no-op that must
 * nonetheless accept the cgroup.procs write and record the mapping for
 * /proc/<pid>/cgroup read-back. This mirrors exactly what systemd 261 does
 * during unit startup: mkdir the cgroup, open cgroup.procs O_WRONLY, write a
 * pid (0 == self), then read /proc/self/cgroup.
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
  static const char ok[] = "GUEST_CGROUP_ATTACH_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
#define AT_FDCWD ((long)-100)
#define O_RDONLY   0
#define O_WRONLY   1
#define O_CLOEXEC  0x80000
void _start(void) {
  static const char dir[] = "/sys/fs/cgroup/test";
  static const char procs[] = "/sys/fs/cgroup/test/cgroup.procs";
  static const char selfcgroup[] = "/proc/self/cgroup";
  static const char zero[] = "0\n";

  /* 1. Create the child cgroup (in-memory registry, never the rootfs). */
  if (sc5(34, AT_FDCWD, (long)dir, 0755, 0, 0) != 0) done(1);

  /* 2. Open cgroup.procs for writing, like systemd's cg_attach(). */
  long fd = sc5(56, AT_FDCWD, (long)procs, O_WRONLY | O_CLOEXEC, 0, 0);
  if (fd < 0) done(2);

  /* 3. Write 0 == self; the write must succeed. */
  if (sc5(64, fd, (long)zero, 2, 0, 0) != 2) done(3);
  if (sc5(57, fd, 0, 0, 0, 0) != 0) done(4); /* close */

  /* 4. Read /proc/self/cgroup and require the recorded "/test" path. */
  long rfd = sc5(56, AT_FDCWD, (long)selfcgroup, O_RDONLY | O_CLOEXEC, 0, 0);
  if (rfd < 0) done(5);
  char buf[128];
  long n = sc5(63, rfd, (long)buf, sizeof buf - 1, 0, 0);
  if (n <= 0) done(6);
  buf[n] = 0;
  int found = 0;
  for (long i = 0; i + 4 < n; i++)
    if (buf[i] == '/' && buf[i + 1] == 't' && buf[i + 2] == 'e' &&
        buf[i + 3] == 's' && buf[i + 4] == 't') found = 1;
  if (!found) done(7);
  done(0);
}
