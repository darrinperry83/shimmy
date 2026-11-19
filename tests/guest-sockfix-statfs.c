/* ARM64 raw ABI: statfs("/run") must report a temporary filesystem (tmpfs),
 * not a disk filesystem, so systemd's rm_rf() does not refuse to clean
 * /run/credentials. Also verify /proc/self/mounts lists /run as tmpfs.
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

#define SYS_statfs   43
#define SYS_openat   56
#define SYS_close    57
#define SYS_read     63
#define SYS_write    64
#define SYS_exit     93

#define AT_FDCWD     (-100)
#define O_RDONLY     0

/* Linux statfs: 120 bytes, first field is f_type (int64). */
struct linux_statfs {
  long long f_type, f_bsize;
  unsigned long long f_blocks, f_bfree, f_bavail, f_files, f_ffree;
  int f_fsid[2];
  long long f_namelen, f_frsize, f_flags, f_spare[4];
};

#define TMPFS_MAGIC  0x01021994LL
#define EXT4_MAGIC   0x0000EF53LL

static long sl(const char* s) { long n = 0; while (s[n]) n++; return n; }

static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "SOCKFIX_OK\n";
  if (!code) sc6(SYS_write, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(SYS_exit, code, 0, 0, 0, 0, 0);
  for (;;) {}
}

void _start(void) {
  struct linux_statfs st;
  if (sc6(SYS_statfs, (long)"/run", (long)&st, 0, 0, 0, 0) != 0) done(1);
  if (st.f_type != TMPFS_MAGIC) done(2);

  /* /proc/self/mounts must list /run as tmpfs too. */
  long fd = sc6(SYS_openat, AT_FDCWD, (long)"/proc/self/mounts", O_RDONLY, 0, 0, 0);
  if (fd < 0) done(3);
  char buf[256];
  long n = sc6(SYS_read, fd, (long)buf, sizeof buf - 1, 0, 0, 0);
  sc6(SYS_close, fd, 0, 0, 0, 0, 0);
  if (n < 0) done(4);
  buf[n] = 0;

  /* Look for the "tmpfs /run tmpfs ..." line. */
  for (long i = 0; i + 10 < n; i++) {
    if (buf[i] == 't' && buf[i+1] == 'm' && buf[i+2] == 'p' && buf[i+3] == 'f' &&
        buf[i+4] == 's' && buf[i+5] == ' ' && buf[i+6] == '/' && buf[i+7] == 'r' &&
        buf[i+8] == 'u' && buf[i+9] == 'n') {
      done(0);
    }
  }
  done(5);
}
