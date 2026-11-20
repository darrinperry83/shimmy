/* AArch64 guest probe for the two remaining cgroup-v2 synthesis gaps systemd
 * hits for every unit at boot:
 *
 *   1. write(2) to a child cgroup's memory.zswap.max must return the byte count
 *      (3 for "max"), not EINVAL — and a later read must reflect that value.
 *   2. name_to_handle_at(2) on a registered child cgroup must return a stable,
 *      non-empty handle with handle_bytes > 0 (systemd reads the first 8 bytes
 *      as the cgroup ID), not ENOENT.
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
  static const char ok[] = "CGNOISE_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}
#define AT_FDCWD ((long)-100)
#define O_RDONLY   0
#define O_WRONLY   1
#define O_CLOEXEC  0x80000
struct file_handle { unsigned int handle_bytes; int handle_type; unsigned char f_handle[8]; };

void _start(void) {
  static const char dir[] = "/sys/fs/cgroup/cgnoise.test";
  static const char zswap_max[] = "/sys/fs/cgroup/cgnoise.test/memory.zswap.max";
  static const char maxval[] = "max";

  /* 1. Create the child cgroup (in-memory registry, never the rootfs). */
  if (sc5(34, AT_FDCWD, (long)dir, 0755, 0, 0) != 0) done(1);

  /* 2. Write "max" to memory.zswap.max: must succeed with the byte count 3,
   *    not EINVAL (-22). */
  long f = sc5(56, AT_FDCWD, (long)zswap_max, O_WRONLY | O_CLOEXEC, 0, 0);
  if (f < 0) done(2);
  if (sc5(64, f, (long)maxval, 3, 0, 0) != 3) done(3);
  if (sc5(57, f, 0, 0, 0, 0) != 0) done(4);   /* close */

  /* 3. Read it back and assert the written value is reflected. */
  long rfd = sc5(56, AT_FDCWD, (long)zswap_max, O_RDONLY | O_CLOEXEC, 0, 0);
  if (rfd < 0) done(5);
  char buf[64];
  long n = sc5(63, rfd, (long)buf, sizeof buf - 1, 0, 0);
  if (n != 3) done(6);
  buf[n] = 0;
  if (buf[0] != 'm' || buf[1] != 'a' || buf[2] != 'x') done(7);
  if (sc5(57, rfd, 0, 0, 0, 0) != 0) done(8);

  /* 4. name_to_handle_at on the child cgroup: success + a non-empty handle. */
  struct file_handle fh;
  fh.handle_bytes = 128;                        /* systemd's ORIGINAL_MAX_HANDLE_SZ */
  fh.handle_type = 0;
  int mount_id = 0;
  if (sc5(264, AT_FDCWD, (long)dir, (long)&fh, (long)&mount_id, 0) != 0) done(9);
  if (fh.handle_bytes != 8) done(10);           /* 8-byte cgroup ID handle */
  int nonempty = 0;
  for (int i = 0; i < 8; i++)
    if (fh.f_handle[i] != 0) nonempty = 1;
  if (!nonempty) done(11);

  done(0);
}
