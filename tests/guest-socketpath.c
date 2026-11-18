/* ARM64 raw ABI: a bound AF_UNIX socket path must be visible to the file syscalls
 * that follow bind — chmod (fchmodat), stat (newfstatat), unlink (unlinkat) — even
 * though the socket lives in the shim's relocated namespace, not the rootfs.
 *
 * Sequence: socket(AF_UNIX, SOCK_STREAM) -> bind("/tmp/probe.sock") ->
 * fchmodat(AT_FDCWD, path, 0600) -> newfstatat(AT_FDCWD, path, S_IFSOCK) ->
 * unlinkat(AT_FDCWD, path). Each must return 0; stat must see a socket inode.
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

#define SYS_unlinkat    35
#define SYS_fchmodat    53
#define SYS_close       57
#define SYS_write       64
#define SYS_exit        93
#define SYS_newfstatat  79
#define SYS_socket      198
#define SYS_bind        200

#define AT_FDCWD       (-100)
#define AF_UNIX        1
#define SOCK_STREAM    1
#define S_IFMT         0170000
#define S_IFSOCK       0140000

struct sockaddr_un_linux {
  unsigned short sun_family;
  char sun_path[108];
};

struct linux_stat {
  unsigned long long st_dev;         /*  0 */
  unsigned long long st_ino;         /*  8 */
  unsigned int st_mode;              /* 16 */
  unsigned int st_nlink;             /* 20 */
  unsigned int st_uid;               /* 24 */
  unsigned int st_gid;               /* 28 */
  unsigned long long st_rdev;        /* 32 */
  unsigned long long __pad1;         /* 40 */
  long long st_size;                 /* 48 */
  int st_blksize;                    /* 56 */
  int __pad2;                        /* 60 */
  long long st_blocks;               /* 64 */
  long long st_atime_sec;            /* 72 */
  long long st_atime_nsec;           /* 80 */
  long long st_mtime_sec;            /* 88 */
  long long st_mtime_nsec;           /* 96 */
  long long st_ctime_sec;            /* 104 */
  long long st_ctime_nsec;           /* 112 */
  unsigned int __unused4[2];         /* 120 */
};

static long sl(const char* s) {
  long n = 0;
  while (s[n]) n++;
  return n;
}

static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "SOCKPATH_GUEST_OK\n";
  if (!code) sc6(SYS_write, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(SYS_exit, code, 0, 0, 0, 0, 0);
  for (;;) {}
}

void _start(void) {
  const char* path = "/tmp/probe.sock";
  struct sockaddr_un_linux addr;
  long i;
  addr.sun_family = AF_UNIX;
  for (i = 0; i < 108; i++) addr.sun_path[i] = 0;
  for (i = 0; i <= sl(path); i++) addr.sun_path[i] = path[i];

  long fd = sc6(SYS_socket, AF_UNIX, SOCK_STREAM, 0, 0, 0, 0);
  if (fd < 0) done(1);

  long alen = 2 + sl(path) + 1;      /* family + path + NUL, as Linux bind expects */
  if (sc6(SYS_bind, fd, (long)&addr, alen, 0, 0, 0) != 0) done(2);

  if (sc6(SYS_fchmodat, AT_FDCWD, (long)path, 0600, 0, 0, 0) != 0) done(3);

  struct linux_stat st;
  if (sc6(SYS_newfstatat, AT_FDCWD, (long)path, (long)&st, 0, 0, 0) != 0) done(4);
  if ((st.st_mode & S_IFMT) != S_IFSOCK) done(5);

  if (sc6(SYS_unlinkat, AT_FDCWD, (long)path, 0, 0, 0, 0) != 0) done(6);

  sc6(SYS_close, fd, 0, 0, 0, 0, 0);
  done(0);
}
