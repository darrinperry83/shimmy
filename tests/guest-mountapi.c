/* AArch64 guest probe for the honest new mount API (fsopen/fsconfig/fsmount/move_mount).
 *
 * systemd/libmount mount /tmp via fsopen("tmpfs") -> fsconfig -> fsconfig(CMD_CREATE)
 * -> fsmount -> move_mount(mfd, "", AT_FDCWD, "/tmp", MOVE_MOUNT_F_EMPTY_PATH). The shim
 * must refuse a tmpfs whose destination is /tmp with EPERM, exactly like mount(2)
 * (case 40), while a tmpfs onto /run/user/<uid> materializes the runtime directory and
 * synthetic API filesystems keep their no-op success.
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
  static const char ok[] = "GUEST_MOUNTAPI_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}

#define AT_FDCWD ((long)-100)
#define FSOPEN_CLOEXEC    0x00000001
#define FSMOUNT_CLOEXEC   0x00000001
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#define FSCONFIG_SET_STRING 1
#define FSCONFIG_CMD_CREATE 6

static long mount_via_new_api(const char *fstype, const char *target, const char *opt) {
  long fsfd = sc5(430, (long)fstype, FSOPEN_CLOEXEC, 0, 0, 0);   /* fsopen */
  if (fsfd < 0) return fsfd;
  if (opt && sc5(431, fsfd, FSCONFIG_SET_STRING, (long)"mode", (long)opt, 0) != 0)   /* fsconfig */
    return -100;
  if (sc5(431, fsfd, FSCONFIG_CMD_CREATE, 0, 0, 0) != 0)         /* fsconfig CREATE */
    return -101;
  long mfd = sc5(432, fsfd, FSMOUNT_CLOEXEC, 0, 0, 0);           /* fsmount */
  sc5(57, fsfd, 0, 0, 0, 0);                                     /* close fsopen fd */
  if (mfd < 0) return mfd;
  long r = sc5(429, mfd, (long)"", AT_FDCWD, (long)target,       /* move_mount */
               MOVE_MOUNT_F_EMPTY_PATH);
  sc5(57, mfd, 0, 0, 0, 0);                                     /* close fsmount fd */
  return r;
}

void _start(void) {
  /* A NULL/bad fstype pointer must be refused, not a phantom success. */
  if (sc5(430, 0, FSOPEN_CLOEXEC, 0, 0, 0) != -14) done(1);     /* fsopen(NULL) -> EFAULT */

  /* tmpfs on /tmp must be refused with EPERM; a tmpfs on /run/user/<uid> now
   * materializes the runtime directory and succeeds (native logind semantics). */
  if (mount_via_new_api("tmpfs", "/tmp", 0) != -1) done(2);      /* -EPERM */
  if (mount_via_new_api("tmpfs", "/run/user/22001", 0) != 0) done(3);
  /* A non-uid /run/user suffix is not the per-user runtime dir: keep no-op success. */
  if (mount_via_new_api("tmpfs", "/run/user/not-a-uid", 0) != 0) done(4);

  /* Synthetic API filesystems keep their existing no-op success. */
  if (mount_via_new_api("proc", "/proc", 0) != 0) done(5);
  if (mount_via_new_api("tmpfs", "/dev/shm", "1777") != 0) done(6);

  /* A bad move_mount target pointer must be -EFAULT. */
  long fsfd = sc5(430, (long)"tmpfs", FSOPEN_CLOEXEC, 0, 0, 0);
  if (fsfd < 0) done(7);
  if (sc5(431, fsfd, FSCONFIG_CMD_CREATE, 0, 0, 0) != 0) done(8);
  long mfd = sc5(432, fsfd, FSMOUNT_CLOEXEC, 0, 0, 0);
  if (mfd < 0) done(9);
  if (sc5(429, mfd, (long)"", AT_FDCWD, 0, MOVE_MOUNT_F_EMPTY_PATH) != -14) done(10);
  sc5(57, fsfd, 0, 0, 0, 0);
  sc5(57, mfd, 0, 0, 0, 0);

  done(0);
}
