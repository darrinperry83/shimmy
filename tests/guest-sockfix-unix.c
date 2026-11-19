/* ARM64 raw ABI: systemd socket activation for /etc/pacman.d/gnupg/S.* must work
 * even when the rootfs /etc/pacman.d is not writable. The shim relocates the
 * AF_UNIX socket (and its parent directory) into the per-boot socket group, so the
 * whole sequence — mkdir parent, bind, fchmodat, stat, unlink — must succeed.
 *
 * Emits "SOCKFIX_OK" on success.
 */
static long sc6(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                   : "memory");
  return x0;
}

#define SYS_mkdirat   34
#define SYS_unlinkat  35
#define SYS_fchmodat  53
#define SYS_close     57
#define SYS_write     64
#define SYS_newfstatat 79
#define SYS_exit      93
#define SYS_socket    198
#define SYS_bind      200

#define AT_FDCWD      (-100)
#define AF_UNIX       1
#define SOCK_STREAM   1
#define S_IFMT        0170000
#define S_IFSOCK      0140000

struct sockaddr_un_linux { unsigned short sun_family; char sun_path[108]; };

struct linux_stat {
  unsigned long long st_dev, st_ino;
  unsigned int st_mode, st_nlink, st_uid, st_gid;
  unsigned long long st_rdev, __pad1;
  long long st_size;
  int st_blksize, __pad2;
  long long st_blocks;
  long long st_atime_sec, st_atime_nsec, st_mtime_sec, st_mtime_nsec,
            st_ctime_sec, st_ctime_nsec;
  unsigned int __unused4[2];
};

static long sl(const char* s) { long n = 0; while (s[n]) n++; return n; }

static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "SOCKFIX_OK\n";
  if (!code) sc6(SYS_write, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(SYS_exit, code, 0, 0, 0, 0, 0);
  for (;;) {}
}

void _start(void) {
  const char* dir  = "/etc/pacman.d/gnupg";
  const char* path = "/etc/pacman.d/gnupg/S.test";
  long i;

  /* systemd's mkdir_parents() for the socket's parent directory. */
  if (sc6(SYS_mkdirat, AT_FDCWD, (long)dir, 0700, 0, 0, 0) != 0) done(1);

  struct sockaddr_un_linux addr;
  addr.sun_family = AF_UNIX;
  for (i = 0; i < 108; i++) addr.sun_path[i] = 0;
  for (i = 0; i <= sl(path); i++) addr.sun_path[i] = path[i];

  long fd = sc6(SYS_socket, AF_UNIX, SOCK_STREAM, 0, 0, 0, 0);
  if (fd < 0) done(2);
  long alen = 2 + sl(path) + 1;
  if (sc6(SYS_bind, fd, (long)&addr, alen, 0, 0, 0) != 0) done(3);
  if (sc6(SYS_fchmodat, AT_FDCWD, (long)path, 0600, 0, 0, 0) != 0) done(4);

  struct linux_stat st;
  if (sc6(SYS_newfstatat, AT_FDCWD, (long)path, (long)&st, 0, 0, 0) != 0) done(5);
  if ((st.st_mode & S_IFMT) != S_IFSOCK) done(6);

  if (sc6(SYS_unlinkat, AT_FDCWD, (long)path, 0, 0, 0, 0) != 0) done(7);
  sc6(SYS_close, fd, 0, 0, 0, 0, 0);
  done(0);
}
