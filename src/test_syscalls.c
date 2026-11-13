// test_syscalls.c — standalone driver for syscalls.c.
//
// Builds its own throwaway guest rootfs under /Users/dperry/linux-shim/tests/ (never /tmp —
// that gets purged), then drives linux_syscall() directly with LINUX numbers and LINUX flag
// values and checks the raw Linux-ABI results.
//
// Build & run:
//   clang -arch arm64 -O2 -Wall -o ../tests/test_syscalls test_syscalls.c syscalls.c && \
//   ../tests/test_syscalls
//
// gmem_* and linux_syscall_ext() are stubbed at the bottom so this links without the rest of
// the shim.

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>

#include "syscalls.h"

// ---------------------------------------------------------------------------------------
// Linux syscall numbers (aarch64)
// ---------------------------------------------------------------------------------------
enum {
  SYS_getcwd = 17, SYS_fcntl = 25, SYS_ioctl = 29, SYS_flock = 32, SYS_mkdirat = 34,
  SYS_unlinkat = 35, SYS_linkat = 37, SYS_renameat = 38, SYS_statfs = 43, SYS_fstatfs = 44,
  SYS_ftruncate = 46, SYS_faccessat = 48, SYS_chdir = 49, SYS_openat = 56, SYS_close = 57,
  SYS_getdents64 = 61, SYS_lseek = 62, SYS_read = 63, SYS_write = 64, SYS_readv = 65,
  SYS_writev = 66, SYS_pread64 = 67, SYS_pwrite64 = 68, SYS_readlinkat = 78,
  SYS_newfstatat = 79, SYS_fstat = 80, SYS_set_tid_address = 96, SYS_futex = 98,
  SYS_set_robust_list = 99, SYS_clock_gettime = 113, SYS_clock_getres = 114,
  SYS_clock_nanosleep = 115, SYS_sched_setaffinity = 122, SYS_sched_getaffinity = 123,
  SYS_kill = 129, SYS_rt_sigaction = 134, SYS_rt_sigprocmask = 135, SYS_rt_sigtimedwait = 137,
  SYS_times = 153, SYS_uname = 160, SYS_getrusage = 165, SYS_getpid = 172, SYS_getppid = 173,
  SYS_getuid = 174, SYS_geteuid = 175, SYS_getgid = 176, SYS_getegid = 177, SYS_gettid = 178,
  SYS_sysinfo = 179, SYS_brk = 214, SYS_munmap = 215, SYS_mremap = 216, SYS_mmap = 222,
  SYS_mprotect = 226, SYS_madvise = 233, SYS_prlimit64 = 261, SYS_getrandom = 278,
  SYS_rseq = 293,
};

// Linux flag values as the guest would pass them.
#define L_AT_FDCWD            (-100)
#define L_AT_SYMLINK_NOFOLLOW 0x100
#define L_AT_REMOVEDIR        0x200
#define L_AT_EMPTY_PATH       0x1000
#define L_O_RDONLY    0x0
#define L_O_WRONLY    0x1
#define L_O_RDWR      0x2
#define L_O_CREAT     0x40
#define L_O_EXCL      0x80
#define L_O_TRUNC     0x200
#define L_O_APPEND    0x400
#define L_O_NONBLOCK  0x800
#define L_O_DIRECTORY 0x4000
#define L_O_CLOEXEC   0x80000

#define SC(nr, ...) linux_syscall((nr), __VA_ARGS__)
#define SC0(nr) linux_syscall((nr), 0, 0, 0, 0, 0, 0)
#define U(x) ((unsigned long)(long)(x))
#define P(x) ((unsigned long)(uintptr_t)(x))

// ---------------------------------------------------------------------------------------
// The Linux aarch64 struct stat, declared independently here so the offsets are checked
// against what Linux actually expects rather than against syscalls.c's own opinion.
// ---------------------------------------------------------------------------------------
struct lx_stat {
  uint64_t st_dev; uint64_t st_ino; uint32_t st_mode; uint32_t st_nlink;
  uint32_t st_uid; uint32_t st_gid; uint64_t st_rdev; uint64_t __pad1;
  int64_t st_size; int32_t st_blksize; int32_t __pad2; int64_t st_blocks;
  int64_t st_atime_sec, st_atime_nsec, st_mtime_sec, st_mtime_nsec,
          st_ctime_sec, st_ctime_nsec;
  uint32_t __unused4[2];
};
_Static_assert(sizeof(struct lx_stat) == 128, "linux stat is 128 bytes");
_Static_assert(offsetof(struct lx_stat, st_dev)        ==   0, "st_dev @0");
_Static_assert(offsetof(struct lx_stat, st_ino)        ==   8, "st_ino @8");
_Static_assert(offsetof(struct lx_stat, st_mode)       ==  16, "st_mode @16");
_Static_assert(offsetof(struct lx_stat, st_nlink)      ==  20, "st_nlink @20");
_Static_assert(offsetof(struct lx_stat, st_uid)        ==  24, "st_uid @24");
_Static_assert(offsetof(struct lx_stat, st_gid)        ==  28, "st_gid @28");
_Static_assert(offsetof(struct lx_stat, st_rdev)       ==  32, "st_rdev @32");
_Static_assert(offsetof(struct lx_stat, st_size)       ==  48, "st_size @48");
_Static_assert(offsetof(struct lx_stat, st_blksize)    ==  56, "st_blksize @56");
_Static_assert(offsetof(struct lx_stat, st_blocks)     ==  64, "st_blocks @64");
_Static_assert(offsetof(struct lx_stat, st_atime_sec)  ==  72, "st_atim @72");
_Static_assert(offsetof(struct lx_stat, st_atime_nsec) ==  80, "st_atim.nsec @80");
_Static_assert(offsetof(struct lx_stat, st_mtime_sec)  ==  88, "st_mtim @88");
_Static_assert(offsetof(struct lx_stat, st_mtime_nsec) ==  96, "st_mtim.nsec @96");
_Static_assert(offsetof(struct lx_stat, st_ctime_sec)  == 104, "st_ctim @104");
_Static_assert(offsetof(struct lx_stat, st_ctime_nsec) == 112, "st_ctim.nsec @112");
_Static_assert(offsetof(struct lx_stat, __unused4)     == 120, "pad @120");

struct lx_dirent64 { uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type;
                     char d_name[]; };
_Static_assert(offsetof(struct lx_dirent64, d_ino)    ==  0, "d_ino @0");
_Static_assert(offsetof(struct lx_dirent64, d_off)    ==  8, "d_off @8");
_Static_assert(offsetof(struct lx_dirent64, d_reclen) == 16, "d_reclen @16");
_Static_assert(offsetof(struct lx_dirent64, d_type)   == 18, "d_type @18");
_Static_assert(offsetof(struct lx_dirent64, d_name)   == 19, "d_name @19");

struct lx_utsname { char sysname[65], nodename[65], release[65], version[65],
                         machine[65], domainname[65]; };
struct lx_timespec { int64_t tv_sec, tv_nsec; };
struct lx_statfs { int64_t f_type, f_bsize; uint64_t f_blocks, f_bfree, f_bavail,
                   f_files, f_ffree; int32_t f_fsid[2];
                   int64_t f_namelen, f_frsize, f_flags, f_spare[4]; };
struct lx_rlimit64 { uint64_t rlim_cur, rlim_max; };
struct lx_rusage { int64_t utv_sec, utv_usec, stv_sec, stv_usec, rest[14]; };
struct lx_tms { int64_t utime, stime, cutime, cstime; };

