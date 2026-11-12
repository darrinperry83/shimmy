// syscalls.c — core Linux aarch64 -> macOS/BSD syscall translation.
//
// ABI (see ARCHITECTURE.md "Syscall ABI"): every return value is the RAW LINUX value.
//   >= 0            success
//   negative        NEGATIVE LINUX errno (-2 == ENOENT).  Never -1/errno.
// macOS errno numbers diverge from Linux above ~34, so every host failure goes through
// linux_errno_from_host().  Linux O_*/AT_*/signal/ioctl/fcntl *values* also differ and are
// translated here; PROT_* happen to match.
//
// "Guest pointers are host pointers": the guest shares our address space, so every guest
// pointer argument is dereferenced directly — there is no address translation anywhere.
//
// Memory: mmap/mprotect/munmap/brk/mremap MUST go through gmem_* (never raw mmap) — that is
// what keeps W^X and the svc/tpidr instruction rewriting correct.
//
// Anything not handled here tail-calls linux_syscall_ext() (the fallback tier).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <crt_externs.h>
#include <stdarg.h>
#include "guestthread.h"
#include <time.h>
#include <signal.h>
#include <termios.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <poll.h>
#include "drmasahi.h"
#include "v4l2cam.h"
#include "snd_alsa.h"
int g_idle_after_address;
static int g_idle_saw_address;
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/sysctl.h>

#include "syscalls.h"
#include "syscalls_ext.h"
#include "signalfd_shim.h"
#include "timerfd_shim.h"
#include "gmem.h"
#ifdef RDGPU_CLIENT
#include "rdgpu_mock_gate.h"
#endif

// ===========================================================================================
// Linux constants (values as seen by an aarch64 Linux guest)
// ===========================================================================================

// errno
#define LX_EPERM 1
#define LX_ENOENT 2
#define LX_ESRCH 3
#define LX_EINTR 4
#define LX_EIO 5
#define LX_EBADF 9
#define LX_EAGAIN 11
#define LX_EDEADLK 35
#define LX_ENOMEM 12
#define LX_EACCES 13
#define LX_EFAULT 14
#define LX_EBUSY 16
#define LX_EEXIST 17
#define LX_ENOTDIR 20
#define LX_EISDIR 21
#define LX_EINVAL 22
#define LX_ENFILE 23
#define LX_EMFILE 24
#define LX_ENOTTY 25
#define LX_ESPIPE 29
#define LX_ERANGE 34
#define LX_ENAMETOOLONG 36
#define LX_ENOSYS 38
#define LX_ENOTEMPTY 39
#define LX_ELOOP 40
#define LX_EOVERFLOW 75
#define LX_ETIMEDOUT 110

// open flags (aarch64 == the "arm/alpha-ish" musl variant, NOT plain asm-generic)
#define LO_ACCMODE   0x0003
#define LO_CREAT     0x0040
#define LO_EXCL      0x0080
#define LO_NOCTTY    0x0100
#define LO_TRUNC     0x0200
#define LO_APPEND    0x0400
#define LO_NONBLOCK  0x0800
#define LO_DSYNC     0x1000
#define LO_ASYNC     0x2000
#define LO_DIRECTORY 0x4000
#define LO_NOFOLLOW  0x8000
#define LO_DIRECT    0x10000
#define LO_LARGEFILE 0x20000
#define LO_NOATIME   0x40000
#define LO_CLOEXEC   0x80000
#define LO_SYNC      0x101000
#define LO_PATH      0x200000

// *at()
#define LX_AT_FDCWD            (-100)
#define LX_AT_SYMLINK_NOFOLLOW 0x100
#define LX_AT_REMOVEDIR        0x200
#define LX_AT_SYMLINK_FOLLOW   0x400
#define LX_AT_NO_AUTOMOUNT     0x800
#define LX_AT_EMPTY_PATH       0x1000

// clocks
#define LX_CLOCK_REALTIME           0
#define LX_CLOCK_MONOTONIC          1
#define LX_CLOCK_PROCESS_CPUTIME_ID 2
#define LX_CLOCK_THREAD_CPUTIME_ID  3
#define LX_CLOCK_MONOTONIC_RAW      4
#define LX_CLOCK_REALTIME_COARSE    5
#define LX_CLOCK_MONOTONIC_COARSE   6
#define LX_CLOCK_BOOTTIME           7

#define LX_TIMER_ABSTIME 1

// mmap (Linux values — gmem_mmap takes LINUX flags directly)
#define LM_SHARED    0x01
#define LM_PRIVATE   0x02
#define LM_FIXED     0x10
#define LM_ANONYMOUS 0x20

#define LX_USER_HZ 100

// ===========================================================================================
// errno translation
// ===========================================================================================

// macOS errno (index) -> Linux errno (value).  1..106 covers all of <sys/errno.h> on Darwin.
// Divergence starts at 11 (EDEADLK/EAGAIN swap) and gets wholesale above 34.
static const short k_errno_m2l[] = {
  /*   0 */ 0,
  /*   1 EPERM            */ 1,
  /*   2 ENOENT           */ 2,
  /*   3 ESRCH            */ 3,
  /*   4 EINTR            */ 4,
  /*   5 EIO              */ 5,
  /*   6 ENXIO            */ 6,
  /*   7 E2BIG            */ 7,
  /*   8 ENOEXEC          */ 8,
  /*   9 EBADF            */ 9,
  /*  10 ECHILD           */ 10,
  /*  11 EDEADLK          */ 35,   // Linux EDEADLK == 35
  /*  12 ENOMEM           */ 12,
  /*  13 EACCES           */ 13,
  /*  14 EFAULT           */ 14,
  /*  15 ENOTBLK          */ 15,
  /*  16 EBUSY            */ 16,
  /*  17 EEXIST           */ 17,
  /*  18 EXDEV            */ 18,
  /*  19 ENODEV           */ 19,
  /*  20 ENOTDIR          */ 20,
  /*  21 EISDIR           */ 21,
  /*  22 EINVAL           */ 22,
  /*  23 ENFILE           */ 23,
  /*  24 EMFILE           */ 24,
  /*  25 ENOTTY           */ 25,
  /*  26 ETXTBSY          */ 26,
  /*  27 EFBIG            */ 27,
  /*  28 ENOSPC           */ 28,
  /*  29 ESPIPE           */ 29,
  /*  30 EROFS            */ 30,
  /*  31 EMLINK           */ 31,
  /*  32 EPIPE            */ 32,
  /*  33 EDOM             */ 33,
  /*  34 ERANGE           */ 34,
  /*  35 EAGAIN           */ 11,   // Linux EAGAIN == 11
  /*  36 EINPROGRESS      */ 115,
  /*  37 EALREADY         */ 114,
  /*  38 ENOTSOCK         */ 88,
  /*  39 EDESTADDRREQ     */ 89,
  /*  40 EMSGSIZE         */ 90,
  /*  41 EPROTOTYPE       */ 91,
  /*  42 ENOPROTOOPT      */ 92,
  /*  43 EPROTONOSUPPORT  */ 93,
  /*  44 ESOCKTNOSUPPORT  */ 94,
  /*  45 ENOTSUP          */ 95,
  /*  46 EPFNOSUPPORT     */ 96,
  /*  47 EAFNOSUPPORT     */ 97,
  /*  48 EADDRINUSE       */ 98,
  /*  49 EADDRNOTAVAIL    */ 99,
  /*  50 ENETDOWN         */ 100,
  /*  51 ENETUNREACH      */ 101,
  /*  52 ENETRESET        */ 102,
  /*  53 ECONNABORTED     */ 103,
  /*  54 ECONNRESET       */ 104,
  /*  55 ENOBUFS          */ 105,
  /*  56 EISCONN          */ 106,
  /*  57 ENOTCONN         */ 107,
  /*  58 ESHUTDOWN        */ 108,
  /*  59 ETOOMANYREFS     */ 109,
  /*  60 ETIMEDOUT        */ 110,
  /*  61 ECONNREFUSED     */ 111,
  /*  62 ELOOP            */ 40,
  /*  63 ENAMETOOLONG     */ 36,
  /*  64 EHOSTDOWN        */ 112,
  /*  65 EHOSTUNREACH     */ 113,
  /*  66 ENOTEMPTY        */ 39,
  /*  67 EPROCLIM         */ 11,   // no Linux equivalent -> EAGAIN
  /*  68 EUSERS           */ 87,
  /*  69 EDQUOT           */ 122,
  /*  70 ESTALE           */ 116,
  /*  71 EREMOTE          */ 66,
  /*  72 EBADRPC          */ 22,   // no Linux equivalent -> EINVAL
  /*  73 ERPCMISMATCH     */ 22,
  /*  74 EPROGUNAVAIL     */ 22,
  /*  75 EPROGMISMATCH    */ 22,
  /*  76 EPROCUNAVAIL     */ 22,
  /*  77 ENOLCK           */ 37,
  /*  78 ENOSYS           */ 38,
  /*  79 EFTYPE           */ 22,
  /*  80 EAUTH            */ 1,
  /*  81 ENEEDAUTH        */ 1,
  /*  82 EPWROFF          */ 5,
  /*  83 EDEVERR          */ 5,
  /*  84 EOVERFLOW        */ 75,
  /*  85 EBADEXEC         */ 8,
  /*  86 EBADARCH         */ 8,
  /*  87 ESHLIBVERS       */ 8,
  /*  88 EBADMACHO        */ 8,
  /*  89 ECANCELED        */ 125,
  /*  90 EIDRM            */ 43,
  /*  91 ENOMSG           */ 42,
  /*  92 EILSEQ           */ 84,
  /*  93 ENOATTR          */ 61,   // Linux ENOATTR == ENODATA == 61
  /*  94 EBADMSG          */ 74,
  /*  95 EMULTIHOP        */ 72,
  /*  96 ENODATA          */ 61,
  /*  97 ENOLINK          */ 67,
  /*  98 ENOSR            */ 63,
  /*  99 ENOSTR           */ 60,
  /* 100 EPROTO           */ 71,
  /* 101 ETIME            */ 62,
  /* 102 EOPNOTSUPP       */ 95,
  /* 103 ENOPOLICY        */ 1,
  /* 104 ENOTRECOVERABLE  */ 131,
  /* 105 EOWNERDEAD       */ 130,
  /* 106 EQFULL           */ 105,  // interface queue full -> ENOBUFS
};

int linux_errno_from_host(int host_errno) {
  if (host_errno <= 0) return 0;
  if ((size_t)host_errno < sizeof k_errno_m2l / sizeof k_errno_m2l[0])
    return (int)k_errno_m2l[host_errno];
  return LX_EINVAL;
}

// Wrap a host call that returns -1/errno into the Linux ABI.
static long herr(long r) { return r < 0 ? -(long)linux_errno_from_host(errno) : r; }
static long herrno(void)  { return -(long)linux_errno_from_host(errno); }

// ===========================================================================================
// Shim-owned descriptors (see the comment on shim_fd_reserve in syscalls.h)
// ===========================================================================================

#define SHIM_FD_MAX 16
static int             g_shim_fds[SHIM_FD_MAX];
static int             g_shim_nfd;
static pthread_mutex_t g_shim_fd_mu = PTHREAD_MUTEX_INITIALIZER;

void shim_fd_reserve(int fd) {
  if (fd < 0) return;
  pthread_mutex_lock(&g_shim_fd_mu);
  for (int i = 0; i < g_shim_nfd; i++) if (g_shim_fds[i] == fd) { pthread_mutex_unlock(&g_shim_fd_mu); return; }
  if (g_shim_nfd < SHIM_FD_MAX) g_shim_fds[g_shim_nfd++] = fd;
  pthread_mutex_unlock(&g_shim_fd_mu);
}

void shim_fd_unreserve(int fd) {
  pthread_mutex_lock(&g_shim_fd_mu);
  for (int i = 0; i < g_shim_nfd; i++)
    if (g_shim_fds[i] == fd) { g_shim_fds[i] = g_shim_fds[--g_shim_nfd]; break; }
  pthread_mutex_unlock(&g_shim_fd_mu);
}

int shim_fd_reserved(int fd) {
  if (fd < 0) return 0;
  pthread_mutex_lock(&g_shim_fd_mu);
  int found = 0;
  for (int i = 0; i < g_shim_nfd; i++) if (g_shim_fds[i] == fd) { found = 1; break; }
  pthread_mutex_unlock(&g_shim_fd_mu);
  return found;
}

// ===========================================================================================
// Module state: guest fs root, guest cwd, guest exe path
// ===========================================================================================

#define GPATH_MAX 4096

static char g_root[PATH_MAX];      // host path of the guest fs root, no trailing '/'
static size_t g_rootlen;
static char g_cwd[GPATH_MAX] = "/";     // guest-absolute cwd
static char g_exe[GPATH_MAX] = "/proc/self/exe";  // guest path of the running program
static int  g_trace;
int g_uid0;                        // RDUID0=1: report uid/gid 0 to the guest. See case 174.

void syscalls_set_root(const char* root) {
  g_root[0] = 0; g_rootlen = 0;
  if (!root || !*root) return;
  char rp[PATH_MAX];
  if (realpath(root, rp)) snprintf(g_root, sizeof g_root, "%s", rp);
  else                    snprintf(g_root, sizeof g_root, "%s", root);
  size_t n = strlen(g_root);
  while (n > 1 && g_root[n-1] == '/') g_root[--n] = 0;
  if (n == 1 && g_root[0] == '/') { g_root[0] = 0; n = 0; }   // root "/" == no prefix
  g_rootlen = n;
  g_trace = getenv("RDSCTRACE") != NULL;
  g_uid0  = getenv("RDUID0")   != NULL;
}

// CWD ACROSS execve. The shim services execve by re-exec'ing ITSELF (shim.c guest_execve), so the
// new shim is a brand new host process and every static here starts over -- including g_cwd, which
// reverts to "/". On Linux the cwd survives execve; here it silently did not, and because the
// resolver prepends g_cwd to every RELATIVE path, an exec'd program wrote its files to the ROOT of
// the rootfs while returning success at every step.
//
// That is how it was found: `dpkg-deb --control foo.deb /root/ctltest` chdir()s into the output
// directory and then exec()s tar. tar extracted perfectly -- into <rootfs>/control, /md5sums,
// /symbols, /shlibs -- and dpkg then reported "failed to open '/var/lib/dpkg/tmp.ci/control'".
// Nothing in the chain returned an error. Anything that does `cd X && exec prog` is affected:
// make, configure, tar's own -C in a subshell, every package manager.
//
// The fix is to carry it: guest_execve exports RDGUESTCWD, and the new shim restores it here.
// getcwd() is a real syscall the guest can observe, so the invariant is directly checkable --
// /bin/pwd (a separate exec'd binary) must print what the parent chdir'd to.
const char* syscalls_get_cwd(void) { return g_cwd; }

void syscalls_set_cwd(const char* gcwd) {
  if (!gcwd || gcwd[0] != '/') return;
  snprintf(g_cwd, sizeof g_cwd, "%s", gcwd);
}

void syscalls_set_exe(const char* guest_path) {
  if (guest_path && *guest_path) snprintf(g_exe, sizeof g_exe, "%s", guest_path);
}

// ===========================================================================================
// Path resolution
// ===========================================================================================

// Append `src` to the buffer at *pn, collapsing "//" and "/./".  ".." is deliberately left
// alone so the HOST resolves it against the real symlink topology.
static void path_cat(char* dst, size_t cap, size_t* pn, const char* src) {
  size_t n = *pn;
  for (const char* s = src; *s; s++) {
    if (*s == '/') {
      if (n && dst[n-1] == '/') continue;                       // collapse "//"
      if (n >= 2 && dst[n-1] == '.' && dst[n-2] == '/') { n--; continue; }  // collapse "/./"
    }
    if (n + 1 < cap) dst[n++] = *s;
  }
  dst[n] = 0;
  *pn = n;
}

static void path_finish(char* dst, size_t* pn) {
  size_t n = *pn;
  if (n >= 2 && dst[n-1] == '.' && dst[n-2] == '/') n--;        // trailing "/."
  if (n > 1 && dst[n-1] == '/') n--;                            // trailing "/"
  if (n == 0) { dst[n++] = '/'; }
  dst[n] = 0;
  *pn = n;
}

// Host path of an open fd, mapped back into the guest namespace.
static long guest_path_of_fd(int fd, char* out, size_t cap) {
  char hp[PATH_MAX];
  if (fcntl(fd, F_GETPATH, hp) < 0) return herrno();
  if (g_rootlen && strncmp(hp, g_root, g_rootlen) == 0 &&
      (hp[g_rootlen] == '/' || hp[g_rootlen] == 0)) {
    const char* g = hp + g_rootlen;
    snprintf(out, cap, "%s", *g ? g : "/");
  } else {
    snprintf(out, cap, "%s", hp);                               // outside the root (e.g. /dev/*)
  }
  return 0;
}

// (dirfd, path) -> guest-absolute path.  Honours AT_FDCWD (-100) and relative paths.
static long guest_abs(int dirfd, const char* path, char* out, size_t cap) {
  if (!path) return -LX_EFAULT;
  size_t n = 0;
  out[0] = 0;
  if (path[0] == '/') {
    path_cat(out, cap, &n, path);
  } else {
    if (dirfd == LX_AT_FDCWD) {
      path_cat(out, cap, &n, g_cwd);
    } else if (dirfd >= 0) {
      char base[GPATH_MAX];
      long r = guest_path_of_fd(dirfd, base, sizeof base);
      if (r < 0) return r;
      path_cat(out, cap, &n, base);
    } else {
      return -LX_EBADF;
    }
    if (!path[0]) return -LX_ENOENT;            // "" without AT_EMPTY_PATH is ENOENT on Linux
    path_cat(out, cap, &n, "/");
    path_cat(out, cap, &n, path);
  }
  path_finish(out, &n);
  if (n + 1 >= cap) return -LX_ENAMETOOLONG;
  return 0;
}

// RDDEVNULL=<path>: hand the guest a REAL FILE when it asks for /dev/null.
//
// An app that redirects its own 0/1/2 to /dev/null before it says anything is indistinguishable
// from an app with nothing to say — kate does exactly this, three dup3()s before Qt has even
// read qtlogging.ini, and then aborts with a message nobody has ever read. Pointing /dev/null at
// a file we can open afterwards recovers that message without special-casing dup3, without
// changing what the guest does, and without the guest being able to tell.
static const char* devnull_override(void) {
  static const char* p; static int done;
  if (!done) {
    done = 1;
    p = getenv("RDDEVNULL");
    if (p && !*p) p = 0;
    if (p) {                                  // create it now; the guest opens O_WRONLY, not O_CREAT
      int fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (fd >= 0) close(fd); else p = 0;
    }
  }
  return p;
}

