/* AArch64 guest probe: a tmpfs mount of /run/user/<uid> must materialize the
 * runtime directory with mode 0700 and report success (native logind semantics),
 * instead of returning EPERM and leaving /run/user empty.
 *
 * Built with: clang --target=aarch64-linux-gnu -nostdlib -static-pie \
 *             -fuse-ld=lld -Wl,-e,_start
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
  static const char ok[] = "RUNUSER_OK\n";
  if (!code) sc5(64, 1, (long)ok, sizeof ok - 1, 0, 0);
  sc5(93, code, 0, 0, 0, 0);
  for (;;) {}
}

/* aarch64 Linux `struct stat` (128 bytes), matching src/syscalls.c's linux_stat. */
struct linux_stat {
  unsigned long long st_dev;    /*  0 */
  unsigned long long st_ino;    /*  8 */
  unsigned int       st_mode;   /* 16 */
  unsigned int       st_nlink;  /* 20 */
  unsigned int       st_uid;    /* 24 */
  unsigned int       st_gid;    /* 28 */
  unsigned long long st_rdev;   /* 32 */
  unsigned long long __pad1;    /* 40 */
  long long          st_size;   /* 48 */
  int                st_blksize;/* 56 */
  int                __pad2;    /* 60 */
  long long          st_blocks; /* 64 */
  long long          st_atime_sec, st_atime_nsec;   /* 72, 80 */
  long long          st_mtime_sec, st_mtime_nsec;   /* 88, 96 */
  long long          st_ctime_sec, st_ctime_nsec;   /* 104, 112 */
  unsigned int       __unused4[2];                  /* 120 */
};

#define AT_FDCWD (-100)
#define S_IFDIR  0040000

void _start(void) {
  static const char source[] = "tmpfs";
  static const char target[] = "/run/user/1234";
  static const char fstype[] = "tmpfs";
  static const char data[] = "uid=1234,gid=1234,mode=0700";

  /* mount(source, target, fstype, flags, data) must materialize and succeed. */
  if (sc5(40, (long)source, (long)target, (long)fstype, 0, (long)data) != 0)
    done(1);

  /* newfstatat(AT_FDCWD, target, &st, 0) must see a directory, mode 0700. */
  struct linux_stat st;
  if (sc5(79, AT_FDCWD, (long)target, (long)&st, 0, 0) != 0) done(2);
  if ((st.st_mode & 0170000) != S_IFDIR) done(3);
  if ((st.st_mode & 0777) != 0700) done(4);

  done(0);
}