// ---------------------------------------------------------------------------------------
// tiny harness
// ---------------------------------------------------------------------------------------
static int g_pass, g_fail;
static const char* g_sect = "";

static void section(const char* s) { g_sect = s; printf("\n--- %s ---\n", s); }

__attribute__((format(printf, 2, 3)))
static void ok(int cond, const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  char msg[512]; vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
  if (cond) { g_pass++; printf("  ok   %s\n", msg); }
  else      { g_fail++; printf("  FAIL [%s] %s\n", g_sect, msg); }
}

#define OK(cond, ...) ok((cond) ? 1 : 0, __VA_ARGS__)

// ---------------------------------------------------------------------------------------
// rootfs fixture
// ---------------------------------------------------------------------------------------
#define TESTDIR "/Users/dperry/linux-shim/tests"
#define ROOT    TESTDIR "/rootfs_syscalls"

static const char* HELLO = "hello from the guest fs\n";

static void rmrf(const char* p) {
  char cmd[2048];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", p);
  if (system(cmd) != 0) { /* first run: nothing to remove */ }
}

static void wfile(const char* path, const char* data, size_t n, mode_t mode) {
  FILE* f = fopen(path, "wb");
  if (!f) { perror(path); exit(2); }
  if (n && fwrite(data, 1, n, f) != n) { perror("fwrite"); exit(2); }
  fclose(f);
  chmod(path, mode);
}

static void make_rootfs(void) {
  rmrf(ROOT);
  mkdir(TESTDIR, 0755);
  if (mkdir(ROOT, 0755) != 0) { perror("mkdir " ROOT); exit(2); }
  mkdir(ROOT "/dir", 0755);
  mkdir(ROOT "/dir/sub", 0755);
  mkdir(ROOT "/etc", 0755);
  mkdir(ROOT "/bin", 0755);
  wfile(ROOT "/hello.txt", HELLO, strlen(HELLO), 0644);
  wfile(ROOT "/dir/a.txt", "alpha", 5, 0644);
  wfile(ROOT "/dir/b.bin", "\0\1\2\3\4\5\6\7", 8, 0600);
  wfile(ROOT "/bin/testprog", "\177ELF-not-really", 15, 0755);
  wfile(ROOT "/etc/hostname", "guest\n", 6, 0644);
  if (symlink("a.txt", ROOT "/dir/link") != 0) { perror("symlink"); exit(2); }
}