// Guest device nodes we service with the host's own (the rootfs tarball has no real dev nodes).
// Isolated PTY experiment: map Linux /dev/pts/N onto one host-owned Darwin
// pty slave. Only a master returned from /dev/ptmx can create this mapping.
static struct { int master, unlocked; char slave[128]; } g_guest_pty[64];
static int pty_index(int fd) {
  for (int i=0; i<64; i++) if (g_guest_pty[i].master == fd && fd > 0) return i;
  return -1;
}
static void pty_register(int fd) {
  char name[128] = {0};
  // Darwin requires unlocking before the slave can service TIOCSWINSZ;
  // Foot sets size before its guest unlockpt call. Keep guest path access gated.
  if (ioctl(fd, TIOCPTYGRANT) || ioctl(fd, TIOCPTYGNAME, name) ||
      ioctl(fd, TIOCPTYUNLK)) {
    if (getenv("RDPTYTRACE")) fprintf(stderr, "[pty] pty_register(%d) FAILED errno=%s\n",
                                      fd, strerror(errno));
    return;
  }
  for (int i=0; i<64; i++) if (!g_guest_pty[i].master) {
    g_guest_pty[i].master=fd;
    g_guest_pty[i].unlocked=0;
    snprintf(g_guest_pty[i].slave,sizeof g_guest_pty[i].slave,"%s",name);
    if (getenv("RDPTYTRACE")) fprintf(stderr, "[pty] pty_register fd=%d slot=%d slave=%s\n",
                                      fd, i, name);
    return;
  }
  if (getenv("RDPTYTRACE")) fprintf(stderr, "[pty] pty_register fd=%d NO FREE SLOT\n", fd);
}
static const char* passthru_dev(const char* g) {
  if (!strncmp(g,"/dev/pts/",9)) {
    char* end; long n=strtol(g+9,&end,10);
    if (*end==0 && n>=0 && n<64 && g_guest_pty[n].slave[0] && g_guest_pty[n].unlocked)
      return g_guest_pty[n].slave;
  }
  static const char* const devs[] = { "/dev/ptmx", "/dev/null", "/dev/zero", "/dev/full", "/dev/random",
                                      "/dev/urandom", "/dev/tty", "/dev/stdin", "/dev/stdout",
                                      "/dev/stderr", 0 };
  if (strcmp(g, "/dev/null") == 0) { const char* o = devnull_override(); if (o) return o; }
  for (int i = 0; devs[i]; i++) if (strcmp(g, devs[i]) == 0) return devs[i];
  if (strncmp(g, "/dev/fd/", 8) == 0) return g;
  return 0;
}

// HOST DIRECTORIES THE GUEST SEES AT THEIR REAL PATHS.
//
// Everything else here pastes the rootfs in front of the guest path, which makes the rootfs an
// ISLAND: a developer cannot `cd ~/project` and run a binary against their own files, they have
// to copy the work into disk-sid/ first. That is the difference between a tool and a harness, and
// it matters more than any amount of window-manager integration for the people this is actually
// for -- ARM Linux developers who want their existing tree, edited in their existing editor.
//
// These prefixes are SAFE to map 1:1 because no Linux rootfs has them: /Users and /Volumes are
// macOS-only names, so passing them through adds paths without shadowing any guest file.
// RDSHARE overrides the list (colon-separated); RDSHARE= disables it entirely.
static int host_share(const char* g, const char** out_prefix) {
  static const char* defaults[] = { "/Users", "/Volumes", 0 };
  static const char* custom[8];
  static char buf[512];
  static int inited;
  if (!inited) {
    inited = 1;
    const char* e = getenv("RDSHARE");
    if (e) {
      snprintf(buf, sizeof buf, "%s", e);
      int n = 0;
      for (char* t = strtok(buf, ":"); t && n < 7; t = strtok(NULL, ":"))
        if (*t) custom[n++] = t;
      custom[n] = 0;
    }
  }
  const char* const* list = getenv("RDSHARE") ? custom : defaults;
  for (int i = 0; list[i]; i++) {
    size_t L = strlen(list[i]);
    if (!strncmp(g, list[i], L) && (g[L] == '/' || g[L] == 0)) {
      if (out_prefix) *out_prefix = list[i];
      return 1;
    }
  }
  return 0;
}

// guest-absolute -> host path.  Returns 0, or -ENAMETOOLONG if it would not fit.
static long guest_to_host(const char* gpath, char* out, size_t cap) {
  const char* pt = passthru_dev(gpath);
  int n;
  if (pt)                    n = snprintf(out, cap, "%s", pt);
  else if (host_share(gpath, NULL)) n = snprintf(out, cap, "%s", gpath);   // real macOS path
  else if (!g_rootlen)       n = snprintf(out, cap, "%s", gpath);
  else                       n = snprintf(out, cap, "%s%s", g_root, gpath);
  return (n < 0 || (size_t)n >= cap) ? -LX_ENAMETOOLONG : 0;
}

// ---- guest-side symlink resolution -----------------------------------------------------------
//
// THE BUG THIS EXISTS TO FIX. Until this function, guest_to_host() was the whole story: paste the
// rootfs in front of the guest path and hand the result to the host kernel. The host then follows
// any symlink itself — and a rootfs symlink's target is in GUEST coordinates, so an absolute one
// like /bin/cat -> /bin/busybox was resolved against the HOST's "/". 323 absolute symlinks in the
// shipping Alpine rootfs, 44 in the Debian one.
//
// The ENOENT half of that ("cat: can't open /bin/cat") is the harmless half and is not why this
// code is careful. The dangerous half is that when the host HAPPENS to have the path, the call
// SUCCEEDS and serves host bytes. Debian's OpenSSL CApath /usr/lib/ssl/certs -> /etc/ssl/certs
// found macOS's own /etc/ssl/certs, which exists and is empty, so certificate verification did
// not error — it succeeded against zero CAs and read as a bad server for as long as anyone looked.
// A guest reading /etc/passwd or /etc/hosts through such a link gets the host's, silently.
//
// So this is a CONTAINMENT boundary, not just a lookup fix, and it is written to that bar:
// resolution happens entirely in guest coordinates, and the host path is only ever formed as
// <root> + <guest-absolute-resolved>. Two consequences worth stating because they are the
// property being defended, not incidental:
//   * an absolute link target restarts at the GUEST root, never the host root;
//   * ".." is popped here, lexically, off the already-resolved path, so it clamps at "/" instead
//     of climbing out of the rootfs on the host. That was a SECOND, independent escape — no
//     symlink needed at all: "/../../../../../../../../tmp/x" read the host's /tmp/x.
// Popping ".." lexically is also what the kernel does, precisely BECAUSE symlinks are resolved as
// we go: by the time a ".." is applied, the path to its left contains no unresolved links.
//
// execve already did a version of this (shim.c guest_host_path), which is why every busybox
// applet ran and why a year of musl work never tripped over it. That copy only readlink()s the
// FULL path, so it misses a link in a DIRECTORY component — which is exactly the CApath shape.
// It is now retired in favour of this one; there is a single implementation.
#define LINK_HOPS_MAX 40                 // Linux's own SYMLOOP_MAX

// Resolve a guest-absolute path to a canonical guest-absolute path with symlinks followed.
// follow_final==0 leaves a symlink in the LAST component alone (readlink, lstat, unlink, rename).
// A trailing '/' forces the final component to be followed regardless, as on Linux.
static long guest_resolve_links(const char* gabs, int follow_final, char* out, size_t cap) {
  char res[GPATH_MAX];  size_t rl = 0;   res[0] = 0;   // resolved so far; "" means the guest root
  char rest[GPATH_MAX];
  if (strlen(gabs) >= sizeof rest) return -LX_ENAMETOOLONG;
  snprintf(rest, sizeof rest, "%s", gabs[0] == '/' ? gabs + 1 : gabs);

  for (int hops = 0; ; ) {
    while (rest[0] == '/') memmove(rest, rest + 1, strlen(rest));   // eat separators
    if (!rest[0]) break;

    size_t clen = strcspn(rest, "/");                                // next component
    char comp[GPATH_MAX];
    memcpy(comp, rest, clen); comp[clen] = 0;
    memmove(rest, rest + clen, strlen(rest) - clen + 1);             // rest now starts at '/' or 0

    if (strcmp(comp, ".") == 0) continue;
    if (strcmp(comp, "..") == 0) {                                   // pop; a no-op at the root
      char* s = strrchr(res, '/');
      if (s) { *s = 0; rl = (size_t)(s - res); } else { res[0] = 0; rl = 0; }
      continue;
    }

    // Is this the last component, and did the guest write a trailing '/'?
    int is_last = (strspn(rest, "/") == strlen(rest));
    int trailing_slash = is_last && rest[0] == '/';

    if (rl + 1 + clen + 1 > sizeof res) return -LX_ENAMETOOLONG;
    size_t save = rl;
    res[rl++] = '/'; memcpy(res + rl, comp, clen); rl += clen; res[rl] = 0;

    if (is_last && !follow_final && !trailing_slash) continue;       // leave the link itself

    char host[PATH_MAX];
    if (g_rootlen + rl + 1 > sizeof host) return -LX_ENAMETOOLONG;
    snprintf(host, sizeof host, "%s%s", g_root, res);
    char tgt[GPATH_MAX];
    ssize_t n = readlink(host, tgt, sizeof tgt - 1);
    if (n < 0) continue;                                             // not a link (or gone): keep
    tgt[n] = 0;
    if (++hops > LINK_HOPS_MAX) return -LX_ELOOP;

    // Splice the target in front of what is left. An absolute target restarts at the GUEST root.
    char next[GPATH_MAX];
    int k = snprintf(next, sizeof next, "%s%s", tgt, rest);
    if (k < 0 || (size_t)k >= sizeof next) return -LX_ENAMETOOLONG;
    snprintf(rest, sizeof rest, "%s", next);
    if (tgt[0] == '/') { res[0] = 0; rl = 0; }                       // absolute: back to guest "/"
    else               { res[save] = 0; rl = save; }                 // relative: undo this comp
  }

  if (rl == 0) { if (cap < 2) return -LX_ENAMETOOLONG; out[0] = '/'; out[1] = 0; return 0; }
  if (rl + 1 > cap) return -LX_ENAMETOOLONG;
  memcpy(out, res, rl + 1);
  return 0;
}

// Exported so there is exactly ONE resolver: syscalls_ext.c and shim.c (execve) call this.
// Returns 0 and fills `hostout` (and `gabsout`, if given, with the resolved GUEST path).
long syscalls_resolve(int lx_dirfd, const char* path, int nofollow_final,
                      char* hostout, size_t hcap, char* gabsout, size_t gcap);

#define RSV_NOFOLLOW 1                   // do not follow a symlink in the final component

// net.c binds named AF_UNIX sockets in RDSOCKETGROUP.  Consult that namespace only
// for an existing socket; ordinary files and symlinks retain rootfs resolution.
// The cheap miss is one lstat, while realpath is reserved for actual sockets.
// A socket reached through a symlinked group component must not escape the group.
static long group_socket_path(const char* guest, char* host, size_t cap) {
  const char* group = getenv("RDSOCKETGROUP");
  if (!group || group[0] != '/' || !guest || guest[0] != '/' ||
      !strcmp(guest, "/tmp/wayland-0") || !strcmp(guest, "/tmp/.X11-unix/X0")) return 0;
  char candidate[PATH_MAX], base[PATH_MAX], canonical[PATH_MAX];
  int n = snprintf(candidate, sizeof candidate, "%s%s", group, guest);
  if (n < 0 || (size_t)n >= sizeof candidate) return -LX_ENAMETOOLONG;
  struct stat st;
  if (lstat(candidate, &st) || !S_ISSOCK(st.st_mode)) return 0;
  if (!realpath(group, base) || !realpath(candidate, canonical)) return -LX_EACCES;
  size_t len = strlen(base);
  if (strncmp(canonical, base, len) || canonical[len] != '/') return -LX_EACCES;
  n = snprintf(host, cap, "%s", canonical);
  if (n < 0 || (size_t)n >= cap) return -LX_ENAMETOOLONG;
  return 1;
}

// The Wayland socket /tmp/wayland-N is namespaced to RDSOCKETGROUP by net.c's
// unix_path_to_host(), so the guest's /tmp/wayland-1 becomes <group>/tmp/wayland-1 (per-run).
// But libwayland's companion lockfile /tmp/wayland-1.lock is a PLAIN FILE opened through this
// resolver — and until this function it mapped to the SHARED rootfs /tmp/wayland-1.lock. A stale
// (or parallel) kiosk therefore held that one lockfile and the next compositor aborted with
// "unable to lock lockfile /tmp/wayland-1.lock" before ever binding its wayland-1 socket, so the
// desktop sidecars had no server to connect to and nothing mapped. Keep the lockfile in the same
// namespace as the socket it guards.
static long group_lockfile_path(const char* guest, char* host, size_t cap) {
  size_t gl = guest ? strlen(guest) : 0;
  if (gl < 19 || guest[0] != '/' || strncmp(guest, "/tmp/wayland-", 13) != 0 ||
      strcmp(guest + gl - 5, ".lock") != 0)
    return 0;
  // wlsrv.c binds the shim's own wayland-0 directly (no lockfile), and that socket lives in
  // RDUNIXPREFIX, not RDSOCKETGROUP — so do not relocate a guest's wayland-0.lock either.
  if (!strcmp(guest, "/tmp/wayland-0.lock")) return 0;
  const char* group = getenv("RDSOCKETGROUP");
  if (!group || group[0] != '/') return 0;
  // <group>/tmp is normally created by shim.c before the guest starts; ensure it anyway so the
  // O_CREAT open succeeds even if a guest compositor runs before the shim's own sockets exist.
  char dir[PATH_MAX];
  int n = snprintf(dir, sizeof dir, "%s/tmp", group);
  if (n < 0 || (size_t)n >= sizeof dir) return -LX_ENAMETOOLONG;
  mkdir(dir, 0700);
  n = snprintf(host, cap, "%s%s", group, guest);
  if (n < 0 || (size_t)n >= cap) return -LX_ENAMETOOLONG;
  return 1;
}

static long resolve_f(int dirfd, const char* path, int flags, char* host, size_t hcap,
                      char* gabs, size_t gcap) {
  char tmp[GPATH_MAX];
  char* g = gabs ? gabs : tmp;
  size_t gc = gabs ? gcap : sizeof tmp;
  long r = guest_abs(dirfd, path, g, gc);
  if (r < 0) return r;
  // The /dev passthrough list is matched on the path the guest ASKED for, before resolution, so
  // that /dev/null and friends keep going to the host's real device nodes (the rootfs has none).
  if (g_rootlen && !passthru_dev(g)) {
    char lr[GPATH_MAX];
    r = guest_resolve_links(g, !(flags & RSV_NOFOLLOW), lr, sizeof lr);
    if (r < 0) return r;
    snprintf(g, gc, "%s", lr);
  }
  // Keep the shim's special host devices and virtual proc paths authoritative.
  long sr = (passthru_dev(g) || !strncmp(g, "/proc/", 6)) ? 0 :
            group_socket_path(g, host, hcap);
  if (sr < 0) return sr;
  if (sr > 0) return 0;
  long lr = group_lockfile_path(g, host, hcap);
  if (lr < 0) return lr;
  if (lr > 0) return 0;
  return guest_to_host(g, host, hcap);
}

// The two things nearly every path-taking syscall needs.
static long resolve(int dirfd, const char* path, char* host, size_t hcap,
                    char* gabs, size_t gcap) {
  return resolve_f(dirfd, path, 0, host, hcap, gabs, gcap);
}

long syscalls_resolve(int lx_dirfd, const char* path, int nofollow_final,
                      char* hostout, size_t hcap, char* gabsout, size_t gcap) {
  return resolve_f(lx_dirfd, path, nofollow_final ? RSV_NOFOLLOW : 0,
                   hostout, hcap, gabsout, gcap);
}

// ===========================================================================================
// /proc/self virtual files
// ===========================================================================================

// An anonymous, seekable, stat-able fd backed by a file that is unlinked immediately.
static int mem_fd(const char* content, size_t len) {
  const char* td = getenv("TMPDIR");
  char tmpl[PATH_MAX];
  if (td && *td) snprintf(tmpl, sizeof tmpl, "%s%srdshim-XXXXXX", td,
                          td[strlen(td)-1] == '/' ? "" : "/");
  else           snprintf(tmpl, sizeof tmpl, "/var/tmp/rdshim-XXXXXX");
  int fd = mkstemp(tmpl);
  if (fd < 0) return -1;
  unlink(tmpl);
  size_t off = 0;
  while (off < len) {
    ssize_t w = write(fd, content + off, len - off);
    if (w <= 0) { close(fd); return -1; }
    off += (size_t)w;
  }
  lseek(fd, 0, SEEK_SET);
  return fd;
}

static unsigned long g_brk_base, g_brk_end;   // tentative: defined with the brk code below

// The MAIN guest thread's real stack extent, registered by the shim right after it builds the
// initial process stack. Zero means "not registered" (the unit tests load no guest).
static unsigned long g_mstack_lo, g_mstack_hi;
void syscalls_set_main_stack(unsigned long lo, unsigned long hi) {
  g_mstack_lo = lo; g_mstack_hi = hi;
}

// /proc/self/maps
//
// THE [stack] LINE IS LOAD-BEARING, NOT DECORATION. This used to report a hardcoded
// 0x7000000000-0x7000800000 while the guest's actual main stack lived wherever the shim's
// malloc put it. That is not a harmless stub:
//
//   glibc's pthread_getattr_np(), for the INITIAL thread only, has no thread descriptor to read
//   and so recovers the stack by scanning THIS FILE for the line whose range CONTAINS
//   __libc_stack_end. With a fabricated [stack] no line contains it, the scan finds nothing, and
//   the pthread_attr_t is left holding defaults -- i.e. bounds that have nothing to do with the
//   stack the thread is actually running on.
//
// WebKit's WebProcess died on exactly this, ~150 ms into startup, with no message (a release
// build's RELEASE_ASSERT prints nothing and abort() discards the block-buffered stderr):
//
//   JSC::VM::setLastStackTop  ->  RELEASE_ASSERT(stack.contains(m_lastStackTop))
//   Source/JavaScriptCore/runtime/VM.cpp:555  ->  WTFCrashWithInfoImpl  ->  abort()
//
// WTF::StackBounds::currentThreadStackBounds() is pthread_getattr_np + pthread_attr_getstack, so
// the bogus bounds propagate straight into an assertion that the running stack pointer is inside
// them. It read as "the web process hangs in a futex" for a whole day.
//
// Lines are emitted in ASCENDING address order because glibc's scan also clamps the size against
// the END OF THE PREVIOUS ENTRY; out-of-order lines make that clamp compute nonsense.
static void proc_maps(char* buf, size_t cap) {
  struct { unsigned long lo, hi; const char* perms; const char* name; } e[4];
  int n = 0;
  e[n].lo = 0x100000000UL; e[n].hi = 0x100100000UL; e[n].perms = "r-xp"; e[n].name = g_exe; n++;
  e[n].lo = 0x100100000UL; e[n].hi = 0x100200000UL; e[n].perms = "rw-p"; e[n].name = g_exe; n++;
  if (g_brk_base && g_brk_end > g_brk_base) {          // the real brk heap, not a guessed one
    e[n].lo = g_brk_base; e[n].hi = g_brk_end;
  } else {
    e[n].lo = 0x200000000UL; e[n].hi = 0x210000000UL;
  }
  e[n].perms = "rw-p"; e[n].name = "[heap]"; n++;
  if (g_mstack_hi > g_mstack_lo) {
    e[n].lo = g_mstack_lo; e[n].hi = g_mstack_hi;
  } else {
    e[n].lo = 0x7000000000UL; e[n].hi = 0x7000800000UL;
  }
  e[n].perms = "rw-p"; e[n].name = "[stack]"; n++;

  for (int i = 1; i < n; i++)                          // insertion sort: n == 4
    for (int j = i; j > 0 && e[j].lo < e[j - 1].lo; j--) {
      typeof(e[0]) t = e[j]; e[j] = e[j - 1]; e[j - 1] = t;
    }

  size_t off = 0;
  for (int i = 0; i < n && off < cap; i++)
    off += (size_t)snprintf(buf + off, cap - off, "%016lx-%016lx %s 00000000 00:00 0 %s\n",
                            e[i].lo, e[i].hi, e[i].perms, e[i].name);
}

