/* ARM64 raw ABI: socket-activation sequence for /etc/pacman.d/gnupg/S.* must work,
 * including the FD-based fchmod/fchown that systemd does after bind (SocketMode=0600).
 * macOS returns EINVAL for fchmod/fchown on a socket fd; the shim must treat both as
 * successful no-ops (Linux semantics) or gpg-agent/dirmngr socket units abort. */
static long sc6(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x5 __asm__("x5") = f, x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}
static int sl(const char* s) { int i = 0; while (s[i]) i++; return i; }
static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "SOCK_FCHMOD_OK\n";
  if (!code) sc6(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(93, code, 0, 0, 0, 0, 0);
  for (;;) {}
}
#define SYS_mkdirat  34
#define SYS_socket   198
#define SYS_bind     200
#define SYS_fchmod   52
#define SYS_fchown   55
struct sun { unsigned short fam; char path[108]; };
void _start(void) {
  const char* dir  = "/etc/pacman.d/gnupg";
  const char* path = "/etc/pacman.d/gnupg/S.fchmodtest";
  (void)sc6(SYS_mkdirat, -100, (long)dir, 0700, 0, 0, 0);   /* EEXIST/EACCES both ok */
  long fd = sc6(SYS_socket, 1, 1, 0, 0, 0, 0);              /* AF_UNIX SOCK_STREAM */
  if (fd < 0) done(1);
  struct sun a; for (int i = 0; i < 108; i++) a.path[i] = 0;
  for (int i = 0; i <= sl(path); i++) a.path[i] = path[i];
  a.fam = 1;
  if (sc6(SYS_bind, fd, (long)&a, 2 + sl(path) + 1, 0, 0, 0) != 0) done(2);
  if (sc6(SYS_fchmod, fd, 0600, 0, 0, 0, 0) != 0) done(3);  /* must be a no-op success */
  if (sc6(SYS_fchown, fd, 0, 0, 0, 0, 0) != 0) done(4);     /* must be a no-op success */
  done(0);
}