// ---------------------------------------------------------------------------------------
int main(void) {
  printf("== test_syscalls: Linux aarch64 -> macOS core translation ==\n");
  make_rootfs();
  syscalls_set_root(ROOT);
  syscalls_set_exe("/bin/testprog");

  char buf[8192];
  long r;

  // ------------------------------------------------------------------ struct layouts
  section("struct layouts");
  OK(sizeof(struct lx_stat) == 128, "sizeof(struct stat) == 128");
  OK(offsetof(struct lx_stat, st_size) == 48, "st_size at byte 48");
  OK(offsetof(struct lx_stat, st_mtime_sec) == 88, "st_mtim at byte 88");
  OK(offsetof(struct lx_dirent64, d_name) == 19, "linux_dirent64 d_name at byte 19");

  // ------------------------------------------------------------------ errno table
  section("errno translation");
  struct { int host; int lx; const char* n; } et[] = {
    { 1, 1, "EPERM" }, { 2, 2, "ENOENT" }, { 9, 9, "EBADF" },
    { 11, 35, "EDEADLK 11 -> 35" }, { 35, 11, "EAGAIN 35 -> 11" },
    { 36, 115, "EINPROGRESS 36 -> 115" }, { 38, 88, "ENOTSOCK 38 -> 88" },
    { 45, 95, "ENOTSUP 45 -> 95" }, { 60, 110, "ETIMEDOUT 60 -> 110" },
    { 62, 40, "ELOOP 62 -> 40" }, { 63, 36, "ENAMETOOLONG 63 -> 36" },
    { 66, 39, "ENOTEMPTY 66 -> 39" }, { 77, 37, "ENOLCK 77 -> 37" },
    { 78, 38, "ENOSYS 78 -> 38" }, { 84, 75, "EOVERFLOW 84 -> 75" },
    { 89, 125, "ECANCELED 89 -> 125" }, { 92, 84, "EILSEQ 92 -> 84" },
    { 105, 130, "EOWNERDEAD 105 -> 130" },
  };
  for (size_t i = 0; i < sizeof et / sizeof et[0]; i++)
    OK(linux_errno_from_host(et[i].host) == et[i].lx, "%s", et[i].n);
  OK(linux_errno_from_host(0) == 0, "host errno 0 -> 0");

  // ------------------------------------------------------------------ openat/read/close
  section("openat + read + close");
  r = SC(SYS_openat, U(L_AT_FDCWD), P("/hello.txt"), L_O_RDONLY, 0, 0, 0);
  OK(r >= 0, "openat /hello.txt -> fd %ld", r);
  int fd = (int)r;
  r = SC(SYS_read, U(fd), P(buf), sizeof buf, 0, 0, 0);
  OK(r == (long)strlen(HELLO), "read %ld bytes (want %zu)", r, strlen(HELLO));
  OK(r > 0 && memcmp(buf, HELLO, (size_t)r) == 0, "content round-trips exactly");
  r = SC(SYS_read, U(fd), P(buf), sizeof buf, 0, 0, 0);
  OK(r == 0, "read at EOF -> 0");
  OK(SC(SYS_close, U(fd), 0, 0, 0, 0, 0) == 0, "close");
  OK(SC(SYS_read, U(fd), P(buf), 1, 0, 0, 0) == -9, "read on closed fd -> -EBADF (-9)");

  // ------------------------------------------------------------------ ENOENT is exactly -2
  section("errors are raw negative Linux errnos");
  r = SC(SYS_openat, U(L_AT_FDCWD), P("/definitely/not/here"), L_O_RDONLY, 0, 0, 0);
  OK(r == -2, "openat missing path -> %ld (want -2 ENOENT)", r);
  r = SC(SYS_openat, U(L_AT_FDCWD), P("/hello.txt"), L_O_RDONLY | L_O_DIRECTORY, 0, 0, 0);
  OK(r == -20, "openat O_DIRECTORY on a file -> %ld (want -20 ENOTDIR)", r);
  r = SC(SYS_openat, U(L_AT_FDCWD), P("/hello.txt"), L_O_WRONLY | L_O_CREAT | L_O_EXCL, 0644, 0, 0);
  OK(r == -17, "openat O_CREAT|O_EXCL on existing -> %ld (want -17 EEXIST)", r);
  OK(SC(SYS_close, 4242, 0, 0, 0, 0, 0) == -9, "close(bogus) -> -9 EBADF");
  OK(SC(SYS_openat, U(L_AT_FDCWD), P(""), L_O_RDONLY, 0, 0, 0) == -2,
     "openat with an empty path -> -2 ENOENT");
  OK(SC(SYS_openat, 4242, P("rel"), L_O_RDONLY, 0, 0, 0) == -9,
     "openat relative to a bogus dirfd -> -9 EBADF");

  // ------------------------------------------------------------------ write + newfstatat
  section("write new file + newfstatat");
  static const char* PAYLOAD = "0123456789abcdef";
  r = SC(SYS_openat, U(L_AT_FDCWD), P("/etc/created.txt"),
         L_O_WRONLY | L_O_CREAT | L_O_TRUNC | L_O_CLOEXEC, 0644, 0, 0);
  OK(r >= 0, "openat O_CREAT -> fd %ld", r);
  fd = (int)r;
  r = SC(SYS_write, U(fd), P(PAYLOAD), strlen(PAYLOAD), 0, 0, 0);
  OK(r == 16, "write -> %ld (want 16)", r);
  OK(SC(SYS_close, U(fd), 0, 0, 0, 0, 0) == 0, "close");

  struct lx_stat st;
  memset(&st, 0xAA, sizeof st);
  r = SC(SYS_newfstatat, U(L_AT_FDCWD), P("/etc/created.txt"), P(&st), 0, 0, 0);
  OK(r == 0, "newfstatat -> %ld", r);
  OK(st.st_size == 16, "st_size == %lld (want 16)", (long long)st.st_size);
  OK((st.st_mode & 0170000) == 0100000, "st_mode S_IFMT == S_IFREG (0100000), got %o",
     st.st_mode & 0170000);
  OK((st.st_mode & 07777) == 0644, "st_mode perms == 0644, got %o", st.st_mode & 07777);
  OK(st.st_nlink >= 1, "st_nlink == %u", st.st_nlink);
  OK(st.st_uid == (uint32_t)getuid(), "st_uid == %u", st.st_uid);
  OK(st.st_blksize > 0, "st_blksize == %d", st.st_blksize);
  OK(st.st_mtime_sec > 1600000000LL, "st_mtim.tv_sec == %lld looks like a real epoch",
     (long long)st.st_mtime_sec);
  OK(st.st_mtime_nsec >= 0 && st.st_mtime_nsec < 1000000000LL, "st_mtim.tv_nsec in range");
  // the two trailing pad words must be zero, and nothing may be written past byte 128
  OK(st.__unused4[0] == 0 && st.__unused4[1] == 0, "trailing pad words zeroed");

  // Read the SAME buffer back purely by byte offset — this is what the guest's musl does, and
  // it catches a struct whose fields are right but whose padding is wrong.
  {
    unsigned char raw[160];
    memset(raw, 0xEE, sizeof raw);
    OK(SC(SYS_newfstatat, U(L_AT_FDCWD), P("/etc/created.txt"), P(raw), 0, 0, 0) == 0,
       "newfstatat into a raw byte buffer");
    uint32_t mode, nlink, uid, gid; int64_t size, mtim_s; int32_t blksize;
    memcpy(&mode,    raw + 16, 4);
    memcpy(&nlink,   raw + 20, 4);
    memcpy(&uid,     raw + 24, 4);
    memcpy(&gid,     raw + 28, 4);
    memcpy(&size,    raw + 48, 8);
    memcpy(&blksize, raw + 56, 4);
    memcpy(&mtim_s,  raw + 88, 8);
    OK(size == 16, "byte offset 48 holds st_size == %lld", (long long)size);
    OK((mode & 0170000) == 0100000, "byte offset 16 holds st_mode (%o)", mode);
    OK(nlink == 1, "byte offset 20 holds st_nlink == %u", nlink);
    OK(uid == (uint32_t)getuid() && gid == (uint32_t)getgid(),
       "byte offsets 24/28 hold st_uid/st_gid");
    OK(blksize > 0, "byte offset 56 holds st_blksize == %d", blksize);
    OK(mtim_s == st.st_mtime_sec, "byte offset 88 holds st_mtim.tv_sec");
    int clean = 1;
    for (int i = 128; i < 160; i++) if (raw[i] != 0xEE) clean = 0;
    OK(clean, "nothing was written past byte 128");
  }

  section("fstat + AT_EMPTY_PATH");
  r = SC(SYS_openat, U(L_AT_FDCWD), P("/etc/created.txt"), L_O_RDONLY, 0, 0, 0);
  fd = (int)r;
  struct lx_stat st2; memset(&st2, 0, sizeof st2);
  OK(SC(SYS_fstat, U(fd), P(&st2), 0, 0, 0, 0) == 0, "fstat -> 0");
  OK(st2.st_size == 16 && st2.st_ino == st.st_ino, "fstat agrees with newfstatat");
  memset(&st2, 0, sizeof st2);
  OK(SC(SYS_newfstatat, U(fd), P(""), P(&st2), L_AT_EMPTY_PATH, 0, 0) == 0,
     "newfstatat AT_EMPTY_PATH -> 0");
  OK(st2.st_size == 16, "AT_EMPTY_PATH size == 16");
  SC(SYS_close, U(fd), 0, 0, 0, 0, 0);

  section("lstat vs stat on a symlink");
  memset(&st, 0, sizeof st);
  OK(SC(SYS_newfstatat, U(L_AT_FDCWD), P("/dir/link"), P(&st), 0, 0, 0) == 0, "stat symlink");
  OK((st.st_mode & 0170000) == 0100000 && st.st_size == 5, "follows to a.txt (5 bytes)");
  memset(&st, 0, sizeof st);
  OK(SC(SYS_newfstatat, U(L_AT_FDCWD), P("/dir/link"), P(&st), L_AT_SYMLINK_NOFOLLOW, 0, 0) == 0,
     "lstat symlink");
  OK((st.st_mode & 0170000) == 0120000, "S_IFMT == S_IFLNK (0120000), got %o",
     st.st_mode & 0170000);

  // ------------------------------------------------------------------ getdents64
  section("getdents64 (Linux linux_dirent64 layout)");
  r = SC(SYS_openat, U(L_AT_FDCWD), P("/dir"), L_O_RDONLY | L_O_DIRECTORY, 0, 0, 0);
  OK(r >= 0, "openat /dir O_DIRECTORY -> fd %ld", r);
  int dfd = (int)r;
  memset(buf, 0, sizeof buf);
  long total = SC(SYS_getdents64, U(dfd), P(buf), 4096, 0, 0, 0);
  OK(total > 0, "getdents64 -> %ld bytes", total);

  int saw_dot = 0, saw_dotdot = 0, saw_a = 0, saw_b = 0, saw_sub = 0, saw_link = 0;
  int nent = 0, align_ok = 1, type_ok = 1;
  for (long off = 0; off < total; ) {
    struct lx_dirent64* de = (struct lx_dirent64*)(buf + off);
    if (de->d_reclen == 0) { align_ok = 0; break; }
    if (de->d_reclen % 8) align_ok = 0;
    if (de->d_reclen < 19 + strlen(de->d_name) + 1) align_ok = 0;
    nent++;
    if (!strcmp(de->d_name, "."))      { saw_dot = 1;  if (de->d_type != 4) type_ok = 0; }
    if (!strcmp(de->d_name, ".."))     { saw_dotdot = 1; if (de->d_type != 4) type_ok = 0; }
    if (!strcmp(de->d_name, "a.txt"))  { saw_a = 1;   if (de->d_type != 8) type_ok = 0; }
    if (!strcmp(de->d_name, "b.bin"))  { saw_b = 1;   if (de->d_type != 8) type_ok = 0; }
    if (!strcmp(de->d_name, "sub"))    { saw_sub = 1; if (de->d_type != 4) type_ok = 0; }
    if (!strcmp(de->d_name, "link"))   { saw_link = 1; if (de->d_type != 10) type_ok = 0; }
    off += de->d_reclen;
    if (off > total) { align_ok = 0; break; }
  }
  OK(align_ok, "every d_reclen is 8-aligned and walks exactly to the end (%d entries)", nent);
  OK(saw_dot && saw_dotdot, "'.' and '..' present as DT_DIR");
  OK(saw_a && saw_b, "a.txt and b.bin present as DT_REG(8)");
  OK(saw_sub, "sub present as DT_DIR(4)");
  OK(saw_link, "link present as DT_LNK(10)");
  OK(type_ok, "all d_type values correct");
  OK(SC(SYS_getdents64, U(dfd), P(buf), 4096, 0, 0, 0) == 0, "second getdents64 -> 0 (drained)");
  OK(SC(SYS_getdents64, U(dfd), P(buf), 8, 0, 0, 0) == 0, "tiny buffer at EOF -> 0");
  OK(SC(SYS_close, U(dfd), 0, 0, 0, 0, 0) == 0, "close dirfd");

  section("getdents64 in small chunks");
  r = SC(SYS_openat, U(L_AT_FDCWD), P("/dir"), L_O_RDONLY | L_O_DIRECTORY, 0, 0, 0);
  dfd = (int)r;
  int chunk_entries = 0, chunk_rounds = 0;
  for (;;) {
    long n = SC(SYS_getdents64, U(dfd), P(buf), 40, 0, 0, 0);   // fits ~1 entry at a time
    if (n <= 0) break;
    chunk_rounds++;
    for (long off = 0; off < n; ) {
      struct lx_dirent64* de = (struct lx_dirent64*)(buf + off);
      chunk_entries++; off += de->d_reclen;
    }
    if (chunk_rounds > 64) break;
  }
  OK(chunk_entries == nent, "chunked walk saw the same %d entries (in %d calls)",
     chunk_entries, chunk_rounds);
  SC(SYS_close, U(dfd), 0, 0, 0, 0, 0);

  // ------------------------------------------------------------------ lseek / pread
  section("lseek + pread64 + pwrite64");
  r = SC(SYS_openat, U(L_AT_FDCWD), P("/hello.txt"), L_O_RDONLY, 0, 0, 0);
  fd = (int)r;
  OK(SC(SYS_lseek, U(fd), 6, 0 /*SEEK_SET*/, 0, 0, 0) == 6, "lseek SEEK_SET 6 -> 6");
  r = SC(SYS_read, U(fd), P(buf), 4, 0, 0, 0);   // "hello from the guest fs\n"[6..9] == "from"
  OK(r == 4 && memcmp(buf, "from", 4) == 0, "read after seek gives 'from' (got '%.4s')", buf);
  OK(SC(SYS_lseek, U(fd), 0, 1 /*SEEK_CUR*/, 0, 0, 0) == 10, "lseek SEEK_CUR 0 -> 10");
  OK(SC(SYS_lseek, U(fd), 0, 2 /*SEEK_END*/, 0, 0, 0) == (long)strlen(HELLO),
     "lseek SEEK_END -> file size");
  OK(SC(SYS_lseek, U(fd), U(-1), 0, 0, 0, 0) == -22, "lseek to negative offset -> -22 EINVAL");
  r = SC(SYS_pread64, U(fd), P(buf), 5, 0, 0, 0);
  OK(r == 5 && memcmp(buf, "hello", 5) == 0, "pread64 at 0 -> 'hello'");
  OK(SC(SYS_lseek, U(fd), 0, 1, 0, 0, 0) == (long)strlen(HELLO),
     "pread64 did not move the file offset");
  SC(SYS_close, U(fd), 0, 0, 0, 0, 0);

  r = SC(SYS_openat, U(L_AT_FDCWD), P("/etc/created.txt"), L_O_RDWR, 0, 0, 0);
  fd = (int)r;
  OK(SC(SYS_pwrite64, U(fd), P("ZZ"), 2, 4, 0, 0) == 2, "pwrite64 2 bytes at offset 4");
  OK(SC(SYS_pread64, U(fd), P(buf), 8, 0, 0, 0) == 8 && memcmp(buf, "0123ZZ67", 8) == 0,
     "pwrite64 landed at the right offset");
  OK(SC(SYS_ftruncate, U(fd), 8, 0, 0, 0, 0) == 0, "ftruncate to 8");
  memset(&st, 0, sizeof st);
  SC(SYS_fstat, U(fd), P(&st), 0, 0, 0, 0);
  OK(st.st_size == 8, "st_size after ftruncate == %lld", (long long)st.st_size);
  SC(SYS_close, U(fd), 0, 0, 0, 0, 0);

  // ------------------------------------------------------------------ readv / writev
  section("readv + writev");
  r = SC(SYS_openat, U(L_AT_FDCWD), P("/etc/iov.txt"),
         L_O_WRONLY | L_O_CREAT | L_O_TRUNC, 0644, 0, 0);
  fd = (int)r;
  struct { const void* b; size_t n; } iov[2] = { { "AAAA", 4 }, { "BBB", 3 } };
  OK(SC(SYS_writev, U(fd), P(iov), 2, 0, 0, 0) == 7, "writev 4+3 -> 7");
  SC(SYS_close, U(fd), 0, 0, 0, 0, 0);
  char p1[4] = {0}, p2[8] = {0};
  struct { void* b; size_t n; } riov[2] = { { p1, 4 }, { p2, 3 } };
  fd = (int)SC(SYS_openat, U(L_AT_FDCWD), P("/etc/iov.txt"), L_O_RDONLY, 0, 0, 0);
  OK(SC(SYS_readv, U(fd), P(riov), 2, 0, 0, 0) == 7, "readv -> 7");
  OK(memcmp(p1, "AAAA", 4) == 0 && memcmp(p2, "BBB", 3) == 0, "readv scattered correctly");
  SC(SYS_close, U(fd), 0, 0, 0, 0, 0);

  // ------------------------------------------------------------------ readlinkat
  section("readlinkat");
  memset(buf, 0, sizeof buf);
  r = SC(SYS_readlinkat, U(L_AT_FDCWD), P("/dir/link"), P(buf), sizeof buf, 0, 0);
  OK(r == 5 && memcmp(buf, "a.txt", 5) == 0, "readlinkat /dir/link -> %ld '%.*s'",
     r, (int)(r > 0 ? r : 0), buf);
  memset(buf, 0, sizeof buf);
  r = SC(SYS_readlinkat, U(L_AT_FDCWD), P("/proc/self/exe"), P(buf), sizeof buf, 0, 0);
  OK(r == (long)strlen("/bin/testprog") && memcmp(buf, "/bin/testprog", (size_t)r) == 0,
     "readlinkat /proc/self/exe -> '%.*s'", (int)(r > 0 ? r : 0), buf);
  OK(SC(SYS_readlinkat, U(L_AT_FDCWD), P("/hello.txt"), P(buf), sizeof buf, 0, 0) == -22,
     "readlinkat on a regular file -> -22 EINVAL");
  memset(buf, 0xCC, sizeof buf);
  r = SC(SYS_readlinkat, U(L_AT_FDCWD), P("/dir/link"), P(buf), 3, 0, 0);
  OK(r == 3 && memcmp(buf, "a.t", 3) == 0 && (unsigned char)buf[3] == 0xCC,
     "readlinkat truncates to bufsiz and does NOT NUL-terminate");

  // ------------------------------------------------------------------ getcwd / chdir
  section("getcwd + chdir + relative paths");
  memset(buf, 0, sizeof buf);
  r = SC(SYS_getcwd, P(buf), sizeof buf, 0, 0, 0, 0);
  OK(r == 2 && strcmp(buf, "/") == 0, "getcwd -> %ld '%s' (len includes NUL)", r, buf);
  OK(SC(SYS_getcwd, P(buf), 1, 0, 0, 0, 0) == -34, "getcwd with tiny buffer -> -34 ERANGE");
  OK(SC(SYS_chdir, P("/dir"), 0, 0, 0, 0, 0) == 0, "chdir /dir");
  memset(buf, 0, sizeof buf);
  r = SC(SYS_getcwd, P(buf), sizeof buf, 0, 0, 0, 0);
  OK(r == 5 && strcmp(buf, "/dir") == 0, "getcwd -> '%s'", buf);
  fd = (int)SC(SYS_openat, U(L_AT_FDCWD), P("a.txt"), L_O_RDONLY, 0, 0, 0);
  OK(fd >= 0, "openat AT_FDCWD relative 'a.txt' -> %d", fd);
  memset(buf, 0, sizeof buf);
  OK(SC(SYS_read, U(fd), P(buf), 16, 0, 0, 0) == 5 && strcmp(buf, "alpha") == 0,
     "relative open read 'alpha'");
  SC(SYS_close, U(fd), 0, 0, 0, 0, 0);
  OK(SC(SYS_chdir, P(".."), 0, 0, 0, 0, 0) == 0, "chdir ..");
  memset(buf, 0, sizeof buf);
  SC(SYS_getcwd, P(buf), sizeof buf, 0, 0, 0, 0);
  OK(strcmp(buf, "/") == 0, "getcwd after '..' -> '%s'", buf);
  OK(SC(SYS_chdir, P("/hello.txt"), 0, 0, 0, 0, 0) == -20, "chdir onto a file -> -20 ENOTDIR");
  OK(SC(SYS_chdir, P("/nope"), 0, 0, 0, 0, 0) == -2, "chdir missing -> -2 ENOENT");

  section("openat with a real dirfd");
  dfd = (int)SC(SYS_openat, U(L_AT_FDCWD), P("/dir"), L_O_RDONLY | L_O_DIRECTORY, 0, 0, 0);
  fd = (int)SC(SYS_openat, U(dfd), P("a.txt"), L_O_RDONLY, 0, 0, 0);
  OK(fd >= 0, "openat(dirfd, 'a.txt') -> %d", fd);
  memset(buf, 0, sizeof buf);
  OK(SC(SYS_read, U(fd), P(buf), 16, 0, 0, 0) == 5 && strcmp(buf, "alpha") == 0,
     "dirfd-relative read 'alpha'");
  SC(SYS_close, U(fd), 0, 0, 0, 0, 0);
  memset(&st, 0, sizeof st);
  OK(SC(SYS_newfstatat, U(dfd), P("sub"), P(&st), 0, 0, 0) == 0 &&
     (st.st_mode & 0170000) == 040000, "newfstatat(dirfd, 'sub') is a directory");
  SC(SYS_close, U(dfd), 0, 0, 0, 0, 0);

  // ------------------------------------------------------------------ mkdirat/unlinkat/etc
  section("mkdirat / renameat / linkat / unlinkat / faccessat");
  OK(SC(SYS_mkdirat, U(L_AT_FDCWD), P("/newdir"), 0755, 0, 0, 0) == 0, "mkdirat /newdir");
  OK(SC(SYS_mkdirat, U(L_AT_FDCWD), P("/newdir"), 0755, 0, 0, 0) == -17,
     "mkdirat again -> -17 EEXIST");
  OK(SC(SYS_faccessat, U(L_AT_FDCWD), P("/newdir"), 0 /*F_OK*/, 0, 0, 0) == 0,
     "faccessat F_OK on /newdir");
  OK(SC(SYS_faccessat, U(L_AT_FDCWD), P("/nothing"), 0, 0, 0, 0) == -2,
     "faccessat F_OK missing -> -2");
  OK(SC(SYS_faccessat, U(L_AT_FDCWD), P("/bin/testprog"), 1 /*X_OK*/, 0, 0, 0) == 0,
     "faccessat X_OK on an executable");
  OK(SC(SYS_renameat, U(L_AT_FDCWD), P("/newdir"), U(L_AT_FDCWD), P("/newdir2"), 0, 0) == 0,
     "renameat /newdir -> /newdir2");
  OK(SC(SYS_unlinkat, U(L_AT_FDCWD), P("/newdir2"), 0, 0, 0, 0) == -21,
     "unlinkat on a dir without AT_REMOVEDIR -> -21 EISDIR");
  OK(SC(SYS_unlinkat, U(L_AT_FDCWD), P("/newdir2"), L_AT_REMOVEDIR, 0, 0, 0) == 0,
     "unlinkat AT_REMOVEDIR");
  OK(SC(SYS_unlinkat, U(L_AT_FDCWD), P("/dir"), L_AT_REMOVEDIR, 0, 0, 0) == -39,
     "rmdir non-empty -> -39 ENOTEMPTY (macOS 66 -> Linux 39)");
  OK(SC(SYS_linkat, U(L_AT_FDCWD), P("/etc/iov.txt"), U(L_AT_FDCWD), P("/etc/iov2.txt"), 0, 0) == 0,
     "linkat");
  memset(&st, 0, sizeof st);
  SC(SYS_newfstatat, U(L_AT_FDCWD), P("/etc/iov2.txt"), P(&st), 0, 0, 0);
  OK(st.st_nlink == 2, "hard link -> st_nlink == %u", st.st_nlink);
  OK(SC(SYS_unlinkat, U(L_AT_FDCWD), P("/etc/iov2.txt"), 0, 0, 0, 0) == 0, "unlinkat file");
  OK(SC(SYS_unlinkat, U(L_AT_FDCWD), P("/etc/iov2.txt"), 0, 0, 0, 0) == -2,
     "unlinkat again -> -2 ENOENT");

  // ------------------------------------------------------------------ /proc + /dev
  section("/proc/self and /dev");
  fd = (int)SC(SYS_openat, U(L_AT_FDCWD), P("/proc/self/cmdline"), L_O_RDONLY, 0, 0, 0);
  OK(fd >= 0, "openat /proc/self/cmdline -> %d", fd);
  memset(buf, 0, sizeof buf);
  r = SC(SYS_read, U(fd), P(buf), sizeof buf, 0, 0, 0);
  OK(r == (long)strlen("/bin/testprog") + 1 && strcmp(buf, "/bin/testprog") == 0,
     "cmdline is '/bin/testprog\\0' (%ld bytes)", r);
  SC(SYS_close, U(fd), 0, 0, 0, 0, 0);

  fd = (int)SC(SYS_openat, U(L_AT_FDCWD), P("/proc/self/maps"), L_O_RDONLY, 0, 0, 0);
  OK(fd >= 0, "openat /proc/self/maps -> %d", fd);
  memset(buf, 0, sizeof buf);
  r = SC(SYS_read, U(fd), P(buf), sizeof buf - 1, 0, 0, 0);
  OK(r > 0 && strstr(buf, "[stack]") != NULL, "maps is non-empty and has a [stack] line");
  OK(SC(SYS_lseek, U(fd), 0, 0, 0, 0, 0) == 0, "maps fd is seekable");
  SC(SYS_close, U(fd), 0, 0, 0, 0, 0);

  fd = (int)SC(SYS_openat, U(L_AT_FDCWD), P("/proc/self/exe"), L_O_RDONLY, 0, 0, 0);
  OK(fd >= 0, "openat /proc/self/exe -> %d", fd);
  memset(buf, 0, sizeof buf);
  r = SC(SYS_read, U(fd), P(buf), 4, 0, 0, 0);
  OK(r == 4 && memcmp(buf, "\177ELF", 4) == 0, "/proc/self/exe reads the guest program");
  SC(SYS_close, U(fd), 0, 0, 0, 0, 0);

  fd = (int)SC(SYS_openat, U(L_AT_FDCWD), P("/dev/null"), L_O_WRONLY, 0, 0, 0);
  OK(fd >= 0, "openat /dev/null -> %d", fd);
  OK(SC(SYS_write, U(fd), P("x"), 1, 0, 0, 0) == 1, "write to /dev/null -> 1");
  SC(SYS_close, U(fd), 0, 0, 0, 0, 0);

  fd = (int)SC(SYS_openat, U(L_AT_FDCWD), P("/dev/urandom"), L_O_RDONLY, 0, 0, 0);
  OK(fd >= 0, "openat /dev/urandom -> %d", fd);
  memset(buf, 0, 32);
  r = SC(SYS_read, U(fd), P(buf), 32, 0, 0, 0);
  int nonzero = 0; for (int i = 0; i < 32; i++) if (buf[i]) nonzero = 1;
  OK(r == 32 && nonzero, "read 32 random bytes");
  SC(SYS_close, U(fd), 0, 0, 0, 0, 0);

  // ------------------------------------------------------------------ uname
  section("uname");
  struct lx_utsname un; memset(&un, 0xAA, sizeof un);
  OK(SC(SYS_uname, P(&un), 0, 0, 0, 0, 0) == 0, "uname -> 0");
  OK(strcmp(un.sysname, "Linux") == 0, "sysname == 'Linux' (got '%s')", un.sysname);
  OK(strcmp(un.machine, "aarch64") == 0, "machine == 'aarch64' (got '%s')", un.machine);
  OK(strcmp(un.release, "6.1.0") == 0, "release == '6.1.0' (got '%s')", un.release);
  OK(un.nodename[0] && un.version[0] && un.domainname[0], "nodename/version/domainname set");
  OK(sizeof un == 390, "utsname is 6 x 65 == 390 bytes");

  // ------------------------------------------------------------------ time
  section("clock_gettime / clock_getres / clock_nanosleep");
  struct lx_timespec t0, t1, res;
  OK(SC(SYS_clock_gettime, 0 /*REALTIME*/, P(&t0), 0, 0, 0, 0) == 0, "clock_gettime REALTIME");
  OK(t0.tv_sec > 1700000000LL && t0.tv_nsec >= 0 && t0.tv_nsec < 1000000000LL,
     "REALTIME sane: %lld.%09lld", (long long)t0.tv_sec, (long long)t0.tv_nsec);
  OK(llabs((long long)t0.tv_sec - (long long)time(NULL)) <= 2, "REALTIME agrees with time()");
  OK(SC(SYS_clock_gettime, 1 /*MONOTONIC*/, P(&t0), 0, 0, 0, 0) == 0, "clock_gettime MONOTONIC");
  OK(t0.tv_sec >= 0 && t0.tv_nsec >= 0 && t0.tv_nsec < 1000000000LL, "MONOTONIC sane");
  struct lx_timespec sl = { 0, 20 * 1000 * 1000 };  // 20ms
  OK(SC(SYS_clock_nanosleep, 1, 0, P(&sl), 0, 0, 0) == 0, "clock_nanosleep 20ms -> 0");
  OK(SC(SYS_clock_gettime, 1, P(&t1), 0, 0, 0, 0) == 0, "clock_gettime MONOTONIC again");
  long long dns = ((long long)t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec);
  OK(dns >= 20000000LL, "MONOTONIC advanced >= 20ms (%lld ns)", dns);
  OK(dns < 5000000000LL, "MONOTONIC advance is plausible, not garbage");
  OK(SC(SYS_clock_getres, 1, P(&res), 0, 0, 0, 0) == 0, "clock_getres MONOTONIC");
  OK(res.tv_sec == 0 && res.tv_nsec > 0, "resolution is sub-second and non-zero (%lld ns)",
     (long long)res.tv_nsec);
  OK(SC(SYS_clock_gettime, 99, P(&t0), 0, 0, 0, 0) == -22, "bogus clockid -> -22 EINVAL");
  struct lx_timespec bad = { 0, 2000000000LL };
  OK(SC(SYS_clock_nanosleep, 1, 0, P(&bad), 0, 0, 0) == -22, "nsec >= 1e9 -> -22 EINVAL");

  // ------------------------------------------------------------------ ids
  section("process ids");
  OK(SC0(SYS_getpid) == getpid(), "getpid");
  OK(SC0(SYS_getppid) == getppid(), "getppid");
  OK(SC0(SYS_getuid) == getuid(), "getuid");
  OK(SC0(SYS_geteuid) == geteuid(), "geteuid");
  OK(SC0(SYS_getgid) == getgid(), "getgid");
  OK(SC0(SYS_getegid) == getegid(), "getegid");
  OK(SC0(SYS_gettid) == getpid(), "gettid == pid (single guest thread)");
  OK(SC0(SYS_set_tid_address) == getpid(), "set_tid_address -> tid");
  OK(SC0(SYS_set_robust_list) == 0, "set_robust_list -> 0");

  // ------------------------------------------------------------------ statfs
  section("statfs / fstatfs");
  struct lx_statfs sf; memset(&sf, 0, sizeof sf);
  OK(sizeof sf == 120, "linux statfs is 120 bytes");
  OK(SC(SYS_statfs, P("/hello.txt"), P(&sf), 0, 0, 0, 0) == 0, "statfs -> 0");
  OK(sf.f_bsize > 0 && sf.f_blocks > 0, "f_bsize=%lld f_blocks=%llu",
     (long long)sf.f_bsize, (unsigned long long)sf.f_blocks);
  OK(sf.f_namelen == 255, "f_namelen == 255");
  memset(&sf, 0, sizeof sf);
  fd = (int)SC(SYS_openat, U(L_AT_FDCWD), P("/hello.txt"), L_O_RDONLY, 0, 0, 0);
  OK(SC(SYS_fstatfs, U(fd), P(&sf), 0, 0, 0, 0) == 0, "fstatfs -> 0");
  OK(sf.f_bsize > 0, "fstatfs f_bsize > 0");

  // ------------------------------------------------------------------ fcntl / flock / ioctl
  section("fcntl / flock / ioctl");
  r = SC(SYS_fcntl, U(fd), 3 /*F_GETFL*/, 0, 0, 0, 0);
  OK(r >= 0 && (r & 3) == 0, "F_GETFL on an O_RDONLY fd -> %#lx (accmode 0)", r);
  OK(SC(SYS_fcntl, U(fd), 2 /*F_SETFD*/, 1 /*FD_CLOEXEC*/, 0, 0, 0) == 0, "F_SETFD FD_CLOEXEC");
  OK(SC(SYS_fcntl, U(fd), 1 /*F_GETFD*/, 0, 0, 0, 0) == 1, "F_GETFD -> FD_CLOEXEC");
  r = SC(SYS_fcntl, U(fd), 0 /*F_DUPFD*/, 10, 0, 0, 0);
  OK(r >= 10, "F_DUPFD(10) -> %ld", r);
  SC(SYS_close, U(r), 0, 0, 0, 0, 0);
  OK(SC(SYS_fcntl, U(fd), 999, 0, 0, 0, 0) == -22, "unknown fcntl cmd -> -22 EINVAL");
  OK(SC(SYS_flock, U(fd), 2 /*LOCK_EX*/, 0, 0, 0, 0) == 0, "flock LOCK_EX");
  OK(SC(SYS_flock, U(fd), 8 /*LOCK_UN*/, 0, 0, 0, 0) == 0, "flock LOCK_UN");
  OK(SC(SYS_ioctl, U(fd), 0x5401 /*TCGETS*/, P(buf), 0, 0, 0) == -25,
     "TCGETS on a regular file -> -25 ENOTTY");
  OK(SC(SYS_ioctl, U(fd), 0xDEAD, P(buf), 0, 0, 0) == -25, "unknown ioctl -> -25 ENOTTY");
  SC(SYS_close, U(fd), 0, 0, 0, 0, 0);

  // O_NONBLOCK must survive the Linux->macOS->Linux flag round-trip.
  fd = (int)SC(SYS_openat, U(L_AT_FDCWD), P("/hello.txt"),
               L_O_RDONLY | L_O_NONBLOCK, 0, 0, 0);
  r = SC(SYS_fcntl, U(fd), 3, 0, 0, 0, 0);
  OK(fd >= 0 && (r & L_O_NONBLOCK), "O_NONBLOCK round-trips through F_GETFL (%#lx)", r);
  SC(SYS_close, U(fd), 0, 0, 0, 0, 0);

  // O_APPEND must actually append.
  fd = (int)SC(SYS_openat, U(L_AT_FDCWD), P("/etc/iov.txt"), L_O_WRONLY | L_O_APPEND, 0, 0, 0);
  OK(SC(SYS_write, U(fd), P("CC"), 2, 0, 0, 0) == 2, "append write");
  SC(SYS_close, U(fd), 0, 0, 0, 0, 0);
  memset(&st, 0, sizeof st);
  SC(SYS_newfstatat, U(L_AT_FDCWD), P("/etc/iov.txt"), P(&st), 0, 0, 0);
  OK(st.st_size == 9, "O_APPEND grew the file to %lld (want 9)", (long long)st.st_size);

  // ------------------------------------------------------------------ futex
  section("futex");
  uint32_t word = 1;
  OK(SC(SYS_futex, P(&word), 0 /*FUTEX_WAIT*/ | 128, 999, 0, 0, 0) == -11,
     "FUTEX_WAIT with a mismatched value -> -11 EAGAIN");
  OK(SC(SYS_futex, P(&word), 1 /*FUTEX_WAKE*/ | 128, 1, 0, 0, 0) == 0,
     "FUTEX_WAKE with no waiters -> 0");
  struct lx_timespec fto = { 0, 5 * 1000 * 1000 };  // 5ms
  OK(SC(SYS_futex, P(&word), 0 | 128, 1, P(&fto), 0, 0) == -110,
     "FUTEX_WAIT matching value + 5ms timeout -> -110 ETIMEDOUT");
  OK(SC(SYS_futex, P(&word), 77, 0, 0, 0, 0) == -38, "unknown futex op -> -38 ENOSYS");

  // ------------------------------------------------------------------ signals (recorded)
  section("rt_sigaction / rt_sigprocmask / rt_sigtimedwait / kill");
  struct { uint64_t h, f, r, m; } sa = { 0x1234, 0, 0, 0 }, old = { 0, 0, 0, 0 };
  OK(sizeof sa == 32, "linux sigaction is 32 bytes");
  OK(SC(SYS_rt_sigaction, 17 /*SIGCHLD*/, P(&sa), P(&old), 8, 0, 0) == 0, "rt_sigaction set");
  memset(&old, 0, sizeof old);
  OK(SC(SYS_rt_sigaction, 17, 0, P(&old), 8, 0, 0) == 0, "rt_sigaction query");
  OK(old.h == 0x1234, "previous handler was remembered (%#llx)", (unsigned long long)old.h);
  OK(SC(SYS_rt_sigaction, 0, P(&sa), 0, 8, 0, 0) == -22, "signal 0 -> -22 EINVAL");
  OK(SC(SYS_rt_sigaction, 17, P(&sa), 0, 4, 0, 0) == -22, "sigsetsize != 8 -> -22 EINVAL");
  uint64_t set = 1ULL << 16, oldset = 0;
  OK(SC(SYS_rt_sigprocmask, 0 /*SIG_BLOCK*/, P(&set), P(&oldset), 8, 0, 0) == 0, "SIG_BLOCK");
  oldset = 0;
  OK(SC(SYS_rt_sigprocmask, 2 /*SIG_SETMASK*/, 0, P(&oldset), 8, 0, 0) == 0, "query mask");
  OK(oldset == set, "mask round-trips (%#llx)", (unsigned long long)oldset);
  struct lx_timespec sw = { 0, 1000000 };
  OK(SC(SYS_rt_sigtimedwait, 0, 0, P(&sw), 8, 0, 0) == -11,
     "rt_sigtimedwait times out -> -11 EAGAIN");
  OK(SC(SYS_kill, U(getpid()), 0, 0, 0, 0, 0) == 0, "kill(self, 0) existence probe -> 0");
  OK(SC(SYS_kill, U(getpid()), 99, 0, 0, 0, 0) == -22, "kill with signal 99 -> -22 EINVAL");
  OK(SC(SYS_kill, 0x7FFFFFF0, 15, 0, 0, 0, 0) == -3, "kill of a missing pid -> -3 ESRCH");

  // ------------------------------------------------------------------ resources
  section("times / getrusage / sysinfo / prlimit64 / affinity");
  struct lx_tms tms; memset(&tms, 0, sizeof tms);
  r = SC(SYS_times, P(&tms), 0, 0, 0, 0, 0);
  OK(r > 0, "times -> %ld ticks", r);
  OK(tms.utime >= 0 && tms.stime >= 0, "tms_utime/%lld tms_stime/%lld non-negative",
     (long long)tms.utime, (long long)tms.stime);
  struct lx_rusage ru; memset(&ru, 0, sizeof ru);
  OK(sizeof ru == 144, "linux rusage is 144 bytes");
  OK(SC(SYS_getrusage, 0 /*RUSAGE_SELF*/, P(&ru), 0, 0, 0, 0) == 0, "getrusage -> 0");
  OK(ru.utv_sec >= 0 && ru.utv_usec >= 0 && ru.utv_usec < 1000000, "ru_utime sane");
  OK(ru.rest[0] > 0, "ru_maxrss (KB) == %lld", (long long)ru.rest[0]);
  unsigned char si[112]; memset(si, 0, sizeof si);
  OK(SC(SYS_sysinfo, P(si), 0, 0, 0, 0, 0) == 0, "sysinfo -> 0");
  int64_t uptime; uint64_t totalram; uint32_t memunit;
  memcpy(&uptime, si + 0, 8); memcpy(&totalram, si + 32, 8); memcpy(&memunit, si + 104, 4);
  OK(uptime >= 0, "sysinfo uptime %lld", (long long)uptime);
  OK(totalram > (1ULL << 30) && memunit == 1, "sysinfo totalram %llu bytes, mem_unit %u",
     (unsigned long long)totalram, memunit);
  struct lx_rlimit64 rl; memset(&rl, 0, sizeof rl);
  OK(SC(SYS_prlimit64, 0, 7 /*RLIMIT_NOFILE*/, 0, P(&rl), 0, 0) == 0, "prlimit64 RLIMIT_NOFILE");
  OK(rl.rlim_cur > 0, "NOFILE soft limit %llu", (unsigned long long)rl.rlim_cur);
  memset(&rl, 0, sizeof rl);
  OK(SC(SYS_prlimit64, 0, 3 /*RLIMIT_STACK*/, 0, P(&rl), 0, 0) == 0, "prlimit64 RLIMIT_STACK");
  OK(rl.rlim_cur > 0, "STACK soft limit %llu", (unsigned long long)rl.rlim_cur);
  unsigned char mask[128]; memset(mask, 0, sizeof mask);
  r = SC(SYS_sched_getaffinity, 0, sizeof mask, P(mask), 0, 0, 0);
  OK(r > 0, "sched_getaffinity -> %ld bytes", r);
  OK(mask[0] & 1, "cpu 0 present in the affinity mask");
  OK(SC(SYS_sched_setaffinity, 0, sizeof mask, P(mask), 0, 0, 0) == 0, "sched_setaffinity -> 0");
  OK(SC(SYS_sched_getaffinity, 0, 4, P(mask), 0, 0, 0) == -22, "cpusetsize < 8 -> -22 EINVAL");

  // ------------------------------------------------------------------ memory
  section("mmap / mprotect / munmap / mremap / brk / madvise");
  r = SC(SYS_mmap, 0, 65536, PROT_READ | PROT_WRITE, 0x22 /*PRIVATE|ANON*/, U(-1), 0);
  OK(r > 0, "mmap 64K anon -> %#lx", (unsigned long)r);
  if (r > 0) {
    memset((void*)r, 0x5A, 65536);
    OK(((unsigned char*)r)[65535] == 0x5A, "the mapping is writable");
    OK(SC(SYS_mprotect, U(r), 65536, PROT_READ, 0, 0, 0) == 0, "mprotect PROT_READ -> 0");
    OK(SC(SYS_madvise, U(r), 65536, 4 /*MADV_DONTNEED*/, 0, 0, 0) == 0, "madvise -> 0");
    OK(SC(SYS_munmap, U(r), 65536, 0, 0, 0, 0) == 0, "munmap -> 0");
  }
  r = SC(SYS_mmap, 0, 32768, PROT_READ | PROT_WRITE, 0x22, U(-1), 0);
  if (r > 0) {
    memcpy((void*)r, "MREMAP", 6);
    long nr2 = SC(SYS_mremap, U(r), 32768, 131072, 1 /*MAYMOVE*/, 0, 0);
    OK(nr2 > 0, "mremap grow with MAYMOVE -> %#lx", (unsigned long)nr2);
    OK(nr2 > 0 && memcmp((void*)nr2, "MREMAP", 6) == 0, "mremap preserved the contents");
    if (nr2 > 0) SC(SYS_munmap, U(nr2), 131072, 0, 0, 0, 0);
  }
  long b0 = SC(SYS_brk, 0, 0, 0, 0, 0, 0);
  OK(b0 > 0, "brk(0) -> %#lx", (unsigned long)b0);
  long b1 = SC(SYS_brk, U(b0 + 65536), 0, 0, 0, 0, 0);
  OK(b1 == b0 + 65536, "brk grow -> %#lx", (unsigned long)b1);
  memset((void*)b0, 0x11, 65536);
  OK(((unsigned char*)b0)[0] == 0x11, "the brk heap is writable");
  OK(SC(SYS_brk, 0, 0, 0, 0, 0, 0) == b1, "brk(0) reports the new break");
  OK(SC(SYS_brk, 1, 0, 0, 0, 0, 0) == b1, "an out-of-range brk leaves the break alone");

  // ------------------------------------------------------------------ getrandom / rseq / ext
  section("getrandom / rseq / fallthrough to linux_syscall_ext");
  memset(buf, 0, 64);
  OK(SC(SYS_getrandom, P(buf), 64, 0, 0, 0, 0) == 64, "getrandom 64 -> 64");
  nonzero = 0; for (int i = 0; i < 64; i++) if (buf[i]) nonzero = 1;
  OK(nonzero, "getrandom produced non-zero bytes");
  OK(SC(SYS_rseq, 0, 0, 0, 0, 0, 0) == -38, "rseq -> -38 ENOSYS");
  OK(SC0(1234) == -38, "an unknown syscall number tail-calls linux_syscall_ext -> -38");
  extern int g_ext_calls;
  OK(g_ext_calls >= 1, "linux_syscall_ext was actually reached (%d call(s))", g_ext_calls);

  // ------------------------------------------------------------------
  printf("\n===============================================\n");
  printf("  passed: %d   failed: %d\n", g_pass, g_fail);
  printf("===============================================\n");
  if (g_fail == 0) rmrf(ROOT);
  else printf("  (rootfs left at %s for inspection)\n", ROOT);
  return g_fail ? 1 : 0;
}