// /proc/self/stat
//
// FIELD 28 IS `startstack`, AND IT IS THE SECOND WAY TO FIND THE MAIN THREAD'S STACK -- the one
// the Boehm GC uses. It was 0 here while /proc/self/maps was reporting a fabricated [stack], so
// both routes to the same number were wrong at once; fixing only maps would have left this trap
// armed for the next collector or runtime that reads it. Field 47 (`start_brk`) is likewise a
// real number we already have.
//
// Emitted with all 52 fields a modern kernel emits, not 39. A parser that indexes to `exit_code`
// on a 39-field line reads past the end, which is a different bug with the same symptom class.
static void proc_stat(char* buf, size_t cap) {
  unsigned long startstack = g_mstack_hi;                  // top of the main guest stack
  unsigned long start_brk  = g_brk_base;
  snprintf(buf, cap,
    "%d (%s) R %d %d %d 0 -1 4194304 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0 "
    "18446744073709551615 0 0 %lu 0 0 0 0 0 0 0 0 0 0 0 "   /* 26..39, 28 = startstack */
    "0 0 0 0 0 0 0 %lu 0 0 0 0 0\n",                        /* 40..52, 47 = start_brk  */
    (int)getpid(), "guest", (int)getppid(), (int)getpid(), (int)getpid(),
    startstack, start_brk);
}


// /proc/cpuinfo — LLVM's sys::getHostCPUName() parses this on aarch64 Linux to pick a target, and
// llvmpipe (via u_cpu_detect) reads it too. Without it LLVM logs
//     "Can't read /proc/cpuinfo: No such file or directory"
// and falls back to a generic CPU. The values below are Apple Silicon's real ones (implementer
// 0x61 = Apple); code generated for an M1 runs fine on later Apple cores.
static void proc_cpuinfo(char* b, size_t cap) {
  static const char kFeatures[] =
      "fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm jscvt "
      "fcma lrcpc dcpop sha3 sm3 sm4 asimddp sha512 asimdfhm dit uscat ilrcpc flagm ssbs sb "
      "paca pacg dcpodp flagm2 frint";
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1) n = 1;
  if (n > 64) n = 64;
  size_t o = 0;
  for (long i = 0; i < n && o + 512 < cap; i++)
    o += (size_t)snprintf(b + o, cap - o,
        "processor\t: %ld\n"
        "BogoMIPS\t: 48.00\n"
        "Features\t: %s\n"
        "CPU implementer\t: 0x61\n"
        "CPU architecture: 8\n"
        "CPU variant\t: 0x0\n"
        "CPU part\t: 0x022\n"
        "CPU revision\t: 0\n\n", i, kFeatures);
  if (o < cap) b[o] = 0;
}

// /proc/meminfo — Mesa's os_get_total_physical_memory() reads this; a missing file leaves it
// guessing, and llvmpipe sizes its shader/tile caches from the answer.
static void proc_meminfo(char* b, size_t cap) {
  unsigned long long total = 0;
  const char* override = getenv("RDNATIVE_TOTAL_MEM_GB");
  if (override && *override) {
    total = strtoull(override, NULL, 10) << 30;   // GB -> bytes (<<30)
    if (total == 0) total = 8ULL << 30;
  } else {
    size_t len = sizeof total;
    if (sysctlbyname("hw.memsize", &total, &len, NULL, 0) != 0 || total == 0)
      total = 8ULL << 30;
  }
  unsigned long long kb = total / 1024;
  snprintf(b, cap,
           "MemTotal:       %llu kB\n"
           "MemFree:        %llu kB\n"
           "MemAvailable:   %llu kB\n"
           "Buffers:               0 kB\n"
           "Cached:                0 kB\n"
           "SwapTotal:             0 kB\n"
           "SwapFree:              0 kB\n",
           kb, kb / 2, kb / 2);
}

// Returns 1 if the path is a virtual /proc file we serve; *fd is then the result (or -errno).
static int proc_open(const char* g, long* out) {
  char buf[4096];
  if (strcmp(g, "/proc/self/exe") == 0) {
    char host[PATH_MAX]; guest_to_host(g_exe, host, sizeof host);
    int fd = open(host, O_RDONLY | O_CLOEXEC);
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/self/cmdline") == 0) {
    size_t n = strlen(g_exe) + 1;
    memcpy(buf, g_exe, n);
    int fd = mem_fd(buf, n);
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/self/maps") == 0) {
    proc_maps(buf, sizeof buf);
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/cpuinfo") == 0) {
    static char cpub[16384];
    proc_cpuinfo(cpub, sizeof cpub);
    int fd = mem_fd(cpub, strlen(cpub));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/meminfo") == 0) {
    proc_meminfo(buf, sizeof buf);
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/self/stat") == 0) {
    proc_stat(buf, sizeof buf);
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  // /proc/asound/* — alsa-lib's snd_card_next() reads these to enumerate the synthetic card.
  if (strncmp(g, "/proc/asound", 12) == 0) {
    const char* c = snd_proc_content(g);
    int fd = mem_fd(c, strlen(c));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  return 0;
}

// ===========================================================================================
// Flag translation
// ===========================================================================================

static int open_flags_l2m(int lf) {
  int mf = lf & LO_ACCMODE;
  if ((lf & LO_ACCMODE) == LO_ACCMODE) mf = O_RDONLY;            // O_PATH-ish garbage
  if (lf & LO_CREAT)     mf |= O_CREAT;
  if (lf & LO_EXCL)      mf |= O_EXCL;
  if (lf & LO_NOCTTY)    mf |= O_NOCTTY;
  if (lf & LO_TRUNC)     mf |= O_TRUNC;
  if (lf & LO_APPEND)    mf |= O_APPEND;
  if (lf & LO_NONBLOCK)  mf |= O_NONBLOCK;
  if (lf & LO_DSYNC)     mf |= O_DSYNC;
  if (lf & LO_ASYNC)     mf |= O_ASYNC;
  if (lf & LO_DIRECTORY) mf |= O_DIRECTORY;
  if (lf & LO_NOFOLLOW)  mf |= O_NOFOLLOW;
  if (lf & LO_CLOEXEC)   mf |= O_CLOEXEC;
  if ((lf & LO_SYNC) == LO_SYNC) mf |= O_SYNC;
  // O_DIRECT / O_LARGEFILE / O_NOATIME have no macOS equivalent and must NOT leak through
  // (Linux O_LARGEFILE 0x20000 happens to be macOS O_NOCTTY).
  return mf;
}

static int open_flags_m2l(int mf) {
  int lf = mf & LO_ACCMODE;
  if (mf & O_CREAT)     lf |= LO_CREAT;
  if (mf & O_EXCL)      lf |= LO_EXCL;
  if (mf & O_NOCTTY)    lf |= LO_NOCTTY;
  if (mf & O_TRUNC)     lf |= LO_TRUNC;
  if (mf & O_APPEND)    lf |= LO_APPEND;
  if (mf & O_NONBLOCK)  lf |= LO_NONBLOCK;
  if (mf & O_DSYNC)     lf |= LO_DSYNC;
  if (mf & O_ASYNC)     lf |= LO_ASYNC;
  if (mf & O_DIRECTORY) lf |= LO_DIRECTORY;
  if (mf & O_NOFOLLOW)  lf |= LO_NOFOLLOW;
  if (mf & O_CLOEXEC)   lf |= LO_CLOEXEC;
  if (mf & O_SYNC)      lf |= LO_SYNC;
  return lf;
}

// Linux signal number -> macOS.  0 == no equivalent.
static const unsigned char k_sig_l2m[32] = {
  0,  SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGBUS,
  SIGFPE, SIGKILL, SIGUSR1, SIGSEGV, SIGUSR2, SIGPIPE, SIGALRM, SIGTERM,
  0 /*SIGSTKFLT*/, SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU, SIGURG,
  SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF, SIGWINCH, SIGIO, 0 /*SIGPWR*/, SIGSYS,
};

static int sig_l2m(int lsig) {
  if (lsig == 0) return 0;
  if (lsig < 0 || lsig > 31) return -1;
  int m = k_sig_l2m[lsig];
  return m ? m : -1;
}

// ===========================================================================================
// Linux struct layouts (aarch64)
// ===========================================================================================

struct linux_stat {                 // 128 bytes
  uint64_t st_dev;                  //   0
  uint64_t st_ino;                  //   8
  uint32_t st_mode;                 //  16
  uint32_t st_nlink;                //  20
  uint32_t st_uid;                  //  24
  uint32_t st_gid;                  //  28
  uint64_t st_rdev;                 //  32
  uint64_t __pad1;                  //  40
  int64_t  st_size;                 //  48
  int32_t  st_blksize;              //  56
  int32_t  __pad2;                  //  60
  int64_t  st_blocks;               //  64
  int64_t  st_atime_sec;            //  72
  int64_t  st_atime_nsec;           //  80
  int64_t  st_mtime_sec;            //  88
  int64_t  st_mtime_nsec;           //  96
  int64_t  st_ctime_sec;            // 104
  int64_t  st_ctime_nsec;           // 112
  uint32_t __unused4[2];            // 120
};
_Static_assert(sizeof(struct linux_stat) == 128, "linux_stat must be 128 bytes");
_Static_assert(offsetof(struct linux_stat, st_ino)       ==   8, "st_ino");
_Static_assert(offsetof(struct linux_stat, st_mode)      ==  16, "st_mode");
_Static_assert(offsetof(struct linux_stat, st_nlink)     ==  20, "st_nlink");
_Static_assert(offsetof(struct linux_stat, st_uid)       ==  24, "st_uid");
_Static_assert(offsetof(struct linux_stat, st_gid)       ==  28, "st_gid");
_Static_assert(offsetof(struct linux_stat, st_rdev)      ==  32, "st_rdev");
_Static_assert(offsetof(struct linux_stat, st_size)      ==  48, "st_size");
_Static_assert(offsetof(struct linux_stat, st_blksize)   ==  56, "st_blksize");
_Static_assert(offsetof(struct linux_stat, st_blocks)    ==  64, "st_blocks");
_Static_assert(offsetof(struct linux_stat, st_atime_sec) ==  72, "st_atim");
_Static_assert(offsetof(struct linux_stat, st_mtime_sec) ==  88, "st_mtim");
_Static_assert(offsetof(struct linux_stat, st_ctime_sec) == 104, "st_ctim");

struct linux_dirent64 {             // header is 19 bytes; d_name follows immediately
  uint64_t d_ino;                   //  0
  int64_t  d_off;                   //  8
  uint16_t d_reclen;                // 16
  uint8_t  d_type;                  // 18
  char     d_name[];                // 19
};
_Static_assert(offsetof(struct linux_dirent64, d_off)    ==  8, "d_off");
_Static_assert(offsetof(struct linux_dirent64, d_reclen) == 16, "d_reclen");
_Static_assert(offsetof(struct linux_dirent64, d_type)   == 18, "d_type");
_Static_assert(offsetof(struct linux_dirent64, d_name)   == 19, "d_name");

struct linux_statfs {               // 120 bytes
  int64_t  f_type, f_bsize;
  uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
  int32_t  f_fsid[2];
  int64_t  f_namelen, f_frsize, f_flags, f_spare[4];
};
_Static_assert(sizeof(struct linux_statfs) == 120, "linux_statfs must be 120 bytes");

struct linux_utsname { char sysname[65], nodename[65], release[65], version[65],
                            machine[65], domainname[65]; };
_Static_assert(sizeof(struct linux_utsname) == 390, "utsname must be 390 bytes");

struct linux_timespec { int64_t tv_sec, tv_nsec; };
struct linux_timeval  { int64_t tv_sec, tv_usec; };

struct linux_rusage {               // 144 bytes
  struct linux_timeval ru_utime, ru_stime;
  int64_t ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap,
          ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw;
};
_Static_assert(sizeof(struct linux_rusage) == 144, "linux_rusage must be 144 bytes");

struct linux_tms { int64_t tms_utime, tms_stime, tms_cutime, tms_cstime; };

struct linux_sysinfo {              // 112 bytes on 64-bit
  int64_t  uptime;
  uint64_t loads[3];
  uint64_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
  uint16_t procs, pad;
  uint64_t totalhigh, freehigh;
  uint32_t mem_unit;
  char     _f[0];
};

struct linux_rlimit64 { uint64_t rlim_cur, rlim_max; };

// NOTE: the members are deliberately NOT named sa_handler/sa_mask — those are macros in
// Darwin's <sys/signal.h>.  Layout is what matters: 4 x u64 in Linux order.
struct linux_sigaction {            // 32 bytes on aarch64
  uint64_t handler;                 //  0
  uint64_t flags;                   //  8
  uint64_t restorer;                // 16
  uint64_t mask;                    // 24
};
_Static_assert(sizeof(struct linux_sigaction) == 32, "linux_sigaction must be 32 bytes");

struct linux_flock {                // 32 bytes on aarch64
  int16_t  l_type;
  int16_t  l_whence;
  int32_t  __pad;
  int64_t  l_start;
  int64_t  l_len;
  int32_t  l_pid;
  int32_t  __pad2;
};
_Static_assert(sizeof(struct linux_flock) == 32, "linux_flock must be 32 bytes");

struct linux_termios { uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
                       uint8_t c_line, c_cc[19]; };
_Static_assert(sizeof(struct linux_termios) == 36, "linux_termios must be 36 bytes");

// Linux termios2: termios + split input/output speeds. foot (and newer glibc terminal apps) use
// TCGETS2/TCSETS2 rather than the legacy TCGETS/TCSETS.
struct linux_termios2 { uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
                        uint8_t c_line, c_cc[19]; uint32_t c_ispeed, c_ospeed; };
_Static_assert(sizeof(struct linux_termios2) == 44, "linux_termios2 must be 44 bytes");

struct linux_winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

// ===========================================================================================
// stat / statfs conversion
// ===========================================================================================

// S_IF* mode bits are identical between Linux and macOS, so st_mode copies straight across.
static void stat_m2l(const struct stat* s, struct linux_stat* l) {
  memset(l, 0, sizeof *l);
  l->st_dev        = (uint64_t)s->st_dev;
  l->st_ino        = (uint64_t)s->st_ino;
  l->st_mode       = (uint32_t)s->st_mode;
  l->st_nlink      = (uint32_t)s->st_nlink;
  // Under RDUID0 the guest is uid/gid 0; report every file as owned by root so stat() agrees with
  // getuid(). Without this, dbus-daemon refuses to set up its transient service directory
  // ("XDG_RUNTIME_DIR /tmp is owned by uid 501, not our uid 0"), which breaks D-Bus service
  // activation (org.kde.KWin / org.kde.kglobalaccel "not provided by any .service files").
  l->st_uid        = g_uid0 ? 0 : (uint32_t)s->st_uid;
  l->st_gid        = g_uid0 ? 0 : (uint32_t)s->st_gid;
  l->st_rdev       = (uint64_t)s->st_rdev;
  l->st_size       = (int64_t)s->st_size;
  l->st_blksize    = (int32_t)s->st_blksize;
  l->st_blocks     = (int64_t)s->st_blocks;
  l->st_atime_sec  = s->st_atimespec.tv_sec;  l->st_atime_nsec = s->st_atimespec.tv_nsec;
  l->st_mtime_sec  = s->st_mtimespec.tv_sec;  l->st_mtime_nsec = s->st_mtimespec.tv_nsec;
  l->st_ctime_sec  = s->st_ctimespec.tv_sec;  l->st_ctime_nsec = s->st_ctimespec.tv_nsec;
}

static void statfs_m2l(const struct statfs* s, struct linux_statfs* l) {
  memset(l, 0, sizeof *l);
  l->f_type    = 0xEF53;                      // claim ext2/3/4; nothing sane keys off this
  l->f_bsize   = (int64_t)s->f_bsize;
  l->f_blocks  = s->f_blocks;
  l->f_bfree   = s->f_bfree;
  l->f_bavail  = (uint64_t)s->f_bavail;
  l->f_files   = s->f_files;
  l->f_ffree   = (uint64_t)s->f_ffree;
  l->f_fsid[0] = s->f_fsid.val[0];
  l->f_fsid[1] = s->f_fsid.val[1];
  l->f_namelen = 255;
  l->f_frsize  = (int64_t)s->f_bsize;
  l->f_flags   = (s->f_flags & MNT_RDONLY) ? 1 : 0;
}

static void rusage_m2l(const struct rusage* r, struct linux_rusage* l) {
  memset(l, 0, sizeof *l);
  l->ru_utime.tv_sec = r->ru_utime.tv_sec; l->ru_utime.tv_usec = r->ru_utime.tv_usec;
  l->ru_stime.tv_sec = r->ru_stime.tv_sec; l->ru_stime.tv_usec = r->ru_stime.tv_usec;
  l->ru_maxrss   = r->ru_maxrss / 1024;       // macOS reports bytes, Linux kilobytes
  l->ru_ixrss    = r->ru_ixrss;
  l->ru_idrss    = r->ru_idrss;
  l->ru_isrss    = r->ru_isrss;
  l->ru_minflt   = r->ru_minflt;
  l->ru_majflt   = r->ru_majflt;
  l->ru_nswap    = r->ru_nswap;
  l->ru_inblock  = r->ru_inblock;
  l->ru_oublock  = r->ru_oublock;
  l->ru_msgsnd   = r->ru_msgsnd;
  l->ru_msgrcv   = r->ru_msgrcv;
  l->ru_nsignals = r->ru_nsignals;
  l->ru_nvcsw    = r->ru_nvcsw;
  l->ru_nivcsw   = r->ru_nivcsw;
}

// ===========================================================================================
// Per-fd DIR* map for getdents64 (the guest is effectively single-threaded)
// ===========================================================================================

#define DIRMAP_MAX 64
static struct { int fd; DIR* d; } g_dirs[DIRMAP_MAX];

static DIR* dirmap_get(int fd) {
  for (int i = 0; i < DIRMAP_MAX; i++) if (g_dirs[i].d && g_dirs[i].fd == fd) return g_dirs[i].d;
  return 0;
}

static DIR* dirmap_open(int fd) {
  DIR* d = dirmap_get(fd);
  if (d) return d;
  int slot = -1;
  for (int i = 0; i < DIRMAP_MAX; i++) if (!g_dirs[i].d) { slot = i; break; }
  if (slot < 0) { errno = EMFILE; return 0; }
  int dup_fd = dup(fd);                       // fdopendir takes ownership; keep the guest's fd
  if (dup_fd < 0) return 0;
  d = fdopendir(dup_fd);
  if (!d) { int e = errno; close(dup_fd); errno = e; return 0; }
  g_dirs[slot].fd = fd; g_dirs[slot].d = d;
  return d;
}

static void dirmap_close(int fd) {
  for (int i = 0; i < DIRMAP_MAX; i++)
    if (g_dirs[i].d && g_dirs[i].fd == fd) { closedir(g_dirs[i].d); g_dirs[i].d = 0; g_dirs[i].fd = -1; }
}

// ===========================================================================================
// brk
// ===========================================================================================

#define BRK_SIZE (256UL * 1024 * 1024)
static unsigned long g_brk_cur;

static long do_brk(unsigned long want) {
  if (!g_brk_base) {
    long r = gmem_mmap(0, BRK_SIZE, PROT_READ | PROT_WRITE, LM_PRIVATE | LM_ANONYMOUS, -1, 0);
    if (r < 0 && r > -4096) return r;
    g_brk_base = g_brk_cur = (unsigned long)r;
    g_brk_end  = g_brk_base + BRK_SIZE;
  }
  if (want == 0) return (long)g_brk_cur;
  if (want >= g_brk_base && want <= g_brk_end) { g_brk_cur = want; return (long)g_brk_cur; }
  return (long)g_brk_cur;                     // Linux returns the unchanged brk on failure
}

// ===========================================================================================
// futex (userspace; HASHED PER-ADDRESS wait queues)
//
// This was one global mutex + condvar, with FUTEX_WAKE as pthread_cond_broadcast. That is fine
// for a single-threaded guest and was correct until llvmpipe became the first real multithreaded
// consumer the shim has had. Then it cost ~47x: 1029 futex calls accounted for the entire
// single-vs-threaded gap on osdemo (55s vs 2s), with every other syscall within 1.0x.
//
// Three separate defects, all removed by keying on the address:
//   1. Thundering herd -- every wake woke every waiter on every address; N-1 re-checked and slept
//      again, which is O(N^2) in rasteriser threads.
//   2. Global serialisation -- every wait and every wake contended one mutex.
//   3. WRONG RETURN VALUE, a correctness bug hiding inside the perf bug: FUTEX_WAKE returned the
//      count of ALL waiters rather than waiters on that address, misinforming guest code that
//      trusts it.
//
// Waiters park on a bucket condvar but each holds its own record with its address and a `woken`
// flag, so a wake targets exactly the matching addresses and a waiter that is merely sharing a
// bucket goes straight back to sleep. Waiting loops on that flag rather than returning on any
// wakeup: spurious returns are legal for FUTEX_WAIT, but they make the guest re-issue the
// syscall, which is the very cost being removed here.
// ===========================================================================================

#define FUTEX_WAIT             0
#define FUTEX_WAKE             1
#define FUTEX_WAKE_OP          5
#define FUTEX_REQUEUE          3
#define FUTEX_CMP_REQUEUE      4
#define FUTEX_WAIT_BITSET      9
#define FUTEX_WAKE_BITSET     10
#define FUTEX_LOCK_PI          6
#define FUTEX_UNLOCK_PI        7
#define FUTEX_TRYLOCK_PI       8
#define FUTEX_WAIT_REQUEUE_PI 11
#define FUTEX_CMP_REQUEUE_PI  12
#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256
// PI-futex word layout: low 30 bits = owner TID, bit 30 = waiters present, 0 = unlocked.
#define FUTEX_TID_MASK        0x3fffffffu
#define FUTEX_WAITERS         0x40000000u

// ---------------------------------------------------------------------------------------------
// RDFUTEX=<prefix>: diagnostic futex trace. One file per process, "<prefix>.<pid>.log", so a
// fork/exec child (WebKit's web process) does not interleave with its parent. DIAGNOSTIC ONLY --
// off unless the env var is set, and nothing on the fast path runs when it is off.
//
// What it must answer: for the ONE wait that never returns, what address, what bitset, what
// value was compared, which bucket, which guest thread -- and did any FUTEX_WAKE ever name that
// same address. A wake log that only counts wakes cannot answer that, so every wake records its
// address and how many waiters it found, including zero.
// ---------------------------------------------------------------------------------------------
// The shim services the guest's execve by loading the new ELF into a FORK OF ITSELF, so a child
// inherits this file's statics -- including an open FILE* belonging to the parent. Without the
// pid re-check every WebKit process would append to the UI process's log and the trace would be
// unattributable, which is precisely the measurement being made here.
static int   g_fx_on = -1;                    // -1 not yet decided, 0 off, 1 on
static int   g_fx_pid;                        // pid the open FILE* belongs to
static FILE* g_fx_f;
static pthread_mutex_t g_fx_mu    = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_fx_openmu = PTHREAD_MUTEX_INITIALIZER;
static double fx_now(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}
static void fx_watchdog_start(void);
// A process that abort()s writes nothing further, and "the trace stops" then reads identically to
// "the process hung" -- the two answers this whole exercise has to tell apart. Log the signal,
// then restore the default and re-raise so behaviour is unchanged.
static int  g_fx_deathfd = -1;
static void fx_death(int sig) {
  char b[64];
  int n = snprintf(b, sizeof b, "\nDIED signal=%d\n", sig);
  if (g_fx_deathfd >= 0) { ssize_t r = write(g_fx_deathfd, b, (size_t)n); (void)r; }
  signal(sig, SIG_DFL);
  raise(sig);
}
static void fx_death_hook(void) {
  g_fx_deathfd = fileno(g_fx_f);
  // ONLY where the disposition is still SIG_DFL. Replacing an existing handler (the shim ignores
  // SIGPIPE, for one) would turn a survivable signal into a kill and manufacture the very death
  // this is here to observe.
  int sigs[] = { SIGABRT, SIGQUIT, SIGTERM, SIGPIPE, SIGSYS, SIGFPE };
  for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
    struct sigaction old;
    if (sigaction(sigs[i], NULL, &old) != 0) continue;
    if (old.sa_handler != SIG_DFL) continue;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = fx_death;
    sigaction(sigs[i], &sa, NULL);
  }
}
static int fx_on(void) {
  int pid = (int)getpid();
  if (g_fx_on == 0) return 0;
  if (g_fx_on == 1 && g_fx_pid == pid) return 1;
  pthread_mutex_lock(&g_fx_openmu);
  if (g_fx_on == 1 && g_fx_pid == pid) { pthread_mutex_unlock(&g_fx_openmu); return 1; }
  const char* p = getenv("RDFUTEX");
  if (!p || !*p) { g_fx_on = 0; pthread_mutex_unlock(&g_fx_openmu); return 0; }
  char path[512];
  snprintf(path, sizeof path, "%s.%d.log", p, pid);
  FILE* f = fopen(path, "w");
  if (!f) { g_fx_on = 0; pthread_mutex_unlock(&g_fx_openmu); return 0; }
  setvbuf(f, NULL, _IOLBF, 0);
  g_fx_f  = f;
  g_fx_pid = pid;
  g_fx_on = 1;
  {                                           // name the PROCESS, not just its pid: three shim
    char** av = *_NSGetArgv();                // processes in a run and the roles are the point
    int    ac = *_NSGetArgc();
    fprintf(f, "# futex trace pid=%d ppid=%d t0=%.3f argv=", pid, (int)getppid(), fx_now());
    for (int i = 0; i < ac && av && av[i]; i++) fprintf(f, "%s ", av[i]);
    fprintf(f, "\n");
  }
  fx_death_hook();                            // say WHY the trace stops, not just that it did
  fx_watchdog_start();                        // fork keeps only the forking thread; restart it
  pthread_mutex_unlock(&g_fx_openmu);
  return 1;
}
#define FX(...) do { if (fx_on()) { pthread_mutex_lock(&g_fx_mu); \
                                    fprintf(g_fx_f, __VA_ARGS__); \
                                    pthread_mutex_unlock(&g_fx_mu); } } while (0)

