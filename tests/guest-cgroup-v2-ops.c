/* AArch64 guest probe for the three cgroup v2 synthesis gaps systemd hits at
 * boot for every unit:
 *
 *   1. name_to_handle_at(2) on a cgroup dir   -> must return a stable handle
 *      (systemd reads the first 8 bytes as the cgroup ID), not ENOENT/EPERM.
 *   2. inotify_add_watch(2) on a cgroup dir   -> must return a real watch
 *      descriptor (the watch is an honest no-op that never fires).
 *   3. write(2) to memory.zswap.max / memory.oom.group / memory.zswap.writeback
 *      -> must accept the write and return the byte count (recording nothing,
 *      matching the shim's no-enforcement cgroup design).
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
  static const char ok[] = "CGROUPFIX_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
#define AT_FDCWD ((long)-100)
#define O_RDONLY  0
#define O_WRONLY  1
#define O_CLOEXEC 0x80000
#define IN_DELETE_SELF 0x400
struct file_handle { unsigned int handle_bytes; int handle_type; unsigned char f_handle[8]; };

static int same_handle(const struct file_handle* a, const struct file_handle* b) {
  if (a->handle_bytes != b->handle_bytes || a->handle_type != b->handle_type) return 0;
  for (int i = 0; i < 8; i++)
    if (a->f_handle[i] != b->f_handle[i]) return 0;
  return 1;
}

void _start(void) {
  static const char dir[] = "/sys/fs/cgroup/system.slice";
  static const char zswap_max[] = "/sys/fs/cgroup/system.slice/memory.zswap.max";
  static const char oom_group[] = "/sys/fs/cgroup/system.slice/memory.oom.group";
  static const char zswap_wb[]  = "/sys/fs/cgroup/system.slice/memory.zswap.writeback";
  static const char maxval[] = "max\n";
  static const char zeroval[] = "0\n";
  static const char oneval[]  = "1\n";

  /* 1. Create the child cgroup (in-memory registry, never the rootfs). */
  if (sc5(34, AT_FDCWD, (long)dir, 0755, 0, 0) != 0) done(1);

  /* 2. name_to_handle_at on the cgroup dir: success + a stable 8-byte handle. */
  struct file_handle fh1, fh2;
  fh1.handle_bytes = 8; fh1.handle_type = 0;
  fh2.handle_bytes = 8; fh2.handle_type = 0;
  int mount_id = 0;
  if (sc5(264, AT_FDCWD, (long)dir, (long)&fh1, (long)&mount_id, 0) != 0) done(2);
  if (fh1.handle_bytes != 8) done(3);
  /* A second call on the same path must reproduce the exact same ID. */
  if (sc5(264, AT_FDCWD, (long)dir, (long)&fh2, (long)&mount_id, 0) != 0) done(4);
  if (!same_handle(&fh1, &fh2)) done(5);

  /* 3. inotify_add_watch on the cgroup dir: a valid, inert watch descriptor. */
  long ifd = sc5(26, 0, 0, 0, 0, 0);   /* inotify_init1(0) */
  if (ifd < 0) done(6);
  long wd = sc5(27, ifd, (long)dir, IN_DELETE_SELF, 0, 0);
  if (wd < 0) done(7);

  /* 4. The three memory attribute files must accept writes (no-op records). */
  long f = sc5(56, AT_FDCWD, (long)zswap_max, O_WRONLY | O_CLOEXEC, 0, 0);
  if (f < 0) done(8);
  if (sc5(64, f, (long)maxval, 4, 0, 0) != 4) done(9);
  if (sc5(57, f, 0, 0, 0, 0) != 0) done(10);   /* close */

  f = sc5(56, AT_FDCWD, (long)oom_group, O_WRONLY | O_CLOEXEC, 0, 0);
  if (f < 0) done(11);
  if (sc5(64, f, (long)zeroval, 2, 0, 0) != 2) done(12);
  if (sc5(57, f, 0, 0, 0, 0) != 0) done(13);

  f = sc5(56, AT_FDCWD, (long)zswap_wb, O_WRONLY | O_CLOEXEC, 0, 0);
  if (f < 0) done(14);
  if (sc5(64, f, (long)oneval, 2, 0, 0) != 2) done(15);
  if (sc5(57, f, 0, 0, 0, 0) != 0) done(16);

  done(0);
}