// ===========================================================================================
// Stubs so this links standalone (the real gmem.c / syscalls_ext.c are separate modules).
// ===========================================================================================

int g_ext_calls;

long linux_syscall_ext(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
                       unsigned long a3, unsigned long a4, unsigned long a5) {
  (void)nr; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
  g_ext_calls++;
  return -38;   // -ENOSYS
}

void syscalls_ext_set_root(const char* root) { (void)root; }

// Minimal stand-in for gmem: real mmap with the Linux flag values translated. The real gmem.c
// additionally enforces W^X and rewrites guest instructions; none of that is exercised here.
long gmem_mmap(unsigned long addr, size_t len, int prot, int flags, int fd, long off) {
  int mf = 0;
  if (flags & 0x01) mf |= MAP_SHARED;
  if (flags & 0x02) mf |= MAP_PRIVATE;
  if (flags & 0x10) mf |= MAP_FIXED;
  if (flags & 0x20) { mf |= MAP_ANON; fd = -1; off = 0; }
  void* p = mmap((void*)addr, len, prot, mf, fd, (off_t)off);
  return p == MAP_FAILED ? -linux_errno_from_host(errno) : (long)(uintptr_t)p;
}
long gmem_mprotect(unsigned long addr, size_t len, int prot) {
  return mprotect((void*)addr, len, prot) == 0 ? 0 : -linux_errno_from_host(errno);
}
long gmem_munmap(unsigned long addr, size_t len) {
  return munmap((void*)addr, len) == 0 ? 0 : -linux_errno_from_host(errno);
}
void gmem_init(void) {}
void* gmem_reserve(size_t len) { (void)len; return 0; }
int gmem_map_code(void* a, const void* b, size_t l) { (void)a; (void)b; (void)l; return -1; }
void gmem_begin_load(void) {}
void gmem_end_load(void) {}
void gmem_rewrite_exec_insns(unsigned char* b, size_t l) { (void)b; (void)l; }

// --- thread/process hooks normally supplied by shim.c ---------------------------------------
// These stubs were missing, so this driver had not LINKED, let alone run, for some time. A test
// binary that cannot be built is indistinguishable from one that passes if nobody runs it, which
// is the failure mode this project keeps rediscovering — so they are restored here rather than
// worked around. Single-threaded, no fork: the values below are the honest answers for a driver
// that drives linux_syscall() directly on the main thread.
int  guest_tid(void) { return (int)getpid(); }
int  guest_is_child(void) { return 0; }
int  guest_child_known(int pid) { (void)pid; return 0; }
int  guest_is_vfork_child(void) { return 0; }
void guest_vfork_finish(void) {}
void guest_thread_exiting(void) {}
void guest_set_tid_address(unsigned long a) { (void)a; }
