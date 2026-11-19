/* AArch64 guest probe for the shim's user/cgroup namespace sandboxing surface.
 *
 * systemd-executor applies PrivateUsers=yes / ProtectControlGroups=true before
 * exec'ing the unit: it unshare(CLONE_NEWUSER), then writes "0 <uid> 1" to
 * /proc/self/uid_map and /proc/self/gid_map (after "deny" to /proc/self/setgroups),
 * and unshare(CLONE_NEWCGROUP) for the cgroup namespace. The shim has no real
 * kernel namespaces, but it already models a virtual uid identity and a private
 * cgroup v2 tree, so each of these must be a no-op success and the id-map writes
 * must return their byte count (never EPERM/ENOENT, which systemd loops on).
 *
 * Also writes /proc/<pid>/uid_map (the path the (sd-userns) helper really uses),
 * which the shim normalizes to /proc/self/uid_map.
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
  static const char ok[] = "NSFIX_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
#define AT_FDCWD ((long)-100)
#define O_WRONLY  1
#define O_CLOEXEC 0x80000

#define CLONE_NEWNS     0x00020000
#define CLONE_NEWCGROUP 0x02000000
#define CLONE_NEWUSER   0x10000000

/* Open `path` for writing, write `data` (`len` bytes), close, and require the
 * write to return exactly `len`. Returns 0 on success, non-zero on failure. */
static int write_map(const char* path, const char* data, long len) {
  long fd = sc5(56, AT_FDCWD, (long)path, O_WRONLY | O_CLOEXEC, 0, 0);
  if (fd < 0) return 1;
  long n = sc5(64, fd, (long)data, len, 0, 0);
  sc5(57, fd, 0, 0, 0, 0);
  return n == len ? 0 : 1;
}

void _start(void) {
  static const char setgroups[] = "/proc/self/setgroups";
  static const char gid_map[]   = "/proc/self/gid_map";
  static const char uid_map[]   = "/proc/self/uid_map";
  static const char deny[]      = "deny";
  static const char m01[]       = "0 0 1\n";

  /* 1. PrivateUsers=yes: the user namespace is a no-op success. */
  if (sc5(97, CLONE_NEWUSER, 0, 0, 0, 0) != 0) done(1);

  /* 2. The id-map files accept the standard writes (setgroups first, like the kernel). */
  if (write_map(setgroups, deny, 4)) done(2);
  if (write_map(gid_map, m01, 6)) done(3);
  if (write_map(uid_map, m01, 6)) done(4);

  /* 3. ProtectControlGroups=true: the cgroup namespace is a no-op success. */
  if (sc5(97, CLONE_NEWCGROUP, 0, 0, 0, 0) != 0) done(5);

  /* 4. CLONE_NEWNS must stay unsupported (there is no mount namespace to unshare):
   * systemd then skips mount namespacing instead of failing on /proc/self/mountinfo. */
  if (sc5(97, CLONE_NEWNS, 0, 0, 0, 0) == 0) done(6);

  /* 5. The (sd-userns) helper writes /proc/<pid>/uid_map, not /proc/self/. */
  {
    long pid = sc5(172, 0, 0, 0, 0, 0); /* getpid */
    char path[64];
    int p = 0, i;
    const char* pre = "/proc/";
    for (i = 0; pre[i]; i++) path[p++] = pre[i];
    long v = pid;
    char digits[20];
    int di = 0;
    do { digits[di++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (di) path[p++] = digits[--di];
    const char* suf = "/uid_map";
    for (i = 0; suf[i]; i++) path[p++] = suf[i];
    path[p] = 0;
    if (write_map(path, m01, 6)) done(7);
  }

  done(0);
}
