// test_syscalls_ext.c — standalone exerciser for the syscalls_ext.c fallback tier.
//
// Build & run:
//   clang -arch arm64 -O2 -Wall -Wextra -o test_syscalls_ext \
//         test_syscalls_ext.c syscalls_ext.c && ./test_syscalls_ext
//
// All scratch state lives under /Users/dperry/linux-shim/tests (never /tmp — it gets purged).
// linux_errno_from_host() is stubbed here so the module links without syscalls.c.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stddef.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include "syscalls_ext.h"

#define TESTDIR "/Users/dperry/linux-shim/tests"
#define TESTROOT TESTDIR "/root"

// --- Linux constants the test asserts against ------------------------------
#define LX_EPERM   1
#define LX_ENOENT  2
#define LX_EBADF   9
#define LX_EINVAL  22
#define LX_ENOSYS  38
#define LX_AT_FDCWD (-100)
#define LX_O_CLOEXEC 0x80000
#define LX_MFD_CLOEXEC 1

// --- local stub for the symbol syscalls.c normally provides -----------------
// macOS and Linux errno values agree up to 34; above that they diverge, so map the handful
// this test can actually provoke and pass everything else through.
int linux_errno_from_host(int host_errno);
int linux_errno_from_host(int host_errno) {
  switch (host_errno) {
    case ENOTSUP:        return 95;   // macOS 45 -> Linux EOPNOTSUPP
    case ENAMETOOLONG:   return 36;
    case ENOSYS:         return 38;
    case ELOOP:          return 40;
    case ENOTEMPTY:      return 39;
    case EOVERFLOW:      return 75;
    default:             return host_errno <= 34 ? host_errno : host_errno;
  }
}

// --- harness ---------------------------------------------------------------
static int g_pass, g_fail;
#define CHECK(cond, ...) do {                                              \
    if (cond) { g_pass++; }                                                \
    else { g_fail++; printf("  FAIL (line %d): ", __LINE__);               \
           printf(__VA_ARGS__); printf("\n"); }                            \
  } while (0)
#define SECTION(name) printf("== %s\n", name)

static long sc(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
               unsigned long a3, unsigned long a4, unsigned long a5) {
  return linux_syscall_ext(nr, a0, a1, a2, a3, a4, a5);
}
#define SC0(n)                   sc((n),0,0,0,0,0,0)
#define SC1(n,a)                 sc((n),(unsigned long)(a),0,0,0,0,0)
#define SC2(n,a,b)               sc((n),(unsigned long)(a),(unsigned long)(b),0,0,0,0)
#define SC3(n,a,b,c)             sc((n),(unsigned long)(a),(unsigned long)(b),(unsigned long)(c),0,0,0)
#define SC4(n,a,b,c,d)           sc((n),(unsigned long)(a),(unsigned long)(b),(unsigned long)(c),(unsigned long)(d),0,0)
#define SC5(n,a,b,c,d,e)         sc((n),(unsigned long)(a),(unsigned long)(b),(unsigned long)(c),(unsigned long)(d),(unsigned long)(e),0)

// Host path for a guest-absolute path.
static const char* H(const char* guest) {
  static char buf[1024];
  snprintf(buf, sizeof buf, "%s%s", TESTROOT, guest);
  return buf;
}

// --- Linux struct statx, redeclared independently so offsets are a real assertion ---
struct t_statx_ts { long long tv_sec; unsigned tv_nsec; int __r; };
struct t_statx {
  unsigned stx_mask, stx_blksize;
  unsigned long long stx_attributes;
  unsigned stx_nlink, stx_uid, stx_gid;
  unsigned short stx_mode, __spare0[1];
  unsigned long long stx_ino, stx_size, stx_blocks, stx_attributes_mask;
  struct t_statx_ts stx_atime, stx_btime, stx_ctime, stx_mtime;
  unsigned stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
  unsigned long long stx_mnt_id;
  unsigned stx_dio_mem_align, stx_dio_offset_align;
  unsigned long long __spare3[12];
};

struct t_timeval { long long tv_sec, tv_usec; };   // Linux timeval: 64-bit usec
struct t_stack   { void* ss_sp; int ss_flags; unsigned long ss_size; };
struct t_tms     { long utime, stime, cutime, cstime; };