// Callable from the other syscall TU (syscalls_ext.c). A process that dies never writes another
// futex line, so "the trace just stops" is ambiguous between hung and dead; these say which.
void fx_note(const char* fmt, ...);
void fx_note(const char* fmt, ...) {
  if (!fx_on()) return;
  va_list ap; va_start(ap, fmt);
  pthread_mutex_lock(&g_fx_mu);
  fprintf(g_fx_f, "%.3f ", fx_now());
  vfprintf(g_fx_f, fmt, ap);
  pthread_mutex_unlock(&g_fx_mu);
  va_end(ap);
}
double fx_stamp(void);
double fx_stamp(void) { return fx_now(); }

#define FU_BUCKETS 256                        // power of two; collisions only cost a re-check
struct fu_waiter {
  unsigned long uaddr;
  uint32_t bitset;                            // which FUTEX_WAKE_BITSET calls may wake us
  int woken;
  struct fu_waiter* next;
  int    fx_tid;                              // diagnostic: guest tid that parked here
  double fx_t0;                               // diagnostic: park time, ms
};
struct fu_bucket {
  pthread_mutex_t mu;
  pthread_cond_t  cv;
  struct fu_waiter* head;
};
static struct fu_bucket g_fu[FU_BUCKETS];
static pthread_once_t   g_fu_once = PTHREAD_ONCE_INIT;

static void fu_init(void) {
  for (int i = 0; i < FU_BUCKETS; i++) {
    pthread_mutex_init(&g_fu[i].mu, NULL);
    pthread_cond_init(&g_fu[i].cv, NULL);
    g_fu[i].head = NULL;
  }
}
// Futex words are 4-byte aligned, so the low bits carry no entropy; shift them out first.
static struct fu_bucket* fu_bucket(unsigned long uaddr) {
  pthread_once(&g_fu_once, fu_init);
  unsigned long h = (uaddr >> 2) * 0x9E3779B97F4A7C15UL;
  return &g_fu[(h >> 48) & (FU_BUCKETS - 1)];
}

static void ts_now(int lclock, struct timespec* t) {
  clock_gettime(lclock == LX_CLOCK_REALTIME ? CLOCK_REALTIME : CLOCK_MONOTONIC, t);
}

// Diagnostic watchdog: every 2s, name every waiter still parked and how long it has been there,
// with the CURRENT value of its futex word. A waiter whose word no longer equals the value it
// compared against is a waiter whose wake was lost -- that is the line we are looking for, and it
// is stated as the quantity itself (address + parked-for + word now), not as "something is stuck".
static void* fx_watchdog(void* _) {
  (void)_;
  for (;;) {
    struct timespec s = { 2, 0 };
    nanosleep(&s, NULL);
    double t = fx_now();
    for (int i = 0; i < FU_BUCKETS; i++) {
      if (pthread_mutex_trylock(&g_fu[i].mu) != 0) {
        FX("%.3f WD bucket=%d LOCKED (skipped)\n", t, i);
        continue;
      }
      for (struct fu_waiter* w = g_fu[i].head; w; w = w->next)
        FX("%.3f WD parked uaddr=%#lx bkt=%d tid=%d bits=%#x woken=%d for=%.1fms word_now=%u\n",
           t, w->uaddr, i, w->fx_tid, w->bitset, w->woken, t - w->fx_t0,
           *(volatile uint32_t*)w->uaddr);
      pthread_mutex_unlock(&g_fu[i].mu);
    }
  }
  return NULL;
}
static void fx_watchdog_start(void) {
  pthread_t th;
  if (pthread_create(&th, NULL, fx_watchdog, NULL) == 0) pthread_detach(th);
}

// Wake at most `n` waiters parked on exactly this address whose bitset intersects `bits`.
// Shared by FUTEX_WAKE, FUTEX_WAKE_BITSET and the two wakes FUTEX_WAKE_OP performs; the callers
// that have no bitset of their own pass FU_BITS_ANY.
#define FU_BITS_ANY 0xFFFFFFFFu
static int fu_wake(unsigned long uaddr, unsigned int n, uint32_t bits) {
  struct fu_bucket* b = fu_bucket(uaddr);
  pthread_mutex_lock(&b->mu);
  int woke = 0;
  int here = 0, mates = 0;                    // diagnostic: waiters on THIS addr / other addrs
  for (struct fu_waiter* w = b->head; w; w = w->next) { if (w->uaddr == uaddr) here++; else mates++; }
  for (struct fu_waiter* w = b->head; w && (unsigned)woke < n; w = w->next)
    if (w->uaddr == uaddr && !w->woken && (w->bitset & bits)) { w->woken = 1; woke++; }
  if (woke) pthread_cond_broadcast(&b->cv);
  pthread_mutex_unlock(&b->mu);
  FX("%.3f WAKE uaddr=%#lx bkt=%d n=%u bits=%#x -> woke=%d (waiters_here=%d other_addrs=%d)\n",
     fx_now(), uaddr, (int)(b - g_fu), n, bits, woke, here, mates);
  return woke;
}

// ---- priority-inheritance futexes ----------------------------------------------------------
// The guest's glibc uses these for pthread_mutex{PTHREAD_PRIO_INHERIT} and its PI condvars.
// PipeWire's data loop sets loop.prio-inherit=true, so pthread_mutex_init(attr with
// PTHREAD_PRIO_INHERIT) probes with FUTEX_LOCK_PI and returns ENOTSUP if it gets ENOSYS -- which
// is exactly what happened here and why PipeWire's data loop refused to start. Every guest thread
// is a host thread of one process, so real priority inheritance is meaningless; we model the PI
// word (owner TID in the low bits, waiters bit) and reuse the plain wait/wake queues, which is
// enough for the guest's own locking to be correct.
//
// The owner value MUST be guest_tid() (what gettid returns to the guest) so that the TID glibc
// writes into the mutex word matches the value our unlock path compares against.

