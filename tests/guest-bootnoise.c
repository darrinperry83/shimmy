/* AArch64 guest probe for the three classes of boot-time error noise cleared
 * on the feature/bootnoise branch:
 *
 *   1. write(2) to /sys/fs/cgroup/system.slice/cpu.idle -> accepted (the file
 *      is a writable no-op leaf, content "0\n"), not EINVAL.
 *   2. setsockopt(IPV6_RECVPKTINFO) and setsockopt(IPV6_RECVHOPLIMIT) on an
 *      AF_INET6 datagram socket -> success (mapped to Darwin's RFC 3542
 *      equivalents), not ENOPROTOOPT.
 *   3. inotify_add_watch("/run/udev") and inotify_add_watch("/var/run/utmp")
 *      -> valid, never-firing no-op watch descriptors, not EBADF.
 */
static long sc(long nr, long a, long b, long c, long d, long e) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "BOOTNOISE_OK\n";
  if (!code) sc(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc(93, code, 0, 0, 0, 0);
  for (;;) {}
}
#define AT_FDCWD        ((long)-100)
#define O_WRONLY        1
#define O_CLOEXEC       0x80000
#define AF_INET6        10
#define SOCK_DGRAM      2
#define SOL_IPV6        41
#define IPV6_RECVHOPLIMIT 37
#define IPV6_RECVPKTINFO  49
#define IN_MODIFY       0x2
#define IN_DELETE       0x200

void _start(void) {
  static const char dir[]   = "/sys/fs/cgroup/system.slice";
  static const char idle[]  = "/sys/fs/cgroup/system.slice/cpu.idle";
  static const char udev[]  = "/run/udev";
  static const char utmp[]  = "/var/run/utmp";
  static const char zeroval[] = "0\n";
  int one = 1;

  /* 1. cpu.idle must exist and accept the "0" write systemd performs. */
  if (sc(34, AT_FDCWD, (long)dir, 0755, 0, 0) != 0) done(1);
  long f = sc(56, AT_FDCWD, (long)idle, O_WRONLY | O_CLOEXEC, 0, 0);
  if (f < 0) done(2);
  if (sc(64, f, (long)zeroval, 2, 0, 0) != 2) done(3);
  if (sc(57, f, 0, 0, 0, 0) != 0) done(4);

  /* 2. IPv6 packet-info / hop-limit ancillary requests must succeed. */
  long s = sc(198, AF_INET6, SOCK_DGRAM, 0, 0, 0);
  if (s < 0) done(5);
  if (sc(208, s, SOL_IPV6, IPV6_RECVPKTINFO, (long)&one, 4) != 0) done(6);
  if (sc(208, s, SOL_IPV6, IPV6_RECVHOPLIMIT, (long)&one, 4) != 0) done(7);
  if (sc(57, s, 0, 0, 0, 0) != 0) done(8);

  /* 3. /run/udev and /var/run/utmp watches must return valid wds. */
  long ifd = sc(26, 0, 0, 0, 0, 0);
  if (ifd < 0) done(9);
  if (sc(27, ifd, (long)udev, IN_DELETE, 0, 0) < 0) done(10);
  if (sc(27, ifd, (long)utmp, IN_MODIFY, 0, 0) < 0) done(11);

  done(0);
}