// ---------------------------------------------------------------------------
int main(void) {
  // Scratch tree. <root>/tmp doubles as the memfd backing dir (see memfd_dir()).
  if (system("mkdir -p " TESTROOT "/tmp " TESTROOT "/sub") != 0) { }
  syscalls_ext_set_root(TESTROOT);
  unlink(H("/f1.txt")); unlink(H("/link")); unlink(H("/target.txt"));
  unlink(H("/tr.txt")); unlink(H("/cfr_src")); unlink(H("/cfr_dst"));
  unlink(H("/ren_a")); unlink(H("/ren_b"));

  // ---------------------------------------------------------------- dup / dup3
  SECTION("23 dup / 24 dup3");
  {
    int fd = open(H("/f1.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "open scratch failed: %s", strerror(errno));
    CHECK(write(fd, "hello", 5) == 5, "write failed");

    long d = SC1(23, fd);
    CHECK(d >= 0 && d != fd, "dup -> %ld", d);
    CHECK(lseek((int)d, 0, SEEK_SET) == 0, "lseek on dup");
    char b[8] = {0};
    CHECK(read((int)d, b, 5) == 5 && memcmp(b, "hello", 5) == 0, "dup fd not usable (got '%s')", b);
    close((int)d);

    long d3 = SC3(24, fd, 30, LX_O_CLOEXEC);
    CHECK(d3 == 30, "dup3 -> %ld (want 30)", d3);
    CHECK((fcntl(30, F_GETFD) & FD_CLOEXEC) != 0, "dup3 did not set FD_CLOEXEC");
    CHECK(lseek(30, 0, SEEK_SET) == 0 && read(30, b, 5) == 5, "dup3 fd not usable");
    close(30);

    CHECK(SC3(24, fd, fd, 0) == -LX_EINVAL, "dup3(fd,fd) must be -EINVAL");
    CHECK(SC3(24, fd, 31, 0x40) == -LX_EINVAL, "dup3 bad flags must be -EINVAL");
    CHECK(SC1(23, 9999) == -LX_EBADF, "dup(badfd) must be -EBADF");
    close(fd);
  }

  // ---------------------------------------------------------- symlinkat/readlink
  SECTION("36 symlinkat");
  {
    int fd = open(H("/target.txt"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "create target");
    write(fd, "T", 1); close(fd);

    long r = SC3(36, "/target.txt", LX_AT_FDCWD, "/link");
    CHECK(r == 0, "symlinkat -> %ld", r);
    char buf[256] = {0};
    ssize_t n = readlink(H("/link"), buf, sizeof buf - 1);
    CHECK(n > 0 && strcmp(buf, "/target.txt") == 0,
          "readlink round-trip got '%s' (n=%zd)", buf, n);
    // The link target must be stored VERBATIM (guest namespace), not rootfs-prefixed.
    CHECK(strstr(buf, TESTROOT) == NULL, "symlink target was wrongly rootfs-prefixed: '%s'", buf);
    CHECK(SC3(36, "/target.txt", LX_AT_FDCWD, "/link") == -EEXIST, "symlinkat over existing");
  }

  // ------------------------------------------------------------ fchmod/fchmodat
  SECTION("52 fchmod / 53 fchmodat / 452 fchmodat2");
  {
    int fd = open(H("/f1.txt"), O_RDWR);
    CHECK(fd >= 0, "reopen f1");
    CHECK(SC2(52, fd, 0640) == 0, "fchmod failed");
    struct stat st;
    CHECK(stat(H("/f1.txt"), &st) == 0 && (st.st_mode & 07777) == 0640,
          "fchmod: mode is %o, want 640", st.st_mode & 07777);
    close(fd);

    CHECK(SC4(53, LX_AT_FDCWD, "/f1.txt", 0600, 0) == 0, "fchmodat failed");
    CHECK(stat(H("/f1.txt"), &st) == 0 && (st.st_mode & 07777) == 0600,
          "fchmodat: mode is %o, want 600", st.st_mode & 07777);

    CHECK(SC4(452, LX_AT_FDCWD, "/f1.txt", 0644, 0) == 0, "fchmodat2 failed");
    CHECK(stat(H("/f1.txt"), &st) == 0 && (st.st_mode & 07777) == 0644,
          "fchmodat2: mode is %o, want 644", st.st_mode & 07777);
    CHECK(SC4(53, LX_AT_FDCWD, "/nope.txt", 0600, 0) == -LX_ENOENT, "fchmodat ENOENT");
  }

  // ---------------------------------------------------------------- utimensat
  SECTION("88 utimensat");
  {
    struct timespec times[2];
    times[0].tv_sec = 1000000000; times[0].tv_nsec = 0;    // atime
    times[1].tv_sec = 1234567890; times[1].tv_nsec = 0;    // mtime
    long r = SC4(88, LX_AT_FDCWD, "/f1.txt", times, 0);
    CHECK(r == 0, "utimensat -> %ld", r);
    struct stat st;
    CHECK(stat(H("/f1.txt"), &st) == 0, "stat after utimensat");
    CHECK(st.st_mtimespec.tv_sec == 1234567890,
          "mtime is %lld, want 1234567890", (long long)st.st_mtimespec.tv_sec);
    CHECK(st.st_atimespec.tv_sec == 1000000000,
          "atime is %lld, want 1000000000", (long long)st.st_atimespec.tv_sec);
    // NULL path == "the dirfd itself" (futimens path)
    int fd = open(H("/target.txt"), O_RDWR);
    times[1].tv_sec = 999000000;
    CHECK(SC4(88, fd, NULL, times, 0) == 0, "utimensat(NULL path) failed");
    CHECK(fstat(fd, &st) == 0 && st.st_mtimespec.tv_sec == 999000000,
          "utimensat(NULL path) mtime is %lld", (long long)st.st_mtimespec.tv_sec);
    close(fd);
  }

  // -------------------------------------------------------------- memfd_create
  SECTION("279 memfd_create");
  {
    long fd = SC2(279, "guestmem", LX_MFD_CLOEXEC);
    CHECK(fd >= 0, "memfd_create -> %ld (%s)", fd, fd < 0 ? "error" : "ok");
    if (fd >= 0) {
      int f = (int)fd;
      CHECK((fcntl(f, F_GETFD) & FD_CLOEXEC) != 0, "MFD_CLOEXEC not honored");
      CHECK(ftruncate(f, 65536) == 0, "ftruncate on memfd: %s", strerror(errno));
      struct stat st;
      CHECK(fstat(f, &st) == 0 && st.st_size == 65536, "memfd size after ftruncate");
      CHECK(st.st_nlink == 0, "memfd should be unlinked (nlink=%u)", (unsigned)st.st_nlink);
      CHECK(pwrite(f, "PAYLOAD", 7, 4096) == 7, "write to memfd: %s", strerror(errno));
      char b[8] = {0};
      CHECK(pread(f, b, 7, 4096) == 7 && memcmp(b, "PAYLOAD", 7) == 0,
            "read back from memfd got '%s'", b);
      // must survive real mmap use, both shared and private
      void* p = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
      CHECK(p != MAP_FAILED, "mmap(MAP_SHARED) on memfd: %s", strerror(errno));
      if (p != MAP_FAILED) {
        CHECK(memcmp((char*)p + 4096, "PAYLOAD", 7) == 0, "mmap view does not see the write");
        memcpy((char*)p + 8192, "VIAMAP", 6);
        msync(p, 65536, MS_SYNC);
        char c[8] = {0};
        CHECK(pread(f, c, 6, 8192) == 6 && memcmp(c, "VIAMAP", 6) == 0,
              "write through mmap not visible via pread (got '%s')", c);
        munmap(p, 65536);
      }
      close(f);
    }
    CHECK(SC2(279, "x", 0x1000) == -LX_EINVAL, "memfd_create bad flags must be -EINVAL");
    CHECK(SC2(279, NULL, 0) == -14 /*EFAULT*/, "memfd_create(NULL) must be -EFAULT");
    // no stray backing files left behind
    CHECK(system("test -z \"$(ls -A " TESTROOT "/tmp)\"") == 0,
          "memfd left a file behind in " TESTROOT "/tmp");
  }

  // -------------------------------------------------------------------- prctl
  SECTION("167 prctl");
  {
    CHECK(SC2(167, 15 /*PR_SET_NAME*/, "linuxguest") == 0, "PR_SET_NAME failed");
    char name[16]; memset(name, 0xAA, sizeof name);
    CHECK(SC2(167, 16 /*PR_GET_NAME*/, name) == 0, "PR_GET_NAME failed");
    CHECK(strcmp(name, "linuxguest") == 0, "PR_GET_NAME round-trip got '%s'", name);

    CHECK(SC2(167, 1 /*PR_SET_PDEATHSIG*/, SIGTERM) == 0, "PR_SET_PDEATHSIG failed");
    int pds = -1;
    CHECK(SC2(167, 2 /*PR_GET_PDEATHSIG*/, &pds) == 0 && pds == SIGTERM,
          "PR_GET_PDEATHSIG got %d", pds);

    CHECK(SC2(167, 38 /*PR_SET_NO_NEW_PRIVS*/, 1) == 0, "PR_SET_NO_NEW_PRIVS should succeed");
    CHECK(SC2(167, 4 /*PR_SET_DUMPABLE*/, 1) == 0, "PR_SET_DUMPABLE");
    CHECK(SC1(167, 3 /*PR_GET_DUMPABLE*/) == 1, "PR_GET_DUMPABLE");
    CHECK(SC2(167, 0x53564d41 /*PR_SET_VMA*/, 0) == 0, "PR_SET_VMA should be a no-op success");
    CHECK(SC1(167, 0x7fffffff) == -LX_EINVAL, "unknown prctl must be -EINVAL");
  }

  // ------------------------------------------------------ sessions / pgrp / ids
  SECTION("154-159 pgid/sid/groups, 166 umask");
  {
    long pg = SC1(155, 0);                                    // getpgid(0)
    CHECK(pg == (long)getpgid(0) && pg > 0, "getpgid(0) -> %ld, host says %d", pg, getpgid(0));
    CHECK(SC1(157, 0) == (long)getsid(0), "getsid(0) mismatch");
    CHECK(SC2(154, 0, pg) == 0, "setpgid(0, own pgid) should succeed");
    CHECK(SC1(155, 999999) == -3 /*ESRCH*/, "getpgid(bogus) must be -ESRCH");

    // setsid: in a child (not a group leader after fork it may still be), accept either the
    // new sid or -EPERM (the caller is already a process-group leader).
    long sid = SC0(156);
    CHECK(sid > 0 || sid == -LX_EPERM, "setsid -> %ld (want >0 or -EPERM)", sid);

    // a forked child that is not a group leader must genuinely get a new session
    pid_t kid = fork();
    if (kid == 0) {
      long s = SC0(156);
      _exit((s > 0 && s == (long)getpid()) ? 0 : 1);
    }
    int wst = 0; waitpid(kid, &wst, 0);
    CHECK(WIFEXITED(wst) && WEXITSTATUS(wst) == 0, "setsid in a fresh child must succeed");

    unsigned gids[64];
    long ng = SC2(158, 64, gids);
    CHECK(ng > 0, "getgroups -> %ld", ng);

    long old = SC1(166, 022);                                 // umask
    CHECK(old >= 0, "umask -> %ld", old);
    CHECK(SC1(166, (unsigned long)old) == 022, "umask did not return the previous mask");
  }

  // --------------------------------------------------------------------- statx
  SECTION("291/258 statx");
  {
    // ABI offsets are load-bearing: the guest's libc reads these exact byte positions.
    CHECK(sizeof(struct t_statx) == 256, "sizeof statx = %zu, want 256", sizeof(struct t_statx));
    CHECK(offsetof(struct t_statx, stx_mask)       ==   0, "off stx_mask");
    CHECK(offsetof(struct t_statx, stx_blksize)    ==   4, "off stx_blksize");
    CHECK(offsetof(struct t_statx, stx_attributes) ==   8, "off stx_attributes");
    CHECK(offsetof(struct t_statx, stx_nlink)      ==  16, "off stx_nlink");
    CHECK(offsetof(struct t_statx, stx_uid)        ==  20, "off stx_uid");
    CHECK(offsetof(struct t_statx, stx_gid)        ==  24, "off stx_gid");
    CHECK(offsetof(struct t_statx, stx_mode)       ==  28, "off stx_mode");
    CHECK(offsetof(struct t_statx, stx_ino)        ==  32, "off stx_ino");
    CHECK(offsetof(struct t_statx, stx_size)       ==  40, "off stx_size");
    CHECK(offsetof(struct t_statx, stx_blocks)     ==  48, "off stx_blocks");
    CHECK(offsetof(struct t_statx, stx_atime)      ==  64, "off stx_atime");
    CHECK(offsetof(struct t_statx, stx_btime)      ==  80, "off stx_btime");
    CHECK(offsetof(struct t_statx, stx_ctime)      ==  96, "off stx_ctime");
    CHECK(offsetof(struct t_statx, stx_mtime)      == 112, "off stx_mtime");
    CHECK(offsetof(struct t_statx, stx_rdev_major) == 128, "off stx_rdev_major");
    CHECK(offsetof(struct t_statx, stx_dev_major)  == 136, "off stx_dev_major");
    CHECK(offsetof(struct t_statx, stx_mnt_id)     == 144, "off stx_mnt_id");

    int fd = open(H("/f1.txt"), O_RDWR);
    write(fd, "0123456789", 10);
    struct stat st;
    fstat(fd, &st);

    struct t_statx sx;
    memset(&sx, 0xAB, sizeof sx);
    long r = SC5(291, LX_AT_FDCWD, "/f1.txt", 0, 0x7ff /*STATX_BASIC_STATS*/, &sx);
    CHECK(r == 0, "statx -> %ld", r);
    CHECK((sx.stx_mask & 0x7ff) == 0x7ff, "stx_mask = %#x, want BASIC_STATS set", sx.stx_mask);
    CHECK((sx.stx_mask & 0x800) != 0, "STATX_BTIME should be reported (macOS has birthtime)");
    CHECK(sx.stx_size == (unsigned long long)st.st_size,
          "stx_size = %llu, want %lld", sx.stx_size, (long long)st.st_size);
    CHECK(sx.stx_ino == (unsigned long long)st.st_ino, "stx_ino mismatch");
    CHECK(sx.stx_mode == (unsigned short)st.st_mode,
          "stx_mode = %o, want %o", sx.stx_mode, st.st_mode);
    CHECK((sx.stx_mode & 0170000) == 0100000, "stx_mode S_IFREG bit (Linux value) not set");
    CHECK(sx.stx_uid == (unsigned)st.st_uid && sx.stx_gid == (unsigned)st.st_gid, "statx uid/gid");
    CHECK(sx.stx_nlink == (unsigned)st.st_nlink, "statx nlink");
    CHECK(sx.stx_blksize == (unsigned)st.st_blksize, "statx blksize");
    CHECK(sx.stx_mtime.tv_sec == (long long)st.st_mtimespec.tv_sec, "statx mtime");
    CHECK(sx.stx_btime.tv_sec == (long long)st.st_birthtimespec.tv_sec, "statx btime");
    CHECK(sx.stx_dev_major != 0 || sx.stx_dev_minor != 0, "statx dev major/minor both zero");
    CHECK(sx.__spare3[11] == 0, "statx tail not zeroed");

    // AT_EMPTY_PATH against an fd
    memset(&sx, 0, sizeof sx);
    CHECK(SC5(291, fd, "", 0x1000 /*AT_EMPTY_PATH*/, 0x7ff, &sx) == 0, "statx AT_EMPTY_PATH");
    CHECK(sx.stx_ino == (unsigned long long)st.st_ino, "statx AT_EMPTY_PATH wrong inode");

    // AT_SYMLINK_NOFOLLOW must see the symlink itself
    memset(&sx, 0, sizeof sx);
    CHECK(SC5(291, LX_AT_FDCWD, "/link", 0x100, 0x7ff, &sx) == 0, "statx on symlink");
    CHECK((sx.stx_mode & 0170000) == 0120000, "AT_SYMLINK_NOFOLLOW: want S_IFLNK, got %o",
          sx.stx_mode & 0170000);

    CHECK(SC5(258, LX_AT_FDCWD, "/f1.txt", 0, 0x7ff, &sx) == 0, "statx alias at 258");
    CHECK(SC5(291, LX_AT_FDCWD, "/does-not-exist", 0, 0x7ff, &sx) == -LX_ENOENT, "statx ENOENT");
    close(fd);
  }

  // ------------------------------------------------------------------- fs tail
  SECTION("45 truncate / 47 fallocate / 50 fchdir / 84 fsync / 276 renameat2");
  {
    int fd = open(H("/tr.txt"), O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(fd, "abcdefghij", 10);
    CHECK(SC2(45, "/tr.txt", 4) == 0, "truncate failed");
    struct stat st; stat(H("/tr.txt"), &st);
    CHECK(st.st_size == 4, "truncate: size %lld want 4", (long long)st.st_size);

    CHECK(SC4(47, fd, 0, 0, 8192) == 0, "fallocate failed");
    fstat(fd, &st);
    CHECK(st.st_size == 8192, "fallocate: size %lld want 8192", (long long)st.st_size);
    CHECK(SC4(47, fd, 0x01 /*KEEP_SIZE*/, 0, 16384) == 0, "fallocate KEEP_SIZE failed");
    fstat(fd, &st);
    CHECK(st.st_size == 8192, "fallocate KEEP_SIZE must not grow the file (%lld)",
          (long long)st.st_size);
    CHECK(SC1(82, fd) == 0, "fsync failed");
    CHECK(SC1(83, fd) == 0, "fdatasync failed");
    CHECK(SC1(82, 9999) == -LX_EBADF, "fsync(badfd) must be -EBADF");
    CHECK(SC4(84, fd, 0, 0, 0) == 0, "sync_file_range -> fsync failed");
    CHECK(SC0(81) == 0, "sync failed");
    close(fd);

    int dfd = open(TESTROOT "/sub", O_RDONLY);
    char cwd0[1024]; if (!getcwd(cwd0, sizeof cwd0)) cwd0[0] = 0;
    CHECK(SC1(50, dfd) == 0, "fchdir failed");
    char cwd[1024];
    CHECK(getcwd(cwd, sizeof cwd) && strstr(cwd, "/sub") != NULL, "fchdir did not move cwd");
    if (cwd0[0] && chdir(cwd0) != 0) { }
    close(dfd);

    close(open(H("/ren_a"), O_WRONLY | O_CREAT, 0644));
    CHECK(SC5(276, LX_AT_FDCWD, "/ren_a", LX_AT_FDCWD, "/ren_b", 0) == 0, "renameat2(0) failed");
    CHECK(stat(H("/ren_b"), &st) == 0, "renameat2 did not move the file");
    close(open(H("/ren_a"), O_WRONLY | O_CREAT, 0644));
    CHECK(SC5(276, LX_AT_FDCWD, "/ren_a", LX_AT_FDCWD, "/ren_b", 1 /*NOREPLACE*/) == -EEXIST,
          "RENAME_NOREPLACE over an existing file must fail");
  }

  // ------------------------------------------------------------ pipe2 / IO tail
  SECTION("59 pipe2 / 71 sendfile / 285 copy_file_range / 436 close_range");
  {
    int fds[2] = {-1, -1};
    CHECK(SC2(59, fds, LX_O_CLOEXEC | 0x800 /*O_NONBLOCK*/) == 0, "pipe2 failed");
    CHECK(fds[0] >= 0 && fds[1] >= 0, "pipe2 did not fill the fd pair");
    CHECK((fcntl(fds[0], F_GETFD) & FD_CLOEXEC) != 0, "pipe2 O_CLOEXEC");
    CHECK((fcntl(fds[1], F_GETFL) & O_NONBLOCK) != 0, "pipe2 O_NONBLOCK");
    CHECK(write(fds[1], "pp", 2) == 2, "pipe2 write");
    char pb[4] = {0};
    CHECK(read(fds[0], pb, 2) == 2 && memcmp(pb, "pp", 2) == 0, "pipe2 read");
    close(fds[0]); close(fds[1]);

    int src = open(H("/cfr_src"), O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(src, "COPYMEPLEASE", 12);
    int dst = open(H("/cfr_dst"), O_RDWR | O_CREAT | O_TRUNC, 0644);
    long long ioff = 0, ooff = 0;
    long n = SC5(285, src, &ioff, dst, &ooff, 12);
    CHECK(n == 12, "copy_file_range -> %ld want 12", n);
    CHECK(ioff == 12 && ooff == 12, "copy_file_range offsets not advanced (%lld/%lld)", ioff, ooff);
    char cb[16] = {0};
    CHECK(pread(dst, cb, 12, 0) == 12 && memcmp(cb, "COPYMEPLEASE", 12) == 0,
          "copy_file_range content '%s'", cb);

    lseek(src, 0, SEEK_SET);
    int dst2 = open(H("/cfr_dst"), O_WRONLY | O_TRUNC);
    long long soff = 0;
    CHECK(SC4(71, dst2, src, &soff, 12) == 12, "sendfile -> not 12");
    CHECK(soff == 12, "sendfile did not advance the offset");
    close(src); close(dst); close(dst2);

    // close_range over a pair of high fds
    int a = open(H("/f1.txt"), O_RDONLY), b = open(H("/f1.txt"), O_RDONLY);
    int A = (int)SC3(24, a, 40, 0), B = (int)SC3(24, b, 41, 0);
    CHECK(A == 40 && B == 41, "dup3 to high fds");
    CHECK(SC3(436, 40, 41, 0) == 0, "close_range failed");
    CHECK(fcntl(40, F_GETFD) < 0 && fcntl(41, F_GETFD) < 0, "close_range left fds open");
    close(a); close(b);
  }

  // --------------------------------------------------------------------- xattr
  SECTION("5-16 xattr / 101 nanosleep");
  {
    long r = SC5(5, "/f1.txt", "user.rdx", "VAL", 3, 0);       // setxattr
    CHECK(r == 0, "setxattr -> %ld", r);
    char v[16] = {0};
    long n = SC5(8, "/f1.txt", "user.rdx", v, sizeof v, 0);    // getxattr
    CHECK(n == 3 && memcmp(v, "VAL", 3) == 0, "getxattr -> %ld '%s'", n, v);
    char names[512] = {0};
    CHECK(SC3(11, "/f1.txt", names, sizeof names) > 0, "listxattr found nothing");
    CHECK(SC5(5, "/f1.txt", "user.rdx", "X", 1, 1 /*XATTR_CREATE*/) == -17 /*EEXIST*/,
          "setxattr XATTR_CREATE over an existing attr must be -EEXIST");
    CHECK(SC2(14, "/f1.txt", "user.rdx") == 0, "removexattr failed");
    CHECK(SC5(8, "/f1.txt", "user.rdx", v, sizeof v, 0) < 0, "getxattr after remove should fail");

    struct timespec req = { 0, 2000000 }, rem = { 0, 0 };      // 2ms
    CHECK(SC2(101, &req, &rem) == 0, "nanosleep failed");
  }

  // ------------------------------------------------------------------ time/proc
  SECTION("169 gettimeofday / 153 times / 124 sched_yield / 125-126 sched prio");
  {
    struct t_timeval tv; memset(&tv, 0, sizeof tv);
    int tz[2] = {-1, -1};
    CHECK(SC2(169, &tv, tz) == 0, "gettimeofday failed");
    CHECK(tv.tv_sec > 1700000000LL, "gettimeofday tv_sec looks wrong: %lld", tv.tv_sec);
    CHECK(tv.tv_usec >= 0 && tv.tv_usec < 1000000,
          "gettimeofday tv_usec out of range: %lld (64-bit Linux layout?)", tv.tv_usec);
    CHECK(tz[0] == 0 && tz[1] == 0, "timezone not zeroed");
    CHECK(SC2(170, &tv, 0) == -LX_EPERM, "settimeofday must be -EPERM");

    struct t_tms tms; memset(&tms, 0xAA, sizeof tms);
    long t = SC1(153, &tms);
    CHECK(t > 0, "times -> %ld", t);
    CHECK(tms.utime >= 0 && tms.stime >= 0, "times filled nonsense");

    CHECK(SC0(124) == 0, "sched_yield");
    CHECK(SC1(125, 0) >= 0, "sched_get_priority_max(SCHED_NORMAL)");
    CHECK(SC1(126, 0) >= 0, "sched_get_priority_min(SCHED_NORMAL)");
    CHECK(SC1(125, 1) >= SC1(126, 1), "FIFO max < min?");
    CHECK(SC1(125, 77) == -LX_EINVAL, "bad sched policy must be -EINVAL");
    CHECK(SC3(116, 0, 0, 0) == 0, "syslog stub must return 0");
    CHECK(SC1(171, NULL) == 0, "adjtimex stub must return 0");
  }

  // -------------------------------------------------------------------- signals
  SECTION("130-138 signal-adjacent");
  {
    CHECK(SC2(130, getpid(), 0) == 0, "tkill(pid, 0) probe should succeed");
    CHECK(SC3(131, getpid(), getpid(), 0) == 0, "tgkill(.,.,0) probe should succeed");
    CHECK(SC2(130, getpid(), 34) == 0, "tkill with an RT signal must not fail");
    CHECK(SC2(130, getpid(), 99) == -LX_EINVAL, "tkill with a bogus signal must be -EINVAL");

    struct t_stack ss = { (void*)0x100000000UL, 0, 65536 }, oss;
    memset(&oss, 0xAA, sizeof oss);
    CHECK(SC2(132, &ss, &oss) == 0, "sigaltstack install failed");
    CHECK(oss.ss_flags == 2 /*SS_DISABLE*/, "initial sigaltstack should report SS_DISABLE");
    memset(&oss, 0, sizeof oss);
    CHECK(SC2(132, NULL, &oss) == 0, "sigaltstack query failed");
    CHECK(oss.ss_sp == (void*)0x100000000UL && oss.ss_size == 65536 && oss.ss_flags == 0,
          "sigaltstack did not round-trip (sp=%p size=%lu flags=%d)",
          oss.ss_sp, oss.ss_size, oss.ss_flags);
    struct t_stack tiny = { (void*)0x100000000UL, 0, 16 };
    CHECK(SC2(132, &tiny, NULL) == -12 /*ENOMEM*/, "undersized sigaltstack must be -ENOMEM");

    unsigned long set = 0xdeadbeef;
    CHECK(SC2(136, &set, 8) == 0, "rt_sigpending failed");
    CHECK(set == 0, "rt_sigpending must report an empty set");
    CHECK(SC2(133, NULL, 8) == -4 /*EINTR*/, "rt_sigsuspend must be -EINTR");
  }

  // --------------------------------------------------------------------- memory
  SECTION("227-237 memory hygiene / 270-271 process_vm_*");
  {
    size_t len = 65536;
    void* p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(p != MAP_FAILED, "mmap for memory tests");
    memset(p, 1, len);
    CHECK(SC3(227, p, len, 4 /*LX_MS_SYNC*/) == 0, "msync(MS_SYNC) failed");
    CHECK(SC3(227, p, len, 1 /*LX_MS_ASYNC*/) == 0, "msync(MS_ASYNC) failed");
    long ml = SC2(228, p, len);
    CHECK(ml == 0 || ml == -LX_EPERM, "mlock -> %ld (0 or -EPERM expected)", ml);
    if (ml == 0) CHECK(SC2(229, p, len) == 0, "munlock failed");
    // macOS munlockall() is ENOSYS; we must still report success to the guest.
    CHECK(SC0(231) == 0, "munlockall must report success, not a host -ENOSYS");
    long mla = SC1(230, 1 /*MCL_CURRENT*/);                    // mlockall: honestly may fail
    CHECK(mla == 0 || mla < 0, "mlockall -> %ld", mla);
    char* vec = malloc(len / 4096 + 1);
    long mc = SC3(232, p, len, vec);
    CHECK(mc == 0, "mincore -> %ld", mc);
    free(vec);
    CHECK(SC3(235, p, len, 0) == 0, "mbind stub must succeed");
    int mode = -1;
    CHECK(SC5(236, &mode, 0, 0, 0, 0) == 0 && mode == 0, "get_mempolicy stub");
    CHECK(SC3(237, 0, 0, 0) == 0, "set_mempolicy stub");
    munmap(p, len);

    // process_vm_readv/writev degenerate to memcpy: the guest shares our address space.
    char srcbuf[32], dstbuf[32];
    memcpy(srcbuf, "PROCESSVMTRANSFER0123456789ABC", 30);
    memset(dstbuf, 0, sizeof dstbuf);
    struct iovec liov = { dstbuf, 30 }, riov = { srcbuf, 30 };
    long n = sc(270, getpid(), (unsigned long)&liov, 1, (unsigned long)&riov, 1, 0);
    CHECK(n == 30, "process_vm_readv -> %ld want 30", n);
    CHECK(memcmp(dstbuf, srcbuf, 30) == 0, "process_vm_readv did not copy");
    memset(dstbuf, 0, sizeof dstbuf);
    struct iovec liov2 = { srcbuf, 30 }, riov2 = { dstbuf, 30 };
    n = sc(271, getpid(), (unsigned long)&liov2, 1, (unsigned long)&riov2, 1, 0);
    CHECK(n == 30 && memcmp(dstbuf, srcbuf, 30) == 0, "process_vm_writev did not copy");
  }

  // ---------------------------------------------------------- rlimits, faccessat2
  SECTION("163-164 rlimit / 439 faccessat2");
  {
    struct { unsigned long long cur, max; } rl;
    CHECK(SC2(163, 7 /*Linux RLIMIT_NOFILE*/, &rl) == 0, "getrlimit(NOFILE) failed");
    CHECK(rl.cur > 0, "RLIMIT_NOFILE cur = %llu", rl.cur);
    CHECK(SC2(163, 3 /*Linux RLIMIT_STACK*/, &rl) == 0, "getrlimit(STACK) failed");
    CHECK(SC2(163, 15 /*RLIMIT_RTTIME: no macOS analogue*/, &rl) == 0, "getrlimit(RTTIME)");
    CHECK(rl.cur == ~0ULL && rl.max == ~0ULL,
          "unmapped rlimit must report Linux RLIM_INFINITY (~0), got %#llx", rl.cur);

    CHECK(SC4(439, LX_AT_FDCWD, "/f1.txt", 0 /*F_OK*/, 0) == 0, "faccessat2 F_OK");
    CHECK(SC4(439, LX_AT_FDCWD, "/nope", 0, 0) == -LX_ENOENT, "faccessat2 ENOENT");
  }

  // ------------------------------------------------- deliberate refusals & ENOSYS
  SECTION("refusals (-EPERM) and genuine -ENOSYS");
  {
    CHECK(SC5(40, "/dev/x", "/mnt", "ext4", 0, 0) == -LX_EPERM, "mount must be -EPERM");
    CHECK(SC2(39, "/mnt", 0) == -LX_EPERM, "umount2 must be -EPERM");
    CHECK(SC1(51, "/newroot") == -LX_EPERM, "chroot must be -EPERM");
    CHECK(SC1(89, "/acct") == -LX_EPERM, "acct must be -EPERM");
    CHECK(SC0(58) == -LX_EPERM, "vhangup must be -EPERM");
    CHECK(SC5(294, 0, 0, 0, 0, 0) == -LX_EPERM, "kexec_file_load must be -EPERM");
    CHECK(SC3(280, 0, 0, 0) == -LX_EPERM, "bpf must be -EPERM");
    CHECK(SC0(142) == -LX_EPERM, "reboot must be -EPERM");
    CHECK(SC1(117, 0) == -LX_EPERM, "ptrace must be -EPERM");
    CHECK(SC1(97, 0) == -LX_EPERM, "unshare must be -EPERM");

    CHECK(SC5(217, 0, 0, 0, 0, 0) == -LX_ENOSYS, "add_key must be -ENOSYS");
    CHECK(SC3(194, 0, 0, 0) == -LX_ENOSYS, "shmget must be -ENOSYS");
    CHECK(SC3(196, 0, 0, 0) == -LX_ENOSYS, "shmat must be -ENOSYS");
    CHECK(SC5(281, 0, 0, 0, 0, 0) == -LX_ENOSYS, "execveat must be -ENOSYS");
    CHECK(SC5(292, 0, 0, 0, 0, 0) == -LX_ENOSYS, "io_pgetevents must be -ENOSYS");
    CHECK(SC5(234, 0, 0, 0, 0, 0) == -LX_ENOSYS, "remap_file_pages must be -ENOSYS");
    CHECK(SC5(435, 0, 0, 0, 0, 0) == -LX_ENOSYS, "clone3 must be -ENOSYS");

    // The contract: anything genuinely unimplemented is EXACTLY -38.
    CHECK(SC0(999999) == -38, "unknown syscall 999999 -> %ld, want -38", SC0(999999));
    CHECK(SC0(1234) == -38, "unknown syscall 1234 -> %ld, want -38", SC0(1234));
    CHECK(SC0(500) == -38, "unknown syscall 500 -> %ld, want -38", SC0(500));
    CHECK(SC0(0) == -38, "syscall 0 (io_setup, core tier) -> %ld, want -38", SC0(0));
  }

  // ---------------------------------------------------------- root resolution
  SECTION("syscalls_ext_set_root path resolution");
  {
    // guest-absolute -> <root><path>; a bogus root must make the same guest path miss.
    CHECK(SC4(53, LX_AT_FDCWD, "/f1.txt", 0644, 0) == 0, "resolve under the real root");
    syscalls_ext_set_root(TESTROOT "/sub");
    CHECK(SC4(53, LX_AT_FDCWD, "/f1.txt", 0644, 0) == -LX_ENOENT,
          "path must re-resolve under the new root");
    syscalls_ext_set_root(TESTROOT);
    CHECK(SC4(53, LX_AT_FDCWD, "/f1.txt", 0644, 0) == 0, "resolve after restoring the root");
  }

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