// Acquire the PI futex at *p (blocking unless trylock). Returns 0 on success, -EDEADLK if the
// calling thread already owns it, -EAGAIN if trylock finds it held by someone else.
static long fu_lock_pi(volatile uint32_t* p, int trylock) {
  unsigned int tid = (unsigned int)guest_tid();
  for (;;) {
    uint32_t v = __atomic_load_n((uint32_t*)p, __ATOMIC_SEQ_CST);
    if ((v & FUTEX_TID_MASK) == 0) {
      if (__atomic_compare_exchange_n((uint32_t*)p, &v, tid, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 0;
      continue;
    }
    if ((v & FUTEX_TID_MASK) == tid)
      return -LX_EDEADLK;
    if (trylock)
      return -LX_EAGAIN;
    // Held by another thread: set the waiters bit, then park until the word changes.
    struct fu_bucket* b = fu_bucket((unsigned long)p);
    pthread_mutex_lock(&b->mu);
    uint32_t cur = *p;
    if ((cur & FUTEX_TID_MASK) == 0) { pthread_mutex_unlock(&b->mu); continue; }
    if ((cur & FUTEX_TID_MASK) == tid) { pthread_mutex_unlock(&b->mu); return -LX_EDEADLK; }
    if (!(cur & FUTEX_WAITERS))
      __atomic_compare_exchange_n((uint32_t*)p, &cur, cur | FUTEX_WAITERS, 0,
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    struct fu_waiter w = { (unsigned long)p, FU_BITS_ANY, 0, b->head, guest_tid(), fx_now() };
    b->head = &w;
    while (!w.woken) pthread_cond_wait(&b->cv, &b->mu);
    struct fu_waiter** pp = &b->head;
    while (*pp && *pp != &w) pp = &(*pp)->next;
    if (*pp) *pp = w.next;
    pthread_mutex_unlock(&b->mu);
  }
}

// Release the PI futex at *p if the calling thread owns it. Returns 0, or -EPERM if not owner.
static long fu_unlock_pi(volatile uint32_t* p) {
  unsigned int tid = (unsigned int)guest_tid();
  uint32_t v = __atomic_load_n((uint32_t*)p, __ATOMIC_SEQ_CST);
  if ((v & FUTEX_TID_MASK) != tid)
    return -LX_EPERM;
  if (__atomic_compare_exchange_n((uint32_t*)p, &v, 0u, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
    if (v & FUTEX_WAITERS)
      fu_wake((unsigned long)p, 1, FU_BITS_ANY);
  }
  return 0;
}

static long do_futex(unsigned long uaddr, int op, unsigned int val,
                     const struct linux_timespec* to, unsigned long uaddr2, unsigned int val3) {
  (void)uaddr2;
  int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
  volatile uint32_t* p = (volatile uint32_t*)uaddr;
  if (!p) return -LX_EFAULT;

  switch (cmd) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET:
    case FUTEX_WAIT_REQUEUE_PI: {
      // The bitset selects which FUTEX_WAKE_BITSET calls may wake us; plain FUTEX_WAIT takes part
      // in all of them. Linux rejects an empty bitset rather than parking a thread nothing can
      // ever reach. WAIT_REQUEUE_PI is a PI-condvar waiter; since CMP_REQUEUE_PI wakes rather than
      // requeues (below), it behaves exactly like a plain wait here.
      uint32_t bits = (cmd == FUTEX_WAIT_BITSET) ? val3 : FU_BITS_ANY;
      if (!bits) return -LX_EINVAL;

      // A futex timeout is a DEADLINE, not a per-park interval. Anchor it against the host
      // monotonic clock ONCE, then re-derive the time remaining on every loop iteration. The
      // previous code re-passed the same relative `rel` each time round the loop, so any
      // broadcast on the bucket -- including one aimed at a DIFFERENT address that merely hashes
      // here -- re-armed the full original interval. Under steady cross-traffic the timeout never
      // fired at all. See tests/wl/futexto.c.
      struct timespec deadline; int have_to = 0;
      if (to) {
        struct timespec rel;
        if (cmd == FUTEX_WAIT_BITSET || cmd == FUTEX_WAIT_REQUEUE_PI) {   // guest-absolute deadline
          struct timespec now;
          ts_now((op & FUTEX_CLOCK_REALTIME) ? LX_CLOCK_REALTIME : LX_CLOCK_MONOTONIC, &now);
          rel.tv_sec  = (time_t)(to->tv_sec  - now.tv_sec);
          rel.tv_nsec = (long)  (to->tv_nsec - now.tv_nsec);
        } else {                               // guest-relative interval
          rel.tv_sec = (time_t)to->tv_sec; rel.tv_nsec = (long)to->tv_nsec;
        }
        if (rel.tv_nsec < 0) { rel.tv_nsec += 1000000000L; rel.tv_sec--; }
        if (rel.tv_sec < 0) { rel.tv_sec = 0; rel.tv_nsec = 0; }
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec  += rel.tv_sec;
        deadline.tv_nsec += rel.tv_nsec;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_nsec -= 1000000000L; deadline.tv_sec++; }
        have_to = 1;
      }

      struct fu_bucket* b = fu_bucket(uaddr);
      pthread_mutex_lock(&b->mu);
      if (*p != val) {
        uint32_t seen = *p;
        pthread_mutex_unlock(&b->mu);
        FX("%.3f WAIT uaddr=%#lx bkt=%d tid=%d op=%#x val=%u word=%u bits=%#x to=%d -> EAGAIN\n",
           fx_now(), uaddr, (int)(b - g_fu), guest_tid(), op, val, seen, bits, have_to);
        return -LX_EAGAIN;
      }

      struct fu_waiter w = { uaddr, bits, 0, b->head, guest_tid(), fx_now() };
      b->head = &w;
      FX("%.3f WAIT uaddr=%#lx bkt=%d tid=%d op=%#x val=%u bits=%#x to=%d -> PARK\n",
         w.fx_t0, uaddr, (int)(b - g_fu), w.fx_tid, op, val, bits, have_to);

      int timedout = 0;
      while (!w.woken) {                        // loop: a bucket-mate's wake is not ours
        if (!have_to) { pthread_cond_wait(&b->cv, &b->mu); continue; }
        struct timespec now, rem;               // time LEFT, recomputed every pass
        clock_gettime(CLOCK_MONOTONIC, &now);
        rem.tv_sec  = deadline.tv_sec  - now.tv_sec;
        rem.tv_nsec = deadline.tv_nsec - now.tv_nsec;
        if (rem.tv_nsec < 0) { rem.tv_nsec += 1000000000L; rem.tv_sec--; }
        if (rem.tv_sec < 0) { timedout = 1; break; }          // deadline already behind us
        if (pthread_cond_timedwait_relative_np(&b->cv, &b->mu, &rem) == ETIMEDOUT) {
          timedout = 1; break;
        }
      }

      struct fu_waiter** pp = &b->head;         // unlink, still under the bucket lock
      while (*pp && *pp != &w) pp = &(*pp)->next;
      if (*pp) *pp = w.next;
      int woken = w.woken;
      pthread_mutex_unlock(&b->mu);

      FX("%.3f WAIT uaddr=%#lx bkt=%d tid=%d -> %s after %.1fms (word_now=%u)\n",
         fx_now(), uaddr, (int)(b - g_fu), w.fx_tid,
         woken ? "WOKEN" : (timedout ? "TIMEDOUT" : "SPURIOUS"),
         fx_now() - w.fx_t0, *p);

      if (timedout && !woken) return -LX_ETIMEDOUT;
      return 0;
    }
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET: {
      // Before this honoured the bitset, FUTEX_WAKE_BITSET(uaddr, val=1, bitset=X) could spend
      // its single wake on a waiter whose bitset does not intersect X and leave the intended
      // waiter parked -- a lost wakeup, i.e. a hang, of exactly the kind WAKE_OP was.
      uint32_t bits = (cmd == FUTEX_WAKE_BITSET) ? val3 : FU_BITS_ANY;
      if (!bits) return -LX_EINVAL;
      return fu_wake(uaddr, val, bits);
    }

    // FUTEX_WAKE_OP: atomically modify *uaddr2, wake up to `val` on uaddr, and if the OLD value
    // of *uaddr2 satisfies the encoded comparison, additionally wake up to `val2` on uaddr2.
    // Return the TOTAL woken.
    //
    // This was unimplemented and fell through to -ENOSYS, which is far worse than it sounds: a
    // WAKE that fails does not report an error to anything that matters -- it silently leaves the
    // waiter parked forever. That is a hang with no diagnostic, and it is what stopped Qt6 (found
    // by Q3: the last guest syscall of an 80s run was futex(op=0x85) -> -38, and then silence).
    // glibc/musl use WAKE_OP for pthread_cond_signal/broadcast paths, so anything with real
    // condvar traffic can hit it.
    //
    // `val2` arrives in the timeout argument slot as a plain integer, not a pointer -- that is
    // the kernel's calling convention for this op, not a bug here.
    case FUTEX_WAKE_OP: {
      unsigned int val2 = (unsigned int)(unsigned long)(uintptr_t)to;
      volatile uint32_t* p2 = (volatile uint32_t*)uaddr2;
      if (!p2) return -LX_EFAULT;

      int op2   = (int)((val3 >> 28) & 0x7);    // bit 31 is FUTEX_OP_OPARG_SHIFT, masked off here
      int shift = (val3 >> 28) & 0x8;
      int cmp   = (int)((val3 >> 24) & 0xf);
      int oparg = (int)((val3 >> 12) & 0xfff);
      int cmparg= (int)(val3 & 0xfff);
      if (shift) oparg = 1 << (oparg & 31);

      uint32_t oldval, newval;
      do {                                      // atomic read-modify-write of *uaddr2
        oldval = *p2;
        switch (op2) {
          case 0: newval = (uint32_t)oparg; break;              // SET
          case 1: newval = oldval + (uint32_t)oparg; break;     // ADD
          case 2: newval = oldval | (uint32_t)oparg; break;     // OR
          case 3: newval = oldval & ~(uint32_t)oparg; break;    // ANDN
          case 4: newval = oldval ^ (uint32_t)oparg; break;     // XOR
          default: return -LX_ENOSYS;
        }
      } while (!__atomic_compare_exchange_n((uint32_t*)p2, &oldval, newval, 0,
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));

      int woke = fu_wake(uaddr, val, FU_BITS_ANY);

      int hit;
      switch (cmp) {
        case 0: hit = ((int)oldval == cmparg); break;   // EQ
        case 1: hit = ((int)oldval != cmparg); break;   // NE
        case 2: hit = ((int)oldval <  cmparg); break;   // LT
        case 3: hit = ((int)oldval <= cmparg); break;   // LE
        case 4: hit = ((int)oldval >  cmparg); break;   // GT
        case 5: hit = ((int)oldval >= cmparg); break;   // GE
        default: return -LX_ENOSYS;
      }
      if (hit) woke += fu_wake(uaddr2, val2, FU_BITS_ANY);
      return woke;
    }
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE: {
      // Still not a real requeue: wake everything on uaddr and let the guest re-contend on
      // uaddr2. `val` (the number the caller wanted woken rather than moved) is deliberately
      // ignored, and that direction is the safe one -- waking MORE than asked is legal, because
      // FUTEX_WAIT may return spuriously and the guest re-checks its predicate, whereas leaving
      // a waiter parked on uaddr while the guest believes it now waits on uaddr2 would strand it
      // with nothing that can ever reach it.
      struct fu_bucket* b = fu_bucket(uaddr);
      pthread_mutex_lock(&b->mu);
      // CMP_REQUEUE exists to detect that the value changed under the caller. Never returning
      // EAGAIN told a guest that lost the race that it had won; the kernel compares here, with
      // the queue locked, and so do we.
      if (cmd == FUTEX_CMP_REQUEUE && *p != val3) {
        pthread_mutex_unlock(&b->mu);
        return -LX_EAGAIN;
      }
      int n = 0;
      for (struct fu_waiter* w = b->head; w; w = w->next)
        if (w->uaddr == uaddr && !w->woken) { w->woken = 1; n++; }
      if (n) pthread_cond_broadcast(&b->cv);
      pthread_mutex_unlock(&b->mu);
      return n;
    }
    case FUTEX_LOCK_PI:
      return fu_lock_pi(p, 0);
    case FUTEX_TRYLOCK_PI:
      return fu_lock_pi(p, 1);
    case FUTEX_UNLOCK_PI:
      return fu_unlock_pi(p);
    case FUTEX_CMP_REQUEUE_PI:
      // PI condvar signal/broadcast: the kernel moves waiters onto the mutex's PI wait queue.
      // Waking them all here (as with plain CMP_REQUEUE) is the safe direction: a waiter that was
      // merely signalled re-acquires the mutex through FUTEX_LOCK_PI and a "requeued" waiter is
      // treated identically, so nothing is stranded and no predicate is left unchecked.
      if (*p != val3) return -LX_EAGAIN;
      return fu_wake(uaddr, 0x7fffffff, FU_BITS_ANY);
    default:
      FX("%.3f UNSUPPORTED op=%#x cmd=%d uaddr=%#lx val=%u val3=%#x tid=%d -> ENOSYS\n",
         fx_now(), op, cmd, uaddr, val, val3, guest_tid());
      return -LX_ENOSYS;
  }
}

// ===========================================================================================
// signals (recorded, never installed — the shim does not deliver signals to guest code)
// ===========================================================================================

static struct linux_sigaction g_sigacts[65];
static uint64_t g_sigmask;

// ===========================================================================================
// ioctl
// ===========================================================================================

#define L_TCGETS     0x5401
#define L_TCSETS     0x5402
#define L_TCSETSW    0x5403
#define L_TCSETSF    0x5404
#define L_TCGETS2    0x802c542a
#define L_TCSETS2    0x402c542b
#define L_TCSETSW2   0x402c542c
#define L_TCSETSF2   0x402c542d
#define L_TCSBRK     0x5409
#define L_TCXONC     0x540A
#define L_TCFLSH     0x540B
#define L_TIOCEXCL   0x540C
#define L_TIOCNXCL   0x540D
#define L_TIOCSCTTY  0x540E
#define L_TIOCGPGRP  0x540F
#define L_TIOCSPGRP  0x5410
#define L_TIOCOUTQ   0x5411
#define L_TIOCGWINSZ 0x5413
#define L_TIOCSWINSZ 0x5414
#define L_TIOCGPTN 0x80045430
#define L_TIOCSPTLCK 0x40045431
#define L_FIONREAD   0x541B
#define L_FIONBIO    0x5421
#define L_TIOCNOTTY  0x5422
#define L_TIOCGSID   0x5429
#define L_FIONCLEX   0x5450
#define L_FIOCLEX    0x5451

// Linux termios bit values (they differ from macOS for IXON/IXOFF, ONLCR and IEXTEN).
#define LI_IGNBRK 0x0001
#define LI_BRKINT 0x0002
#define LI_IGNPAR 0x0004
#define LI_PARMRK 0x0008
#define LI_INPCK  0x0010
#define LI_ISTRIP 0x0020
#define LI_INLCR  0x0040
#define LI_IGNCR  0x0080
#define LI_ICRNL  0x0100
#define LI_IXON   0x0400
#define LI_IXANY  0x0800
#define LI_IXOFF  0x1000
#define LO_OPOST  0x0001
#define LO_ONLCR  0x0004
#define LC_CS8    0x0030
#define LC_CREAD  0x0080
#define LC_PARENB 0x0100
#define LC_HUPCL  0x0400
#define LC_CLOCAL 0x0800
#define LL_ISIG   0x0001
#define LL_ICANON 0x0002
#define LL_ECHO   0x0008
#define LL_ECHOE  0x0010
#define LL_ECHOK  0x0020
#define LL_ECHONL 0x0040
#define LL_NOFLSH 0x0080
#define LL_TOSTOP 0x0100
#define LL_IEXTEN 0x8000

static void termios_m2l(const struct termios* m, struct linux_termios* l) {
  memset(l, 0, sizeof *l);
  uint32_t i = 0;
  if (m->c_iflag & IGNBRK) i |= LI_IGNBRK;
  if (m->c_iflag & BRKINT) i |= LI_BRKINT;
  if (m->c_iflag & IGNPAR) i |= LI_IGNPAR;
  if (m->c_iflag & PARMRK) i |= LI_PARMRK;
  if (m->c_iflag & INPCK)  i |= LI_INPCK;
  if (m->c_iflag & ISTRIP) i |= LI_ISTRIP;
  if (m->c_iflag & INLCR)  i |= LI_INLCR;
  if (m->c_iflag & IGNCR)  i |= LI_IGNCR;
  if (m->c_iflag & ICRNL)  i |= LI_ICRNL;
  if (m->c_iflag & IXON)   i |= LI_IXON;
  if (m->c_iflag & IXANY)  i |= LI_IXANY;
  if (m->c_iflag & IXOFF)  i |= LI_IXOFF;
  l->c_iflag = i;

  uint32_t o = 0;
  if (m->c_oflag & OPOST)  o |= LO_OPOST;
  if (m->c_oflag & ONLCR)  o |= LO_ONLCR;
  l->c_oflag = o;

  uint32_t c = LC_CS8 | 0x0F /* B38400 */;
  if (m->c_cflag & CREAD)  c |= LC_CREAD;
  if (m->c_cflag & PARENB) c |= LC_PARENB;
  if (m->c_cflag & HUPCL)  c |= LC_HUPCL;
  if (m->c_cflag & CLOCAL) c |= LC_CLOCAL;
  l->c_cflag = c;

  uint32_t f = 0;
  if (m->c_lflag & ISIG)   f |= LL_ISIG;
  if (m->c_lflag & ICANON) f |= LL_ICANON;
  if (m->c_lflag & ECHO)   f |= LL_ECHO;
  if (m->c_lflag & ECHOE)  f |= LL_ECHOE;
  if (m->c_lflag & ECHOK)  f |= LL_ECHOK;
  if (m->c_lflag & ECHONL) f |= LL_ECHONL;
  if (m->c_lflag & NOFLSH) f |= LL_NOFLSH;
  if (m->c_lflag & TOSTOP) f |= LL_TOSTOP;
  if (m->c_lflag & IEXTEN) f |= LL_IEXTEN;
  l->c_lflag = f;

  l->c_cc[0]  = m->c_cc[VINTR];    l->c_cc[1]  = m->c_cc[VQUIT];
  l->c_cc[2]  = m->c_cc[VERASE];   l->c_cc[3]  = m->c_cc[VKILL];
  l->c_cc[4]  = m->c_cc[VEOF];     l->c_cc[5]  = m->c_cc[VTIME];
  l->c_cc[6]  = m->c_cc[VMIN];     l->c_cc[8]  = m->c_cc[VSTART];
  l->c_cc[9]  = m->c_cc[VSTOP];    l->c_cc[10] = m->c_cc[VSUSP];
  l->c_cc[11] = m->c_cc[VEOL];     l->c_cc[12] = m->c_cc[VREPRINT];
  l->c_cc[13] = m->c_cc[VDISCARD]; l->c_cc[14] = m->c_cc[VWERASE];
  l->c_cc[15] = m->c_cc[VLNEXT];   l->c_cc[16] = m->c_cc[VEOL2];
}

static void termios_l2m(const struct linux_termios* l, struct termios* m) {
  memset(m, 0, sizeof *m);
  tcflag_t i = 0;
  if (l->c_iflag & LI_IGNBRK) i |= IGNBRK;
  if (l->c_iflag & LI_BRKINT) i |= BRKINT;
  if (l->c_iflag & LI_IGNPAR) i |= IGNPAR;
  if (l->c_iflag & LI_PARMRK) i |= PARMRK;
  if (l->c_iflag & LI_INPCK)  i |= INPCK;
  if (l->c_iflag & LI_ISTRIP) i |= ISTRIP;
  if (l->c_iflag & LI_INLCR)  i |= INLCR;
  if (l->c_iflag & LI_IGNCR)  i |= IGNCR;
  if (l->c_iflag & LI_ICRNL)  i |= ICRNL;
  if (l->c_iflag & LI_IXON)   i |= IXON;
  if (l->c_iflag & LI_IXANY)  i |= IXANY;
  if (l->c_iflag & LI_IXOFF)  i |= IXOFF;
  m->c_iflag = i;

  tcflag_t o = 0;
  if (l->c_oflag & LO_OPOST) o |= OPOST;
  if (l->c_oflag & LO_ONLCR) o |= ONLCR;
  m->c_oflag = o;

  tcflag_t c = CS8;
  if (l->c_cflag & LC_CREAD)  c |= CREAD;
  if (l->c_cflag & LC_PARENB) c |= PARENB;
  if (l->c_cflag & LC_HUPCL)  c |= HUPCL;
  if (l->c_cflag & LC_CLOCAL) c |= CLOCAL;
  m->c_cflag = c;

  tcflag_t f = 0;
  if (l->c_lflag & LL_ISIG)   f |= ISIG;
  if (l->c_lflag & LL_ICANON) f |= ICANON;
  if (l->c_lflag & LL_ECHO)   f |= ECHO;
  if (l->c_lflag & LL_ECHOE)  f |= ECHOE;
  if (l->c_lflag & LL_ECHOK)  f |= ECHOK;
  if (l->c_lflag & LL_ECHONL) f |= ECHONL;
  if (l->c_lflag & LL_NOFLSH) f |= NOFLSH;
  if (l->c_lflag & LL_TOSTOP) f |= TOSTOP;
  if (l->c_lflag & LL_IEXTEN) f |= IEXTEN;
  m->c_lflag = f;

  m->c_cc[VINTR]  = l->c_cc[0];  m->c_cc[VQUIT]    = l->c_cc[1];
  m->c_cc[VERASE] = l->c_cc[2];  m->c_cc[VKILL]    = l->c_cc[3];
  m->c_cc[VEOF]   = l->c_cc[4];  m->c_cc[VTIME]    = l->c_cc[5];
  m->c_cc[VMIN]   = l->c_cc[6];  m->c_cc[VSTART]   = l->c_cc[8];
  m->c_cc[VSTOP]  = l->c_cc[9];  m->c_cc[VSUSP]    = l->c_cc[10];
  m->c_cc[VEOL]   = l->c_cc[11]; m->c_cc[VREPRINT] = l->c_cc[12];
  m->c_cc[VDISCARD] = l->c_cc[13]; m->c_cc[VWERASE] = l->c_cc[14];
  m->c_cc[VLNEXT] = l->c_cc[15]; m->c_cc[VEOL2]    = l->c_cc[16];
  cfsetispeed(m, B38400); cfsetospeed(m, B38400);
}

// termios2 differs from termios only by the split c_ispeed/c_ospeed words; the flag/cc layout is
// identical. Reuse the termios conversion and just carry the speeds (fixed at B38400, as above).
static void termios2_m2l(const struct termios* m, struct linux_termios2* l) {
  struct linux_termios base;
  termios_m2l(m, &base);
  memset(l, 0, sizeof *l);
  l->c_iflag = base.c_iflag; l->c_oflag = base.c_oflag;
  l->c_cflag = base.c_cflag; l->c_lflag = base.c_lflag;
  l->c_line  = base.c_line;
  memcpy(l->c_cc, base.c_cc, sizeof base.c_cc);
  l->c_ispeed = 0x0F /* B38400 */; l->c_ospeed = 0x0F;
}
static void termios2_l2m(const struct linux_termios2* l, struct termios* m) {
  struct linux_termios base;
  base.c_iflag = l->c_iflag; base.c_oflag = l->c_oflag;
  base.c_cflag = l->c_cflag; base.c_lflag = l->c_lflag;
  base.c_line  = l->c_line;
  memcpy(base.c_cc, l->c_cc, sizeof base.c_cc);
  termios_l2m(&base, m);
}

// tcgetattr/tcsetattr/tcdrain/tcflow/tcflush on Darwin all fail on a pty MASTER with ENOTTY —
// Darwin serves termios only on the SLAVE. Linux tooling (foot) routinely calls tcgetattr on the
// master, so redirect those operations to the slave, exactly as TIOCGWINSZ below already does.
// Returns the fd to operate on; *closed_out is set when that fd must be closed after the call.
static int pty_slave_redirect(int fd, int* closed_out) {
  *closed_out = 0;
  int p = pty_index(fd);
  if (getenv("RDPTYTRACE")) fprintf(stderr, "[pty] redirect fd=%d pty_index=%d\n", fd, p);
  if (p < 0) return fd;
  int s = open(g_guest_pty[p].slave, O_RDWR | O_NOCTTY);
  if (s < 0) { if (getenv("RDPTYTRACE")) fprintf(stderr, "[pty] open slave %s failed %s\n",
                                                 g_guest_pty[p].slave, strerror(errno)); return fd; }
  *closed_out = 1;
  return s;
}

static long do_ioctl(int fd, unsigned long req, unsigned long arg) {
  // Linux ioctl command numbers are 32-bit; guest libc may sign-extend them.
  req &= 0xffffffffUL;
  switch (req) {
    case L_TCGETS: {
      struct termios t;
      int closed = 0; int tgt = pty_slave_redirect(fd, &closed);
      long r = tcgetattr(tgt, &t) < 0 ? herrno() : 0;
      if (closed) close(tgt);
      if (r != 0) return r;
      termios_m2l(&t, (struct linux_termios*)arg);
      return 0;
    }
    case L_TCSETS: case L_TCSETSW: case L_TCSETSF: {
      struct termios t;
      termios_l2m((const struct linux_termios*)arg, &t);
      int act = req == L_TCSETS ? TCSANOW : (req == L_TCSETSW ? TCSADRAIN : TCSAFLUSH);
      int closed = 0; int tgt = pty_slave_redirect(fd, &closed);
      long r = herr(tcsetattr(tgt, act, &t));
      if (closed) close(tgt);
      return r;
    }
    case L_TCGETS2: {
      struct termios t;
      int closed = 0; int tgt = pty_slave_redirect(fd, &closed);
      long r = tcgetattr(tgt, &t) < 0 ? herrno() : 0;
      if (closed) close(tgt);
      if (r != 0) return r;
      termios2_m2l(&t, (struct linux_termios2*)arg);
      return 0;
    }
    case L_TCSETS2: case L_TCSETSW2: case L_TCSETSF2: {
      struct termios t;
      termios2_l2m((const struct linux_termios2*)arg, &t);
      int act = req == L_TCSETS2 ? TCSANOW : (req == L_TCSETSW2 ? TCSADRAIN : TCSAFLUSH);
      int closed = 0; int tgt = pty_slave_redirect(fd, &closed);
      long r = herr(tcsetattr(tgt, act, &t));
      if (closed) close(tgt);
      return r;
    }
    case L_TCSBRK:  { int closed = 0; int tgt = pty_slave_redirect(fd, &closed);
      long r = herr(tcdrain(tgt)); if (closed) close(tgt); return r; }
    case L_TCXONC:  { int closed = 0; int tgt = pty_slave_redirect(fd, &closed);
      long r = herr(tcflow(tgt, (int)arg == 0 ? TCOOFF : ((int)arg == 1 ? TCOON :
                                          ((int)arg == 2 ? TCIOFF : TCION))));
      if (closed) close(tgt); return r; }
    case L_TCFLSH:  { int closed = 0; int tgt = pty_slave_redirect(fd, &closed);
      long r = herr(tcflush(tgt, (int)arg == 0 ? TCIFLUSH :
                                          ((int)arg == 1 ? TCOFLUSH : TCIOFLUSH)));
      if (closed) close(tgt); return r; }
    case L_TIOCGWINSZ: {
      struct winsize w;
      int p=pty_index(fd), s=p>=0?open(g_guest_pty[p].slave,O_RDWR|O_NOCTTY):-1;
      int target=s>=0?s:fd;
      int result=ioctl(target, TIOCGWINSZ, &w);
      if(s>=0)close(s);
      if (result < 0) return herrno();
      struct linux_winsize* lw = (struct linux_winsize*)arg;
      lw->ws_row = w.ws_row; lw->ws_col = w.ws_col;
      lw->ws_xpixel = w.ws_xpixel; lw->ws_ypixel = w.ws_ypixel;
      return 0;
    }
    case L_TIOCSWINSZ: {
      const struct linux_winsize* lw = (const struct linux_winsize*)arg;
      struct winsize w = { lw->ws_row, lw->ws_col, lw->ws_xpixel, lw->ws_ypixel };
      int p=pty_index(fd), s=p>=0?open(g_guest_pty[p].slave,O_RDWR|O_NOCTTY):-1;
      int target=s>=0?s:fd;
      int result=ioctl(target,TIOCSWINSZ,&w);
      if(s>=0)close(s);
      return herr(result);
    }
    case L_TIOCGPTN: { int p=pty_index(fd); if(p<0)return -LX_ENOTTY; *(int*)arg=p; return 0; }
    case L_TIOCSPTLCK: { int p=pty_index(fd); if(p<0)return -LX_ENOTTY;
                           if (*(int*)arg) return -95; // EOPNOTSUPP: Darwin has no relock ioctl
                           if (ioctl(fd,TIOCPTYUNLK)<0)return herrno();
                           g_guest_pty[p].unlocked=1; return 0; }
    case L_FIONREAD:  { int n = 0; if (ioctl(fd, FIONREAD, &n) < 0) return herrno();
                        *(int*)arg = n; return 0; }
    case L_FIONBIO:   { int on = *(const int*)arg; return herr(ioctl(fd, FIONBIO, &on)); }
    case L_FIOCLEX:   return herr(fcntl(fd, F_SETFD, FD_CLOEXEC));
    case L_FIONCLEX:  return herr(fcntl(fd, F_SETFD, 0));
    case L_TIOCGPGRP: { pid_t p = tcgetpgrp(fd); if (p < 0) return herrno();
                        *(int*)arg = (int)p; return 0; }
    case L_TIOCSPGRP: return herr(tcsetpgrp(fd, (pid_t)*(const int*)arg));
    case L_TIOCGSID:  { pid_t s = getsid(0); if (s < 0) return herrno();
                        *(int*)arg = (int)s; return 0; }
    case L_TIOCEXCL:  return herr(ioctl(fd, TIOCEXCL));
    case L_TIOCNXCL:  return herr(ioctl(fd, TIOCNXCL));
    case L_TIOCSCTTY: return herr(ioctl(fd,TIOCSCTTY,0));
    case L_TIOCNOTTY: return herr(ioctl(fd, TIOCNOTTY));
    case L_TIOCOUTQ:  { int n = 0; if (ioctl(fd, TIOCOUTQ, &n) < 0) return herrno();
                        *(int*)arg = n; return 0; }
    default:
      if (snd_owns_fd(fd)) return snd_ioctl(fd, req, arg);
      if (drm_owns_fd(fd)) return drm_ioctl(fd, req, arg);
      if (cam_owns_fd(fd)) return cam_ioctl(fd, req, arg);
      return -LX_ENOTTY;
  }
}

// ===========================================================================================
// fcntl
// ===========================================================================================

#define LF_DUPFD          0
#define LF_GETFD          1
#define LF_SETFD          2
#define LF_GETFL          3
#define LF_SETFL          4
#define LF_GETLK          5
#define LF_SETLK          6
#define LF_SETLKW         7
#define LF_SETOWN         8
#define LF_GETOWN         9
#define LF_DUPFD_CLOEXEC  1030
// File SEALING. Linux-only as an API, and WebKitGTK's SharedMemory depends on it: it creates a
// memfd with MFD_ALLOW_SEALING, ftruncates it, then seals it — and treats a failed seal as fatal.
// We were returning EINVAL from do_fcntl's default, so the WebProcess called abort() with no
// output whatsoever (a release-build RELEASE_ASSERT prints nothing), which read as "the auxiliary
// process vanished". MEASURED: nr=25(0x9,0x409,0x3) -> -22.
//
// Darwin has no sealing, but the SECURITY property seals provide — stopping another process
// resizing or rewriting shared memory behind your back — is about a mutually-untrusting pair, and
// both ends here are the same guest under one shim. So we RECORD the seals, REPORT them back
// truthfully from F_GET_SEALS, and ENFORCE the two we actually can (SHRINK/GROW block ftruncate).
// That is deliberately NOT the "say yes and do nothing" pattern that caused two of today's bugs:
// a caller that sets a seal and then reads it back sees the truth, and a caller that tries the
// resize a seal forbids is refused.
#define LF_ADD_SEALS      1033
#define LF_GET_SEALS      1034
#define L_SEAL_SEAL       0x0001
#define L_SEAL_SHRINK     0x0002
#define L_SEAL_GROW       0x0004
#define L_SEAL_WRITE      0x0008

// Small open-addressed table keyed by fd. Sealing is rare (a handful of fds per process), so a
// flat array indexed by fd is both simplest and fastest.
#define SEAL_MAX_FD 4096
static unsigned g_seals[SEAL_MAX_FD];
static unsigned seals_get(int fd) { return (fd >= 0 && fd < SEAL_MAX_FD) ? g_seals[fd] : 0; }
static void     seals_set(int fd, unsigned v) { if (fd >= 0 && fd < SEAL_MAX_FD) g_seals[fd] = v; }

static int flock_type_l2m(int t) { return t == 0 ? F_RDLCK : (t == 1 ? F_WRLCK : F_UNLCK); }
static int flock_type_m2l(int t) { return t == F_RDLCK ? 0 : (t == F_WRLCK ? 1 : 2); }

static long do_fcntl(int fd, int cmd, unsigned long arg) {
  switch (cmd) {
    case LF_DUPFD: case LF_DUPFD_CLOEXEC: {
      int nf = fcntl(fd, cmd == LF_DUPFD ? F_DUPFD : F_DUPFD_CLOEXEC, (int)arg);
      if (nf >= 0 && drm_owns_fd(fd)) drm_register_fd(nf);   // a dup is still the render node
      if (nf >= 0 && snd_owns_fd(fd)) snd_dup_fd(fd, nf);    // a dup of a snd node keeps its role
      return herr(nf);
    }
    case LF_GETFD:         return herr(fcntl(fd, F_GETFD));
    case LF_SETFD:         return herr(fcntl(fd, F_SETFD, (int)arg));
    case LF_GETFL: {
      int r = fcntl(fd, F_GETFL);
      return r < 0 ? herrno() : open_flags_m2l(r);
    }
    case LF_SETFL: {
      int mf = 0;
      if (arg & LO_NONBLOCK) mf |= O_NONBLOCK;
      if (arg & LO_APPEND)   mf |= O_APPEND;
      if (arg & LO_ASYNC)    mf |= O_ASYNC;
      return herr(fcntl(fd, F_SETFL, mf));
    }
    case LF_GETLK: case LF_SETLK: case LF_SETLKW: {
      struct linux_flock* lf = (struct linux_flock*)arg;
      if (!lf) return -LX_EFAULT;
      struct flock mf;
      memset(&mf, 0, sizeof mf);
      mf.l_start  = lf->l_start;
      mf.l_len    = lf->l_len;
      mf.l_pid    = lf->l_pid;
      mf.l_type   = (short)flock_type_l2m(lf->l_type);
      mf.l_whence = lf->l_whence;
      int mcmd = cmd == LF_GETLK ? F_GETLK : (cmd == LF_SETLK ? F_SETLK : F_SETLKW);
      if (fcntl(fd, mcmd, &mf) < 0) return herrno();
      if (cmd == LF_GETLK) {
        lf->l_type   = (int16_t)flock_type_m2l(mf.l_type);
        lf->l_whence = (int16_t)mf.l_whence;
        lf->l_start  = mf.l_start;
        lf->l_len    = mf.l_len;
        lf->l_pid    = mf.l_pid;
      }
      return 0;
    }
    case LF_GET_SEALS: return (long)seals_get(fd);
    case LF_ADD_SEALS: {
      unsigned cur = seals_get(fd);
      if (cur & L_SEAL_SEAL) return -LX_EPERM;      // F_SEAL_SEAL forbids adding more; honoured.
      seals_set(fd, cur | (unsigned)arg);
      return 0;
    }
    case LF_SETOWN: return herr(fcntl(fd, F_SETOWN, (int)arg));
    case LF_GETOWN: return herr(fcntl(fd, F_GETOWN));
    default:        return -LX_EINVAL;
  }
}

// ===========================================================================================
// misc helpers
// ===========================================================================================

static int clockid_l2m(int lc) {
  switch (lc) {
    case LX_CLOCK_REALTIME:
    case LX_CLOCK_REALTIME_COARSE:    return CLOCK_REALTIME;
    case LX_CLOCK_MONOTONIC:
    case LX_CLOCK_MONOTONIC_COARSE:
    case LX_CLOCK_MONOTONIC_RAW:      return CLOCK_UPTIME_RAW;
    case LX_CLOCK_BOOTTIME:           return CLOCK_MONOTONIC_RAW;
    case LX_CLOCK_PROCESS_CPUTIME_ID: return CLOCK_PROCESS_CPUTIME_ID;
    case LX_CLOCK_THREAD_CPUTIME_ID:  return CLOCK_THREAD_CPUTIME_ID;
    default:                          return -1;
  }
}

static int lseek_whence_l2m(int w) {
  switch (w) {
    case 0: return SEEK_SET;
    case 1: return SEEK_CUR;
    case 2: return SEEK_END;
    case 3: return SEEK_DATA;   // Linux SEEK_DATA=3 / SEEK_HOLE=4; macOS has them SWAPPED
    case 4: return SEEK_HOLE;
    default: return -1;
  }
}

// ===========================================================================================
// getdents64
// ===========================================================================================

static long do_getdents64(int fd, unsigned char* buf, size_t cap) {
  if (!buf) return -LX_EFAULT;
  DIR* d = dirmap_open(fd);
  if (!d) return herrno();
  size_t used = 0;
  for (;;) {
    long pos = telldir(d);
    errno = 0;
    struct dirent* e = readdir(d);
    if (!e) {
      if (errno) return herrno();
      break;                                   // end of directory
    }
    size_t nl = strlen(e->d_name);
    size_t rec = (19 + nl + 1 + 7) & ~(size_t)7;   // 8-align, room for the NUL
    if (used + rec > cap) {
      seekdir(d, pos);                         // put it back for the next call
      if (used == 0) return -LX_EINVAL;        // buffer too small for even one entry
      break;
    }
    unsigned char* p = buf + used;
    memset(p, 0, rec);
    uint64_t ino = (uint64_t)e->d_ino;
    int64_t  off = (int64_t)telldir(d);        // offset of the NEXT entry, like Linux
    uint16_t rl  = (uint16_t)rec;
    uint8_t  ty  = (uint8_t)e->d_type;         // DT_* values are identical on both systems
    memcpy(p +  0, &ino, 8);
    memcpy(p +  8, &off, 8);
    memcpy(p + 16, &rl,  2);
    memcpy(p + 18, &ty,  1);
    memcpy(p + 19, e->d_name, nl + 1);
    used += rec;
  }
  return (long)used;
}

// ===========================================================================================
// dispatch
// ===========================================================================================

long linux_syscall(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
                   unsigned long a3, unsigned long a4, unsigned long a5) {
  char host[PATH_MAX], gabs[GPATH_MAX];
  long r;

  switch (nr) {

  // ---------- 17 getcwd ----------
  case 17: {
    char* buf = (char*)a0; size_t sz = (size_t)a1;
    if (!buf) return -LX_EFAULT;
    size_t n = strlen(g_cwd);
    if (sz < n + 1) return -LX_ERANGE;
    memcpy(buf, g_cwd, n + 1);
    return (long)(n + 1);                       // Linux returns the length INCLUDING the NUL
  }

  // ---------- 25 fcntl ----------
  case 25: return do_fcntl((int)a0, (int)a1, a2);

  // ---------- 29 ioctl ----------
  case 29:
    if (getenv("RDPTYTRACE"))
      fprintf(stderr, "[pty] ioctl fd=%d req=0x%lx arg=%#lx\n", (int)a0, a1, a2);
    return do_ioctl((int)a0, a1, a2);

  // ---------- 0x2002 rdmetal GPU guest syscall ----------
#ifdef RDGPU_CLIENT
  case 0x2002: return rdgpu_mock_gate(a0, a1, a2, a3, a4, a5);
#endif

  // ---------- 32 flock ----------  (LOCK_SH/EX/NB/UN values match)
  case 32: return herr(flock((int)a0, (int)a1));

  // ---------- 34 mkdirat ----------
  case 34:
    if ((r = resolve_f((int)a0, (const char*)a1, RSV_NOFOLLOW, host, sizeof host, 0, 0)) < 0)
      return r;
    return herr(mkdir(host, (mode_t)a2));

  // ---------- 35 unlinkat ----------
  case 35: {
    if ((r = resolve_f((int)a0, (const char*)a1, RSV_NOFOLLOW, host, sizeof host, 0, 0)) < 0)
      return r;
    if (a2 & LX_AT_REMOVEDIR) return herr(rmdir(host));
    if (unlink(host) == 0) return 0;
    // macOS reports EPERM for unlink(dir); Linux reports EISDIR.
    if (errno == EPERM) {
      struct stat st;
      if (lstat(host, &st) == 0 && S_ISDIR(st.st_mode)) return -LX_EISDIR;
      errno = EPERM;
    }
    return herrno();
  }

  // ---------- 37 linkat ----------
  case 37: {
    char host2[PATH_MAX];
    int oldnf = (a4 & LX_AT_SYMLINK_FOLLOW) ? 0 : RSV_NOFOLLOW;
    if ((r = resolve_f((int)a0, (const char*)a1, oldnf, host, sizeof host, 0, 0)) < 0) return r;
    if ((r = resolve_f((int)a2, (const char*)a3, RSV_NOFOLLOW, host2, sizeof host2, 0, 0)) < 0)
      return r;
    return herr(link(host, host2));
  }

  // ---------- 38 renameat ----------
  case 38: {
    char host2[PATH_MAX];
    if ((r = resolve_f((int)a0, (const char*)a1, RSV_NOFOLLOW, host, sizeof host, 0, 0)) < 0)
      return r;
    if ((r = resolve_f((int)a2, (const char*)a3, RSV_NOFOLLOW, host2, sizeof host2, 0, 0)) < 0)
      return r;
    return herr(rename(host, host2));
  }

  // ---------- 43 statfs / 44 fstatfs ----------
  case 43: {
    struct statfs s;
    if ((r = resolve(LX_AT_FDCWD, (const char*)a0, host, sizeof host, 0, 0)) < 0) return r;
    if (statfs(host, &s) < 0) return herrno();
    if (!a1) return -LX_EFAULT;
    statfs_m2l(&s, (struct linux_statfs*)a1);
    return 0;
  }
  case 44: {
    struct statfs s;
    if (fstatfs((int)a0, &s) < 0) return herrno();
    if (!a1) return -LX_EFAULT;
    statfs_m2l(&s, (struct linux_statfs*)a1);
    return 0;
  }

  // ---------- 46 ftruncate ----------
  case 46: {                                          // ftruncate
    // Enforce the seals we can. This is what makes F_ADD_SEALS an implementation rather than a
    // promise: a guest that seals against resize and then tries to resize is refused, exactly as
    // Linux would refuse it.
    unsigned sl = seals_get((int)a0);
    if (sl & (L_SEAL_SHRINK | L_SEAL_GROW)) {
      struct stat st;
      if (fstat((int)a0, &st) == 0) {
        if ((sl & L_SEAL_SHRINK) && (off_t)a1 < st.st_size) return -LX_EPERM;
        if ((sl & L_SEAL_GROW)   && (off_t)a1 > st.st_size) return -LX_EPERM;
      }
    }
    return herr(ftruncate((int)a0, (off_t)a1));
  }

  // ---------- 48 faccessat ----------
  case 48: {
    if ((r = resolve((int)a0, (const char*)a1, host, sizeof host, gabs, sizeof gabs)) < 0)
      return r;
    if (strcmp(gabs, "/proc/self/exe") == 0 || strcmp(gabs, "/proc/self/maps") == 0 ||
        strcmp(gabs, "/proc/self/cmdline") == 0 || strcmp(gabs, "/proc/self/stat") == 0 ||
        strcmp(gabs, "/proc/cpuinfo") == 0 || strcmp(gabs, "/proc/meminfo") == 0)
      return (a2 & 2) ? -LX_EACCES : 0;         // readable/executable, never writable
    // NB: syscall 48 takes only (dfd, path, mode) — the kernel ignores any 4th register, and
    // so must we: musl's 3-argument wrapper leaves x3 holding junk.  AT_EACCESS is faccessat2.
    { static int on = -1;
      if (on < 0) on = getenv("RDPATHS") != 0;
      if (on) {
        int rc = faccessat(AT_FDCWD, host, (int)a2, 0);
        fprintf(stderr, "[path] tid=%d access %s -> %s\n", guest_tid(), gabs,
                rc == 0 ? "ok" : strerror(errno));
        return herr(rc);
      } }
    return herr(faccessat(AT_FDCWD, host, (int)a2, 0));
  }

  // ---------- 49 chdir ----------
  case 49: {
    if ((r = resolve(LX_AT_FDCWD, (const char*)a0, host, sizeof host, gabs, sizeof gabs)) < 0)
      return r;
    struct stat st;
    if (stat(host, &st) < 0) return herrno();
    if (!S_ISDIR(st.st_mode)) return -LX_ENOTDIR;
    if (access(host, X_OK) < 0) return herrno();
    // realpath() through the host resolves ".." against the real symlink topology; map back.
    char rp[PATH_MAX];
    if (realpath(host, rp)) {
      if (g_rootlen && strncmp(rp, g_root, g_rootlen) == 0 &&
          (rp[g_rootlen] == '/' || rp[g_rootlen] == 0))
        snprintf(g_cwd, sizeof g_cwd, "%s", rp[g_rootlen] ? rp + g_rootlen : "/");
      else if (!g_rootlen)
        snprintf(g_cwd, sizeof g_cwd, "%s", rp);
      else
        snprintf(g_cwd, sizeof g_cwd, "%s", gabs);
    } else {
      snprintf(g_cwd, sizeof g_cwd, "%s", gabs);
    }
    return 0;
  }

  // ---------- 56 openat ----------
  case 56: {
    // O_NOFOLLOW must stop the GUEST-side resolver too, not only the host open(): if we resolved
    // the final component here, the host would be handed a non-link and O_NOFOLLOW would silently
    // become a no-op instead of the ELOOP the guest asked for.
    if ((r = resolve_f((int)a0, (const char*)a1, ((int)a2 & LO_NOFOLLOW) ? RSV_NOFOLLOW : 0,
                       host, sizeof host, gabs, sizeof gabs)) < 0)
      return r;
    // RDPATHS=1 names the FILE. RDVERBOSE prints pointers, which is useless when the question is
    // "what is the guest looking for" -- the answer is always a string it never shows you.
    // (From Q2, who needed it to identify a forked child; it never reached main and I then spent
    //  a diagnosis session unable to see what an app was searching for.)
    { static int on = -1;
      if (on < 0) on = getenv("RDPATHS") != 0;
      if (on) fprintf(stderr, "[path] tid=%d openat %s\n", guest_tid(), gabs); }
    FX("%.3f OPENAT tid=%d %s\n", fx_now(), guest_tid(), gabs);

    // /dev/tty: hand back the SHIM's own stderr rather than the host's controlling terminal.
    //
    // The host's /dev/tty returns ENXIO whenever the shim has no controlling terminal -- which
    // is every backgrounded run, i.e. every scripted test. That is not a neutral failure: an app
    // reads it as "there is no terminal" and SILENCES ITSELF. kate does exactly that, and K3
    // measured the whole sequence:
    //
    //     openat("/dev/tty")  -> -6 (ENXIO)
    //     openat("/dev/null") -> 5
    //     dup3(5,0) dup3(5,1) dup3(5,2)   <- stdin, stdout AND stderr to /dev/null
    //
    // So nobody stole kate's stderr; kate threw it away, and every downstream diagnosis of
    // "the app is silent" was chasing a decision the app made on purpose. The control is clean:
    // `kate --help` and `kate --version` never do this and PRINT their errors, and kcalc -- same
    // KF6 stack, same libQt6Core which contains the "/dev/tty" string -- never opens it at all.
    //
    // The shim keeps a private CLOEXEC dup of its own stderr (shim.c) precisely so the guest
    // cannot take our diagnostics away. Handing a dup of that back is better than the host's
    // tty in both directions: it always succeeds, and what the guest writes lands where we can
    // read it instead of on a terminal that may not exist.
    // A DRM render node, so Mesa will load a HARDWARE driver. There is no such device on macOS;
    // the fd is a real (empty) descriptor purely so the guest holds something valid, and every
    // ioctl on it is answered by drmasahi.c. Off unless RDDRM=1 — presenting it changes which
    // driver Mesa picks, so it must be an explicit A/B arm.
    if (drm_is_node(gabs)) {
      int dfd = open("/dev/null", O_RDWR);
      if (dfd >= 0) { drm_register_fd(dfd); return dfd; }
      return herrno();
    }
    // /sys/dev/char/226:*/uevent — libdrm's drmGetDeviceNameFromFd2() reads DEVNAME= here to
    // name the GPU. An empty guest /sys makes it return NULL, which aquamarine dereferences.
    if (drm_sysfs_kind(gabs) == 2) {
      const char* c = drm_sysfs_uevent();
      int ufd = mem_fd(c, strlen(c));
      if (ufd >= 0) return ufd;
      return herrno();
    }
    // /sys/dev/char/226:*/device/{uevent,vendor,device,subsystem_vendor,subsystem_device} —
    // libdrm's drmGetDevice2()/drmGetDevices2() read PCI identity here so the synthetic GPU
    // enumerates as a PCI device and _eglFindDevice() can match it.
    if (drm_sysfs_kind(gabs) == 5 || drm_sysfs_kind(gabs) == 6) {
      const char* c = drm_sysfs_pci_content(gabs);
      if (!c) return -LX_EIO;
      int ufd = mem_fd(c, strlen(c));
      if (ufd >= 0) return ufd;
      return herrno();
    }
    if (cam_is_node(gabs)) {                       // /dev/video0, same shape as the DRM node
      int cfd = open("/dev/null", O_RDWR);
      if (cfd >= 0) { cam_register_fd(cfd); return cfd; }
      return herrno();
    }
    // /dev/snd/* — the synthetic ALSA card. controlC0 needs no direction; the pcm nodes assign
    // their stream direction (playback 'p' / capture 'c') so the ioctl/mmap handlers route right.
    if (snd_is_node(gabs)) {
      int sfd = open("/dev/null", O_RDWR);
      if (sfd < 0) return herrno();
      snd_register_fd(sfd);
      size_t l = strlen(gabs);
      if (l > 1 && gabs[l-1] == 'p') snd_assign_fd(sfd, 0 /*SNDRV_PCM_STREAM_PLAYBACK*/);
      else if (l > 1 && gabs[l-1] == 'c') snd_assign_fd(sfd, 1 /*SNDRV_PCM_STREAM_CAPTURE*/);
      return sfd;
    }
    if (strcmp(gabs, "/dev/tty") == 0) {
      int fd = dup(fileno(stderr));
      if (fd >= 0) return fd;
      // fall through to the normal path if even that fails
    }
    long vres;
    if (proc_open(gabs, &vres)) return vres;
    int mf = open_flags_l2m((int)a2);
    int fd = open(host, mf, (mode_t)a3);
    if (fd < 0) return herrno();
    if (strcmp(gabs,"/dev/ptmx")==0) pty_register(fd);
    return fd;
  }

  // ---------- 57 close ----------
  case 57:
    { int p=pty_index((int)a0); if(p>=0) g_guest_pty[p].master=0; }
    seals_set((int)a0, 0);
    if (getenv("IDLE_PIPE_TRACE") && (int)a0 == 11) {
      struct stat st;
      int ok = fstat((int)a0, &st);
      fprintf(stderr, "[idle-socket] pid=%d op=close fd=11 valid=%d ino=%llu\n",
              getpid(), ok == 0, ok == 0 ? (unsigned long long)st.st_ino : 0ULL);
    }
    dirmap_close((int)a0);
    signalfd_shim_closed((int)a0);
    timerfd_shim_closed((int)a0);
    snd_forget_fd((int)a0);
    // A synthetic DRM render-node fd MUST leave the registry when it is closed. Without this the
    // fd number stays "owned" forever, so the next regular file to reuse that number (glibc's
    // dlopen of libGLESv2 did exactly this) is misclassified: its mmap() is routed to drm_mmap_bo
    // instead of gmem_mmap and fails EINVAL -> "failed to map segment from shared object".
    drm_forget_fd((int)a0);
    return herr(close((int)a0));

  // ---------- 61 getdents64 ----------
  case 61: return do_getdents64((int)a0, (unsigned char*)a1, (size_t)a2);

  // ---------- 62 lseek ----------
  case 62: {
    int w = lseek_whence_l2m((int)a2);
    if (w < 0) return -LX_EINVAL;
    off_t o = lseek((int)a0, (off_t)a1, w);
    return o < 0 ? herrno() : (long)o;
  }

  // ---------- 63 read / 64 write ----------
  case 63: {
    long r = herr((long)read((int)a0, (void*)a1, (size_t)a2));
    if (getenv("IDLE_PIPE_TRACE") && (int)a0 < 32) {
      struct stat st;
      if (fstat((int)a0, &st) == 0 && (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))) {
        fprintf(stderr, "[idle-fd] pid=%d op=read fd=%ld ret=%ld type=%s dev=%llu ino=%llu\n",
                getpid(), a0, r, S_ISFIFO(st.st_mode) ? "pipe" : "socket",
                (unsigned long long)st.st_dev, (unsigned long long)st.st_ino);
        if (S_ISFIFO(st.st_mode) && r == 66) g_idle_saw_address = 1;
        if (S_ISFIFO(st.st_mode) && r == 0 && g_idle_saw_address) g_idle_after_address = 1;
      }
    }
    return r;
  }
  case 64: {
    long r = herr((long)write((int)a0, (const void*)a1, (size_t)a2));
    if (getenv("IDLE_PIPE_TRACE") && (int)a0 < 32) {
      struct stat st;
      if (fstat((int)a0, &st) == 0 && (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)))
        fprintf(stderr, "[idle-fd] pid=%d op=write fd=%ld ret=%ld type=%s dev=%llu ino=%llu\n",
                getpid(), a0, r, S_ISFIFO(st.st_mode) ? "pipe" : "socket",
                (unsigned long long)st.st_dev, (unsigned long long)st.st_ino);
      if (g_idle_after_address && (int)a0 == 11 && r == 8) {
        int pending = -1;
        int ioctl_result = ioctl((int)a0, FIONREAD, &pending);
        struct pollfd p = {.fd=(int)a0, .events=POLLIN};
        int poll_result = poll(&p, 1, 0);
        struct stat st;
        int ok = fstat((int)a0, &st);
        fprintf(stderr, "[idle-eventfd] pid=%d fd=11 pending=%d ioctl=%d poll=%d revents=%#hx ino=%llu\n",
                getpid(), pending, ioctl_result, poll_result, p.revents,
                ok == 0 ? (unsigned long long)st.st_ino : 0ULL);
      }
    }
    return r;
  }

  // ---------- 65 readv / 66 writev ----------  (struct iovec layout matches)
  case 65: return herr((long)readv((int)a0, (const struct iovec*)a1, (int)a2));
  case 66: return herr((long)writev((int)a0, (const struct iovec*)a1, (int)a2));

  // ---------- 67 pread64 / 68 pwrite64 ----------
  case 67: return herr((long)pread((int)a0, (void*)a1, (size_t)a2, (off_t)a3));
  case 68: return herr((long)pwrite((int)a0, (const void*)a1, (size_t)a2, (off_t)a3));

  // ---------- 78 readlinkat ----------
  case 78: {
    char* buf = (char*)a2; size_t sz = (size_t)a3;
    if (!buf || (long)sz <= 0) return -LX_EINVAL;
    if ((r = resolve_f((int)a0, (const char*)a1, RSV_NOFOLLOW, host, sizeof host,
                       gabs, sizeof gabs)) < 0)
      return r;
    if (strcmp(gabs, "/proc/self/exe") == 0) {
      size_t n = strlen(g_exe);
      if (n > sz) n = sz;
      memcpy(buf, g_exe, n);                    // readlink does NOT NUL-terminate
      return (long)n;
    }
    if (strcmp(gabs, "/proc/self/cwd") == 0) {
      size_t n = strlen(g_cwd);
      if (n > sz) n = sz;
      memcpy(buf, g_cwd, n);
      return (long)n;
    }
    // /sys/dev/char/226:*/device/subsystem — libdrm's drmParseSubsystemType() readlink()s this to
    // learn the bus type; report "/pci" so the GPU is treated as a PCI device.
    if (drm_sysfs_kind(gabs) == 4) {
      const char* t = drm_sysfs_link();
      size_t n = strlen(t);
      if (n > sz) n = sz;
      memcpy(buf, t, n);                          // readlink does NOT NUL-terminate
      return (long)n;
    }
    ssize_t n = readlink(host, buf, sz);
    if (n < 0) return herrno();
    // A symlink target that points into the rootfs must be reported in guest coordinates.
    if (g_rootlen && (size_t)n > g_rootlen && strncmp(buf, g_root, g_rootlen) == 0 &&
        buf[g_rootlen] == '/') {
      memmove(buf, buf + g_rootlen, (size_t)n - g_rootlen);
      n -= (ssize_t)g_rootlen;
    }
    return (long)n;
  }

  // ---------- 79 newfstatat / 80 fstat ----------
  case 79: {
    struct stat st;
    struct linux_stat* out = (struct linux_stat*)a2;
    if (!out) return -LX_EFAULT;
    const char* p = (const char*)a1;
    if ((a3 & LX_AT_EMPTY_PATH) && (!p || !*p)) {
      if (fstat((int)a0, &st) < 0) return herrno();
      stat_m2l(&st, out);
      if (drm_is_node(gabs)) drm_fake_stat(&out->st_mode, &out->st_rdev, gabs);
      if (cam_is_node(gabs)) cam_fake_stat(&out->st_mode, &out->st_rdev);
      return 0;
    }
    if ((r = resolve_f((int)a0, p, (a3 & LX_AT_SYMLINK_NOFOLLOW) ? RSV_NOFOLLOW : 0,
                       host, sizeof host, gabs, sizeof gabs)) < 0) return r;
    if (strcmp(gabs, "/proc/self/exe") == 0) guest_to_host(g_exe, host, sizeof host);
    // Synthetic sysfs nodes under /sys/dev/char/226:*/ — answer from drmasahi.c, not the host fs.
    if (drm_sysfs_kind(gabs)) {
      memset(out, 0, sizeof *out);
      drm_fake_stat(&out->st_mode, &out->st_rdev, gabs);
      return 0;
    }
    // /dev/dri/card0 and /dev/dri/renderD128 are synthetic char nodes; the host rootfs only has
    // empty placeholder files so libdrm's opendir("/dev/dri") has something to list. Fake their
    // stat() as the 226:0 / 226:128 char device drmGetDevices2() requires.
    if (drm_is_node(gabs)) {
      memset(out, 0, sizeof *out);
      drm_fake_stat(&out->st_mode, &out->st_rdev, gabs);
      return 0;
    }
    // /dev/snd/* — synthetic ALSA char nodes (major 116).
    if (snd_is_node(gabs)) {
      memset(out, 0, sizeof *out);
      snd_fake_stat(&out->st_mode, &out->st_rdev, gabs);
      return 0;
    }
    int rc = (a3 & LX_AT_SYMLINK_NOFOLLOW) ? lstat(host, &st) : stat(host, &st);
    if (rc < 0) return herrno();
    stat_m2l(&st, out);
    return 0;
  }
  case 80: {
    struct stat st;
    if (!a1) return -LX_EFAULT;
    if (fstat((int)a0, &st) < 0) return herrno();
    stat_m2l(&st, (struct linux_stat*)a1);
    if (drm_owns_fd((int)a0)) {
      struct linux_stat* o = (struct linux_stat*)a1;
      drm_fake_stat(&o->st_mode, &o->st_rdev, "renderD128");
    }
    if (snd_owns_fd((int)a0)) {
      struct linux_stat* o = (struct linux_stat*)a1;
      snd_fake_stat(&o->st_mode, &o->st_rdev, "controlC0");
    }
    return 0;
  }

  // ---------- 93 exit / 94 exit_group ----------
  // exit() ends the CALLING THREAD; exit_group() ends the process. That distinction only started
  // to matter once clone() worked: musl's pthread_exit issues SYS_exit, and terminating the whole
  // process there would kill the program the moment any thread finished.
  case 93:
    FX("%.3f EXIT(thread) tid=%d code=%d vfork=%d child=%d\n",
       fx_now(), guest_tid(), (int)a0, guest_is_vfork_child(), guest_is_child());
    if (guest_is_vfork_child()) guest_vfork_finish(-1, (int)a0);   // never returns
    if (guest_is_child()) {
      guest_thread_exiting();          // CLONE_CHILD_CLEARTID + futex wake, so join() completes
      pthread_exit(NULL);
    }
    fflush(NULL);
    _exit((int)a0);
  case 94:
    FX("%.3f EXIT_GROUP tid=%d code=%d\n", fx_now(), guest_tid(), (int)a0);
    if (fx_on()) fflush(g_fx_f);
    fflush(NULL);
    _exit((int)a0);

  // ---------- 96 set_tid_address ----------
  case 96: guest_set_tid_address(a0); return (long)guest_tid();

  // ---------- 98 futex ----------
  case 98: return do_futex(a0, (int)a1, (unsigned int)a2,
                           (const struct linux_timespec*)a3, a4, (unsigned int)a5);

  // ---------- 99 set_robust_list ----------
  case 99: return 0;

  // ---------- 113 clock_gettime ----------
  case 113: {
    int mc = clockid_l2m((int)a0);
    if (mc < 0) return -LX_EINVAL;
    if (!a1) return -LX_EFAULT;
    struct timespec t;
    if (clock_gettime((clockid_t)mc, &t) < 0) return herrno();
    struct linux_timespec* lt = (struct linux_timespec*)a1;
    lt->tv_sec = t.tv_sec; lt->tv_nsec = t.tv_nsec;
    return 0;
  }

  // ---------- 114 clock_getres ----------
  case 114: {
    int mc = clockid_l2m((int)a0);
    if (mc < 0) return -LX_EINVAL;
    if (a1) {
      struct timespec t;
      if (clock_getres((clockid_t)mc, &t) < 0) { t.tv_sec = 0; t.tv_nsec = 1; }
      struct linux_timespec* lt = (struct linux_timespec*)a1;
      lt->tv_sec = t.tv_sec; lt->tv_nsec = t.tv_nsec ? t.tv_nsec : 1;
    }
    return 0;
  }

  // ---------- 115 clock_nanosleep ----------
  case 115: {
    int mc = clockid_l2m((int)a0);
    if (mc < 0) return -LX_EINVAL;
    const struct linux_timespec* req = (const struct linux_timespec*)a2;
    if (!req) return -LX_EFAULT;
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000L || req->tv_sec < 0) return -LX_EINVAL;
    struct timespec want = { (time_t)req->tv_sec, (long)req->tv_nsec };
    if ((int)a1 & LX_TIMER_ABSTIME) {
      struct timespec now;
      clock_gettime((clockid_t)mc, &now);
      want.tv_sec  -= now.tv_sec;
      want.tv_nsec -= now.tv_nsec;
      if (want.tv_nsec < 0) { want.tv_nsec += 1000000000L; want.tv_sec--; }
      if (want.tv_sec < 0) return 0;            // deadline already passed
    }
    struct timespec rem;
    if (nanosleep(&want, &rem) < 0) {
      if (errno == EINTR && !((int)a1 & LX_TIMER_ABSTIME) && a3) {
        struct linux_timespec* lr = (struct linux_timespec*)a3;
        lr->tv_sec = rem.tv_sec; lr->tv_nsec = rem.tv_nsec;
      }
      return herrno();
    }
    return 0;
  }

  // ---------- 122 sched_setaffinity / 123 sched_getaffinity ----------
  case 122: {
    long target = (long)(int)a0;
    if (target != 0 && target != guest_tid() &&
        !(target == (long)getpid() && guest_tid() == 1))
      return guest_tid_known((int)target) ? -95 : -LX_ESRCH;
    // Darwin does not offer Linux's arbitrary CPU bitmask pinning. Accept the
    // unrestricted mask (the host scheduler already uses all online CPUs),
    // but never report success for a restriction we did not apply.
    size_t sz = (size_t)a1;
    const unsigned char* mask = (const unsigned char*)a2;
    if (!mask) return -LX_EFAULT;
    if (sz < 8 || sz > 128) return -LX_EINVAL;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    if ((size_t)((ncpu + 7) / 8) > sz) return -LX_EINVAL;
    for (long i = 0; i < ncpu && i < 1024; ++i)
      if (!(mask[i / 8] & (1u << (i % 8)))) return -95; // EOPNOTSUPP
    return 0;
  }
  case 123: {
    long target = (long)(int)a0;
    if (target != 0 && target != guest_tid() &&
        !(target == (long)getpid() && guest_tid() == 1))
      return guest_tid_known((int)target) ? -95 : -LX_ESRCH;
    size_t sz = (size_t)a1;
    unsigned char* mask = (unsigned char*)a2;
    if (!mask) return -LX_EFAULT;
    if (sz < 8) return -LX_EINVAL;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    if (sz > 128) sz = 128;                     // kernel copies out a cpumask_t, not `sz`
    memset(mask, 0, sz);
    for (long i = 0; i < ncpu && (size_t)(i / 8) < sz; i++) mask[i / 8] |= (unsigned char)(1u << (i % 8));
    return (long)sz;
  }

  // ---------- 129 kill ----------
  // The last hole in the guest/host pid conflation that 27d9cc0 closed for tkill/tgkill. This one
  // handed a0 straight to the host kill(), and the negative/zero targets are the dangerous half:
  //
  //   kill(0,  sig)  "my process group"  -> on the host that is the SHIM's process group, i.e.
  //                                         the user's shell and every sibling of the test harness
  //   kill(-1, sig)  "everything I may signal" -> every process this UID owns, on the real machine
  //   kill(-pg, sig) "that process group" -> a host process group the guest cannot have meant
  //
  // A guest believes it is alone. It cannot name a group it did not create, so the honest delivery
  // of all three is "this process" — which is exactly what the guest would observe on Linux, where
  // its group contains only itself.
  //
  // Positive pids are the case that legitimately crosses: fork()/wait4() hand the guest REAL host
  // pids, and signalling a child must keep working. So a positive target is allowed through only
  // if we know it — our own pid, one of our guest tids (a thread, so: this process), or a host
  // child we spawned. Anything else is -ESRCH. MEASURED before this change: kill(1, 0) from the
  // guest returned EPERM, meaning it had reached launchd — the identical signature to the abort()
  // bug, in a syscall that fix did not touch.
  case 129: {
    int ms = sig_l2m((int)a1);
    if (ms < 0) return -LX_EINVAL;
    // NB: guest TIDs are deliberately NOT accepted here, unlike in tkill/tgkill. kill(2) names a
    // PROCESS; treating a thread id as a process id is the same conflation from the other side,
    // and it would make kill(1, sig) "succeed" purely because guest tid 1 is the main thread.
    long tgt = (long)(int)a0;
    if (tgt <= 0) return herr(kill(getpid(), ms));            // 0 / -1 / -pgrp: us, never the host
    if (tgt == (long)getpid())          return herr(kill(getpid(), ms));
    if (guest_child_known((int)tgt))    return herr(kill((pid_t)tgt, ms));
    return -LX_ESRCH;                                         // a pid from a namespace we did not issue
  }

  // ---------- 134 rt_sigaction ----------
  // Recorded only: the shim runs the guest under a Mach exception handler and never delivers
  // host signals into guest code, so installing a real handler would be actively wrong.
  case 134: {
    int sig = (int)a0;
    if (sig < 1 || sig > 64) return -LX_EINVAL;
    if (a3 && a3 != 8) return -LX_EINVAL;       // sigsetsize must be 8 on aarch64
    struct linux_sigaction prev = g_sigacts[sig];
    if (a1) g_sigacts[sig] = *(const struct linux_sigaction*)a1;
    if (a2) *(struct linux_sigaction*)a2 = prev;
    return 0;
  }

  // ---------- 135 rt_sigprocmask ----------
  case 135: {
    int howv = (int)a0;                         // SIG_BLOCK 0, SIG_UNBLOCK 1, SIG_SETMASK 2
    if (a3 && a3 != 8) return -LX_EINVAL;
    uint64_t old = g_sigmask;
    if (a1) {
      uint64_t set = *(const uint64_t*)a1;
      if      (howv == 0) g_sigmask |=  set;
      else if (howv == 1) g_sigmask &= ~set;
      else if (howv == 2) g_sigmask  =  set;
      else return -LX_EINVAL;
    }
    if (a2) *(uint64_t*)a2 = old;
    return 0;
  }

  // ---------- 137 rt_sigtimedwait ----------
  // Nothing ever queues a signal to the guest, so the honest answer is "timed out".
  case 137: {
    const struct linux_timespec* to = (const struct linux_timespec*)a2;
    if (to && (to->tv_sec > 0 || to->tv_nsec > 0)) {
      struct timespec t = { (time_t)to->tv_sec, (long)to->tv_nsec };
      nanosleep(&t, 0);
    }
    return -LX_EAGAIN;
  }

  // ---------- 139 rt_sigreturn ----------
  case 139: return 0;                           // unreachable: no signal frames are ever built

  // ---------- 153 times ----------
  case 153: {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) < 0) return herrno();
    if (a0) {
      struct linux_tms* t = (struct linux_tms*)a0;
      t->tms_utime = ru.ru_utime.tv_sec * LX_USER_HZ + ru.ru_utime.tv_usec / (1000000 / LX_USER_HZ);
      t->tms_stime = ru.ru_stime.tv_sec * LX_USER_HZ + ru.ru_stime.tv_usec / (1000000 / LX_USER_HZ);
      struct rusage rc;
      if (getrusage(RUSAGE_CHILDREN, &rc) == 0) {
        t->tms_cutime = rc.ru_utime.tv_sec * LX_USER_HZ + rc.ru_utime.tv_usec / (1000000 / LX_USER_HZ);
        t->tms_cstime = rc.ru_stime.tv_sec * LX_USER_HZ + rc.ru_stime.tv_usec / (1000000 / LX_USER_HZ);
      } else { t->tms_cutime = 0; t->tms_cstime = 0; }
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * LX_USER_HZ + now.tv_nsec / (1000000000L / LX_USER_HZ);
  }

  // ---------- 160 uname ----------
  case 160: {
    struct linux_utsname* u = (struct linux_utsname*)a0;
    if (!u) return -LX_EFAULT;
    memset(u, 0, sizeof *u);
    snprintf(u->sysname,    sizeof u->sysname,    "%s", "Linux");
    snprintf(u->nodename,   sizeof u->nodename,   "%s", "localhost");
    snprintf(u->release,    sizeof u->release,    "%s", "6.1.0");
    snprintf(u->version,    sizeof u->version,    "%s", "#1 SMP PREEMPT r-drift shim");
    snprintf(u->machine,    sizeof u->machine,    "%s", "aarch64");
    snprintf(u->domainname, sizeof u->domainname, "%s", "(none)");
    return 0;
  }

  // ---------- 165 getrusage ----------
  case 165: {
    int who = (int)(long)a0;                    // SELF 0, CHILDREN -1, THREAD 1
    int mwho = who == -1 ? RUSAGE_CHILDREN : RUSAGE_SELF;
    struct rusage ru;
    if (getrusage(mwho, &ru) < 0) return herrno();
    if (!a1) return -LX_EFAULT;
    rusage_m2l(&ru, (struct linux_rusage*)a1);
    return 0;
  }

  // ---------- 172..178 creds ----------
  case 172: return (long)getpid();
  case 173: return (long)getppid();
  // The guest is the host's uid (501 here), not 0. A Linux container answers this with a user
  // namespace and a uid map; macOS has none, but the shim IS the guest's kernel, so it can map.
  // RDUID0=1 reports 0/0. OPT-IN, and deliberately so: the tree is owned by the real uid and every
  // file operation still runs as that uid, so this changes what the guest BELIEVES, not what it may
  // do. Some programs only test `id -u` and then do work the real uid can already perform
  // (dictionaries-common's update-dictcommon-* refuse outright); those are fixed by it. Others
  // branch on uid 0 into chown()/chmod() they would otherwise skip -- dpkg does exactly that, which
  // is why `dpkg --force-not-root` is the better answer there and this is not the default.
  case 174: return g_uid0 ? 0 : (long)getuid();
  case 175: return g_uid0 ? 0 : (long)geteuid();
  case 176: return g_uid0 ? 0 : (long)getgid();
  case 177: return g_uid0 ? 0 : (long)getegid();
  // gettid MUST be the GUEST tid, not the host pid.
  //
  // This used to return getpid() with the comment "single guest thread: tid == pid", which stopped
  // being true the moment clone() worked. Two things were wrong with it at once. It is not
  // per-thread — MEASURED: four guest threads all reported the same number — so anything keying
  // per-thread state on gettid (glibc's malloc arena debug, Chromium, any tid-indexed map) sees
  // one thread. And it CONTRADICTED set_tid_address (syscall 96) two cases below, which has always
  // returned guest_tid(): one identity, two namespaces, depending which syscall you asked.
  //
  // It also fed the signal path the wrong universe. musl's abort() is tkill(gettid(), SIGABRT);
  // 27d9cc0 taught tkill to recognise guest tids via guest_tid_known(), and a gettid that answers
  // with a host pid routes around the very check that fix installed.
  case 178: return (long)guest_tid();

  // ---------- 179 sysinfo ----------
  case 179: {
    struct linux_sysinfo* si = (struct linux_sysinfo*)a0;
    if (!si) return -LX_EFAULT;
    memset(si, 0, sizeof *si);
    struct timespec up;
    clock_gettime(CLOCK_MONOTONIC, &up);
    si->uptime = up.tv_sec;
    uint64_t memsize = 0; size_t msz = sizeof memsize;
    if (sysctlbyname("hw.memsize", &memsize, &msz, 0, 0) != 0) memsize = 8ULL << 30;
    double la[3] = {0, 0, 0};
    if (getloadavg(la, 3) == 3)
      for (int i = 0; i < 3; i++) si->loads[i] = (uint64_t)(la[i] * 65536.0);
    si->mem_unit  = 1;
    si->totalram  = memsize;
    si->freeram   = memsize / 4;
    si->sharedram = 0;
    si->bufferram = 0;
    si->totalswap = 0;
    si->freeswap  = 0;
    si->procs     = 1;
    return 0;
  }

  // ---------- 214 brk ----------
  case 214: return do_brk(a0);

  // ---------- 215 munmap ----------
  case 215: return gmem_munmap(a0, (size_t)a1);

  // ---------- 216 mremap ----------
  case 216: {
    unsigned long old = a0; size_t olds = (size_t)a1, news = (size_t)a2;
    int flags = (int)a3;
    const size_t PG = 16384;
    size_t oldr = (olds + PG - 1) & ~(PG - 1), newr = (news + PG - 1) & ~(PG - 1);
    if (!news) return -LX_EINVAL;
    if (newr <= oldr) {
      if (newr < oldr) gmem_munmap(old + newr, oldr - newr);
      return (long)old;
    }
    // GROW. Linux tries to extend the mapping IN PLACE first, and without MREMAP_MAYMOVE that is
    // the only thing it is allowed to do. Returning ENOMEM unconditionally was not a conservative
    // approximation: a guest that grows a buffer with plain mremap() gets a failure that can
    // never succeed, and the usual reaction -- back off and retry -- becomes an infinite loop.
    // Qt's QML engine does exactly that on startup and spun ~625k mremaps (walking 10 GB of
    // address space one 16K page at a time) instead of ever reaching the first line of QML.
    //
    // The extension is a fresh anonymous RW mapping placed with a hint at the old region's end.
    // gmem_mmap() returns where it actually landed; anywhere but the requested address means the
    // range was taken, so it is released and the move path (if permitted) takes over.
    size_t extlen = newr - oldr;
    static int mr_v = -1;
    if (mr_v < 0) mr_v = getenv("RDVERBOSE") != 0;
    if (mr_v)
      fprintf(stderr, "[mremap] grow %#lx %#zx -> %#zx flags=%#x\n", old, oldr, newr, flags);
    long ext = gmem_mmap(old + oldr, extlen, PROT_READ | PROT_WRITE,
                         LM_PRIVATE | LM_ANONYMOUS, -1, 0);
    if (ext >= 0 || ext <= -4096) {
      if ((unsigned long)ext == old + oldr) return (long)old;    // grew in place
      gmem_munmap((unsigned long)ext, extlen);                   // landed elsewhere: unusable
    }
    if (!(flags & 1 /*MREMAP_MAYMOVE*/)) return -LX_ENOMEM;
    long np = gmem_mmap(0, newr, PROT_READ | PROT_WRITE, LM_PRIVATE | LM_ANONYMOUS, -1, 0);
    if (np < 0 && np > -4096) return np;
    memcpy((void*)np, (const void*)old, olds);
    gmem_munmap(old, oldr);
    return np;
  }

  // ---------- 222 mmap ----------  (flags are LINUX values; gmem_mmap wants exactly that)
  case 222:
    // mmap() on the synthetic DRM render node resolves a GEM fake offset to the BO's host
    // backing (the asahi driver maps command buffers / shader binaries this way). Anything else
    // on the node is not a real file, so it cannot fall through to gmem_mmap's fd path.
    if (drm_owns_fd((int)a4)) {
      void* p = drm_mmap_bo((int)a4, (long)a5);
      if (p) return (long)(uintptr_t)p;
      return -LX_EINVAL;
    }
    // mmap() on a synthetic ALSA pcm node maps the status/control/data ring-buffer pages.
    if (snd_owns_fd((int)a4)) {
      void* p = snd_mmap((int)a4, (long)a5);
      if (p) return (long)(uintptr_t)p;
      return -LX_EINVAL;
    }
    return gmem_mmap(a0, (size_t)a1, (int)a2, (int)a3, (int)a4, (long)a5);

  // ---------- 226 mprotect ----------
  case 226: return gmem_mprotect(a0, (size_t)a1, (int)a2);

  // ---------- 233 madvise ----------
  case 233: return 0;                           // advisory only; never fatal to ignore

  // ---------- 260 wait4 ----------
  case 260: {
    int st = 0;
    int lopt = (int)a2, mopt = 0;
    if (lopt & 1) mopt |= WNOHANG;              // WNOHANG
    if (lopt & 2) mopt |= WUNTRACED;            // WUNTRACED
    if (lopt & 8) mopt |= WCONTINUED;           // Linux WCONTINUED=8, macOS=0x10
    struct rusage ru;
    pid_t p = wait4((pid_t)(long)a0, &st, mopt, a3 ? &ru : 0);
    if (p < 0) return herrno();
    if (a1) *(int*)a1 = st;                     // wait status encoding is compatible
    if (a3 && p > 0) rusage_m2l(&ru, (struct linux_rusage*)a3);
    return (long)p;
  }

  // ---------- 261 prlimit64 ----------
  case 261: {
    static const int lim_l2m[16] = {
      RLIMIT_CPU, RLIMIT_FSIZE, RLIMIT_DATA, RLIMIT_STACK, RLIMIT_CORE, RLIMIT_RSS,
      RLIMIT_NPROC, RLIMIT_NOFILE, RLIMIT_MEMLOCK, RLIMIT_AS, -1, -1, -1, -1, -1, -1,
    };
    int res = (int)a1;
    if (res < 0 || res > 15 || lim_l2m[res] < 0) {
      if (a3) { struct linux_rlimit64* o = (struct linux_rlimit64*)a3;
                o->rlim_cur = o->rlim_max = UINT64_MAX; }
      return 0;                                 // unmodelled limits: report "unlimited"
    }
    struct rlimit rl;
    if (getrlimit(lim_l2m[res], &rl) < 0) return herrno();
    // RLIMIT_NOFILE is not just a number to the guest, it is a LOOP BOUND. The "close every
    // descriptor before exec" idiom -- libdbus's dbus-spawn.c, glib's g_spawn, python's
    // subprocess, busybox -- walks 3..rlim_cur calling close() on each. macOS hands us
    // rlim_cur = 1048576, and on Linux each of those closes is a cheap in-kernel EBADF, whereas
    // here every one is a guest trap through the Mach exception handler.
    //
    // MEASURED by Q2 chasing Qt6: the first Wayland protocol request arrived at t=74s before
    // this clamp and t=1s after. It was never a hang -- it was 74 seconds inside a close() loop,
    // and an 80s watchdog sat right on the boundary, which is why it read as a deadlock.
    //
    // No Linux system ships a soft NOFILE limit anywhere near a million, so reporting one was
    // never faithful. Clamp what the GUEST SEES; the host limit is untouched and still governs
    // how many descriptors can actually be opened.
    if (res == 7 /* RLIMIT_NOFILE */ && rl.rlim_cur > 4096) rl.rlim_cur = 4096;
    if (a3) {
      struct linux_rlimit64* o = (struct linux_rlimit64*)a3;
      o->rlim_cur = rl.rlim_cur == RLIM_INFINITY ? UINT64_MAX : (uint64_t)rl.rlim_cur;
      o->rlim_max = rl.rlim_max == RLIM_INFINITY ? UINT64_MAX : (uint64_t)rl.rlim_max;
    }
    if (a2) {
      const struct linux_rlimit64* n = (const struct linux_rlimit64*)a2;
      rl.rlim_cur = n->rlim_cur == UINT64_MAX ? RLIM_INFINITY : (rlim_t)n->rlim_cur;
      rl.rlim_max = n->rlim_max == UINT64_MAX ? RLIM_INFINITY : (rlim_t)n->rlim_max;
      if (setrlimit(lim_l2m[res], &rl) < 0) return herrno();
    }
    return 0;
  }

  // ---------- 278 getrandom ----------
  case 278: {
    void* buf = (void*)a0; size_t len = (size_t)a1;
    if (!buf && len) return -LX_EFAULT;
    arc4random_buf(buf, len);
    return (long)len;
  }

  // ---------- 293 rseq ----------
  case 293: return -LX_ENOSYS;                  // no restartable sequences under the shim

  default:
    if (g_trace) { fprintf(stderr, "[sc] -> ext nr=%lu\n", nr); fflush(stderr); }
    return linux_syscall_ext(nr, a0, a1, a2, a3, a4, a5);
  }
}
