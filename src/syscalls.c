#include "guest_ownership.h"
#include "guest_cred_registry.h"
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
#include "fastcall.h"
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
#include <sys/event.h>
#include <poll.h>
#include "drmasahi.h"
#include "v4l2cam.h"
#include "snd_alsa.h"
#include "evdev.h"
#include "vt.h"
#include "rdva_host.h"
int g_idle_after_address;
static int g_idle_saw_address;
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/sysctl.h>

#include "syscalls.h"
#include "net.h"
#include "syscalls_ext.h"
#include "signalfd_shim.h"
#include "timerfd_shim.h"
#include "pidfd_shim.h"
#include "inotify_watch.h"
#include "unix_outq_peer.h"
// net.c owns the kqueue-backed epoll; on close(2) it must drop the fd from every epoll set
// (Linux drops a watched fd from epoll automatically on close; kqueue does not).
extern void net_fd_closed(int fd);
extern void rd_rtnl_dup(int oldfd, int newfd);
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
#define LX_ECHILD 10
#define LX_EDEADLK 35
#define LX_ENOMEM 12
#define LX_EACCES 13
#define LX_EROFS 30
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
#define LX_EOPNOTSUPP 95
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
#include "openat2_compat.h"

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

#define SHIM_FD_MAX 512
static int             g_shim_fds[SHIM_FD_MAX];
static int             g_shim_nfd;
static pthread_mutex_t g_shim_fd_mu = PTHREAD_MUTEX_INITIALIZER;

int shim_fd_reserve(int fd) {
  if (fd < 0) return fd;
  // Relocate to a high fd number. systemd's pack_fds() (fd-util.c) consolidates the service's
  // listen fds onto 3,4,5,... via fcntl(F_DUPFD, i+3) + close in a restart loop; if any shim-owned
  // descriptor (pty master, signalfd/timerfd/inotify end, pidfd) sits at a low number, F_DUPFD(3)
  // can never return 3 and pack_fds spins forever. Keep shim fds >= 256 so 3..255 stay free.
  if (fd < 256) {
    int hi = fcntl(fd, F_DUPFD_CLOEXEC, 256);
    if (hi >= 0) { close(fd); fd = hi; }
  }
  pthread_mutex_lock(&g_shim_fd_mu);
  for (int i = 0; i < g_shim_nfd; i++) if (g_shim_fds[i] == fd) { pthread_mutex_unlock(&g_shim_fd_mu); return fd; }
  if (g_shim_nfd < SHIM_FD_MAX) g_shim_fds[g_shim_nfd++] = fd;
  pthread_mutex_unlock(&g_shim_fd_mu);
  // Shim-owned descriptors must never leak into a spawned guest (exec): set FD_CLOEXEC so a
  // posix_spawn/execve child closes them. Otherwise the executor inherits PID 1's pty masters and
  // timerfd/signalfd socketpair ends, fdset_new_fill() sees them, and the service exec loops.
  int rc = fcntl(fd, F_SETFD, FD_CLOEXEC);
  if (getenv("RDEXECDBG") && fd < 64) {
    FILE* f = fopen("/tmp/rd_exec_dbg.txt", "a");
    if (f) { fprintf(f, "pid=%d RESERVE fd=%d cloexec_rc=%d\n", (int)getpid(), fd, rc); fclose(f); }
  }
  return fd;
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
static unsigned g_guest_uid[3], g_guest_gid[3];
static void guest_cred_export(void) {
  static const char* const names[6] = {"RDGUEST_RUID", "RDGUEST_EUID", "RDGUEST_SUID",
                                       "RDGUEST_RGID", "RDGUEST_EGID", "RDGUEST_SGID"};
  char value[24];
  for (int i = 0; i < 6; i++) {
    snprintf(value, sizeof value, "%u", i < 3 ? g_guest_uid[i] : g_guest_gid[i - 3]);
    setenv(names[i], value, 1);
  }
}
unsigned guest_cred_uid(int which) { return g_guest_uid[which]; }
unsigned guest_cred_gid(int which) { return g_guest_gid[which]; }
static long guest_cred_set(unsigned id[3], int real, int effective, int saved) {
  int requested[3] = {real, effective, saved};
  for (int i = 0; i < 3; i++)
    if (requested[i] >= 0 && id[1] != 0 &&
        (unsigned)requested[i] != id[0] && (unsigned)requested[i] != id[1] &&
        (unsigned)requested[i] != id[2]) return -1; // EPERM
  for (int i = 0; i < 3; i++) if (requested[i] >= 0) id[i] = (unsigned)requested[i];
  guest_cred_export();
  if (g_uid0 && guest_cred_registry_update(g_guest_uid[1], g_guest_gid[1]) < 0 &&
      getenv("RDCREDLOG"))
    fprintf(stderr, "[cred] failed to publish virtual credentials for pid=%d\n", (int)getpid());
  return 0;
}
long guest_cred_set_uid(int r, int e, int s) { return guest_cred_set(g_guest_uid, r, e, s); }
long guest_cred_set_gid(int r, int e, int s) { return guest_cred_set(g_guest_gid, r, e, s); }
static int  g_is_init;             // THIS shim process is the guest PID 1 (argv[0] = /sbin/init)
static long g_guest_ppid;          // guest ppid of THIS process (0 for init, else the parent's guest pid)

// The guest pid of THIS process: 1 for init, the host pid otherwise (host pids ARE guest pids for
// non-init processes; the shim is the guest's kernel and issues them).
long syscalls_guest_pid(void) { return g_is_init ? 1 : (long)getpid(); }
void syscalls_set_guest_ppid(long ppid) { g_guest_ppid = ppid; }
long syscalls_get_guest_ppid(void) { return g_guest_ppid; }

static void synth_materialize_all(void);   // defined with the /proc,/sys,/dev synthesis below
static void drm_materialize_udev(void);     // only the actual guest PID1 boot owner updates seat state
static void evdev_materialize_udev(void);
static int  cgroup_dir_path(int fd, char* out, size_t cap);  // defined with the cgroup synthesis below

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
  guest_ownership_set_root(g_root);
  g_trace = getenv("RDSCTRACE") != NULL;
  g_uid0  = getenv("RDUID0")   != NULL;
  if (g_uid0) {
    static const char* const names[6] = {"RDGUEST_RUID", "RDGUEST_EUID", "RDGUEST_SUID",
                                         "RDGUEST_RGID", "RDGUEST_EGID", "RDGUEST_SGID"};
    for (int i = 0; i < 6; i++) {
      const char* value = getenv(names[i]);
      unsigned parsed = value ? (unsigned)strtoul(value, NULL, 10) : 0;
      if (i < 3) g_guest_uid[i] = parsed; else g_guest_gid[i - 3] = parsed;
    }
  }
  // Materialize the synthesized /proc,/sys,/dev directory tree as real (empty) dirs so every
  // path-taking syscall finds them; do_getdents64() serves their entries instead of readdir.
  if (g_rootlen) synth_materialize_all();
}

const char* syscalls_get_root(void) { return g_root; }

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

// Is `guest_path` the guest PID 1? systemd decides "system manager" by getpid()==1, so the shim
// must report 1 for the process the kernel would have launched as init (argv[0] = /sbin/init, the
// systemd-sysvcompat symlink). /usr/lib/systemd/systemd is also the user manager's binary, so
// path alone is insufficient. The boot owner's host PID survives daemon-reexec and is carried
// to children in RDVT_ROOT_PID; only that exact process may claim guest PID 1.
static int guest_path_is_init(const char* p) {
  if (!p) return 0;
  static const char* const inits[] = {
    "/sbin/init", "/usr/sbin/init", "/usr/bin/init", "/bin/init", "/etc/init", "/init",
    "/usr/lib/systemd/systemd",
  };
  for (size_t i = 0; i < sizeof inits / sizeof inits[0]; i++)
    if (strcmp(p, inits[i]) == 0) return 1;
  return 0;
}

void syscalls_set_exe(const char* guest_path) {
  if (guest_path && *guest_path) snprintf(g_exe, sizeof g_exe, "%s", guest_path);
  const char *root = getenv("RDVT_ROOT_PID");
  char *end = NULL;
  long boot_pid = root ? strtol(root, &end, 10) : -1;
  g_is_init = guest_path_is_init(guest_path) && root && end != root && !*end &&
              boot_pid > 0 && boot_pid == (long)getpid();
  if (g_is_init && g_rootlen) { drm_materialize_udev(); evdev_materialize_udev(); }
}

// Scoped PID-1 identity. syscalls_set_exe() sets it for the init process; the fork() child clears
// it (a forked child is a NEW guest process with its own pid, not PID 1). clone(CLONE_THREAD) and
// vfork share the address space, so they must NOT touch it here -- a cloned thread is still part of
// PID 1's thread group, and a vfork child re-exec's (re-running main -> syscalls_set_exe) before it
// can be observed. exec() also re-runs main(), so a forked+exec'd getty/dbus lands here with its
// own argv[0] and g_is_init=0.
void syscalls_set_init(int v) { g_is_init = v; }

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
  // A cgroup child-dir fd is backed by the host's /sys/fs/cgroup dirfd (see cgroup_dir_open), so
  // its F_GETPATH is wrong; the fd->guest-path table is authoritative first. This also makes
  // dirfd-relative mkdirat/openat (systemd's cg_create uses a cgroup dirfd) resolve correctly.
  if (cgroup_dir_path(fd, out, cap) == 0) return 0;
  char hp[PATH_MAX];
  if (fcntl(fd, F_GETPATH, hp) < 0) return herrno();
  if (g_rootlen && strncmp(hp, g_root, g_rootlen) == 0 &&
      (hp[g_rootlen] == '/' || hp[g_rootlen] == 0)) {
    const char* g = hp + g_rootlen;
    snprintf(out, cap, "%s", *g ? g : "/");
  } else {
    // macOS keeps /tmp, /var and /etc as symlinks into /private, and F_GETPATH reports the
    // CANONICAL "/private/..." form. Map that back, so a host fd to /tmp resolves as guest
    // /tmp rather than the nonexistent guest /private/tmp. This is exactly what broke
    // systemd's CREDENTIALS step: fsmount() hands back a /tmp dir fd, fd_reopen(mfd,
    // O_DIRECTORY) does openat(mfd, "."), and the leaked /private/tmp path made every
    // service fail with "Failed to set up credentials: No such file or directory".
    if (strncmp(hp, "/private/", 9) == 0)
      snprintf(out, cap, "%s", hp + 8);                          // "/private/tmp" -> "/tmp"
    else
      snprintf(out, cap, "%s", hp);                              // outside the root (e.g. /dev/*)
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

// /dev/kmsg — the kernel printk buffer. systemd logs here by default before journald is up, and
// its executor is hardcoded to --log-target kmsg. There is no kernel, so writes are appended to a
// per-boot file (so the guest's real error is visible instead of vanishing into a journal socket
// that has no reader yet). journald reads it like a pollable char device: a plain file is ALWAYS
// EOF-readable under kqueue, which made journald's read/epoll loop spin and it never reached
// sd_notify/READY=1. So a READER gets a pipe fed by a tail thread that follows the file — the pipe
// blocks (and only wakes via epoll) when a writer appends, exactly like a char device.
struct kmsg_tail { int fd_w; char path[160]; };

static void* kmsg_tail_thread(void* arg) {
  struct kmsg_tail* t = (struct kmsg_tail*)arg;
  int f = open(t->path, O_RDONLY | O_CLOEXEC);
  if (f < 0) { close(t->fd_w); free(t); return NULL; }
  off_t off = 0;
  char buf[8192];
  for (;;) {
    struct stat st;
    if (fstat(f, &st) == 0 && st.st_size > off) {
      ssize_t n = pread(f, buf, sizeof buf, off);
      if (n <= 0) break;
      off += n;
      if (write(t->fd_w, buf, (size_t)n) < 0) break;   // reader closed the pipe (EPIPE)
    } else {
      usleep(20000);                                    // 20ms poll
    }
  }
  close(f); close(t->fd_w); free(t);
  return NULL;
}

static int kmsg_open(unsigned long lflags) {
  const char* boot = getenv("RDSHIM_BOOT");
  char path[160];
  snprintf(path, sizeof path, "/tmp/rdshim-kmsg-%s.log", boot && *boot ? boot : "0");
  if ((lflags & 3) != 1 /* O_RDONLY or O_RDWR: journald opens O_RDWR and reads */) {
    int p[2];
    if (pipe(p) < 0) { int fd = open(path, O_RDONLY | O_CLOEXEC); return fd < 0 ? herrno() : fd; }
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    fcntl(p[0], F_SETFL, O_NONBLOCK);   // kmsg readers are always non-blocking (journald uses epoll)
    struct kmsg_tail* t = (struct kmsg_tail*)malloc(sizeof *t);
    if (t) {
      t->fd_w = p[1];
      snprintf(t->path, sizeof t->path, "%s", path);
      pthread_t th;
      if (pthread_create(&th, NULL, kmsg_tail_thread, t) == 0) pthread_detach(th);
      else { close(p[1]); free(t); }
    } else close(p[1]);
    return p[0];
  }
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
  return fd < 0 ? herrno() : fd;
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

// /proc/<pid>/root — the pid's mount-namespace root. Every shim guest shares the one rootfs, so
// it is always the guest root "/". PipeWire's flatpak check opens this with O_DIRECTORY and then
// probes ".flatpak-info" inside it; with no mapping the open fails ENOENT, pw_check_flatpak()
// returns that error, and module-access.c treats ANY non-zero as "client is from flatpak" and
// parks the client in "wait for 'flatpak' permissions" forever — which is exactly the pw-cli /
// wireplumber handshake stall. Rewrite /proc/<digits>/root[/...] to /[/...].
// Returns 1 (rewrote) / 0 (not this path) / negative (too long).
static int proc_pid_root(const char* g, char* out, size_t cap) {
  if (strncmp(g, "/proc/", 6) != 0) return 0;
  const char* p = g + 6;
  if (*p < '0' || *p > '9') return 0;
  while (*p >= '0' && *p <= '9') p++;
  if (strncmp(p, "/root", 5) != 0) return 0;
  p += 5;                                      // p is "" or "/..."
  int n = snprintf(out, cap, "%s", *p ? p : "/");
  if (n < 0 || (size_t)n >= cap) return -1;
  return 1;
}

// /proc/self/fd/<N> and /proc/<pid>/fd/<N> — the fd-reopen magic file. systemd's fd_reopen()
// opens this to turn an O_PATH fd into a readable/writable one (e.g. reading a sysfs "uevent"
// file after chasing it). Return the referenced fd number, or -1 if `g` is not such a path.
static int proc_fd_number(const char* g) {
  if (strncmp(g, "/proc/", 6) != 0) return -1;
  const char* p = g + 6;
  if (strncmp(p, "self/fd/", 8) == 0) {
    p += 8;
  } else {
    if (*p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9') p++;
    if (strncmp(p, "/fd/", 4) != 0) return -1;
    p += 4;
  }
  if (*p < '0' || *p > '9') return -1;
  int n = 0;
  while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
  return *p ? -1 : n;
}

static long resolve_f(int dirfd, const char* path, int flags, char* host, size_t hcap,
                      char* gabs, size_t gcap) {
  char tmp[GPATH_MAX];
  char* g = gabs ? gabs : tmp;
  size_t gc = gabs ? gcap : sizeof tmp;
  long r = guest_abs(dirfd, path, g, gc);
  if (r < 0) {
    if (getenv("RDEXECDBG")) {
      FILE* f = fopen("/tmp/rd_exec_dbg.txt", "a");
      if (f) { fprintf(f, "pid=%d RESOLVE-FAIL dirfd=%d path=%s r=%ld\n", (int)getpid(), dirfd, path?path:"-", r); fclose(f); }
    }
    return r;
  }
  // The /dev passthrough list is matched on the path the guest ASKED for, before resolution, so
  // that /dev/null and friends keep going to the host's real device nodes (the rootfs has none).
  if (g_rootlen && !passthru_dev(g)) {
    char lr[GPATH_MAX];
    r = guest_resolve_links(g, !(flags & RSV_NOFOLLOW), lr, sizeof lr);
    if (r < 0) {
      if (getenv("RDEXECDBG")) {
        FILE* f = fopen("/tmp/rd_exec_dbg.txt", "a");
        if (f) { fprintf(f, "pid=%d RESOLVE-LINKS-FAIL dirfd=%d path=%s g=%s r=%ld\n", (int)getpid(), dirfd, path?path:"-", g, r); fclose(f); }
      }
      return r;
    }
    snprintf(g, gc, "%s", lr);
  }
  // /proc/<pid>/root must resolve to the guest root BEFORE guest_to_host() pastes the rootfs on.
  {
    char proot[GPATH_MAX];
    int pr = proc_pid_root(g, proot, sizeof proot);
    if (pr < 0) return -LX_ENAMETOOLONG;
    if (pr > 0) snprintf(g, gc, "%s", proot);
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
  // An explicit confined directory wins. Otherwise use the private per-boot
  // socket group: unlike TMPDIR, RDSOCKETGROUP survives guest execve even when
  // the guest supplies an empty envp. This keeps synthetic /proc readable in
  // exec'd children without falling back to a sandbox-denied /var/tmp.
  const char* td = getenv("RDX_MEMFD_DIR");
  if (!td || !*td) td = getenv("RDSOCKETGROUP");
  if (!td || !*td) td = getenv("TMPDIR");
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
// Resolve the guest pid/ppid a /proc/<pid>/stat or /proc/<pid>/status record must report.
//
// `self` means the reader is asking about its own process (its real guest identity). Otherwise
// `pid` names ANOTHER guest process, and the answer comes from how the shim issued that pid:
//   * pid 1 is init -> ppid 0.
//   * a pid THIS process forked (guest_child_known) -> ppid is our guest pid.
//   * anything else is a descendant an executor/daemon forked on our behalf -> the best honest
//     answer is "descends from init", ppid 1. systemd only compares this field against its own
//     getpid() to answer pid_is_my_child(), so a valid non-zero ppid is what matters.
static void proc_pid_identity(long pid, int is_self, long* out_pid, long* out_ppid) {
  if (is_self) {
    *out_pid  = syscalls_guest_pid();
    *out_ppid = syscalls_get_guest_ppid();
    return;
  }
  *out_pid = pid;
  if (pid == 1)                    *out_ppid = 0;
  else if (guest_child_known((int)pid)) *out_ppid = syscalls_guest_pid();
  else                             *out_ppid = 1;
}

static void proc_stat_for(char* buf, size_t cap, long pid, long ppid, const char* name) {
  unsigned long startstack = g_mstack_hi;                  // top of the main guest stack
  unsigned long start_brk  = g_brk_base;
  snprintf(buf, cap,
    "%ld (%s) R %ld %ld %ld 0 -1 4194304 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0 "
    "18446744073709551615 0 0 %lu 0 0 0 0 0 0 0 0 0 0 0 "   /* 26..39, 28 = startstack */
    "0 0 0 0 0 0 0 %lu 0 0 0 0 0\n",                        /* 40..52, 47 = start_brk  */
    pid, name, ppid, pid, pid, startstack, start_brk);
}

static void proc_stat(char* buf, size_t cap) {
  proc_stat_for(buf, cap, syscalls_guest_pid(), syscalls_get_guest_ppid(), "guest");
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
  if (n > 32) n = 32; // match /sys, /proc/self/status and sched_getaffinity
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

// /proc/uptime — uptime(1), systemd CPU accounting, and any tool measuring elapsed host time.
// Matches the sysinfo(2) handler below: CLOCK_MONOTONIC counts since boot on macOS.
static void proc_uptime(char* b, size_t cap) {
  struct timespec up;
  if (clock_gettime(CLOCK_MONOTONIC, &up) != 0) { up.tv_sec = 0; up.tv_nsec = 0; }
  double total = (double)up.tv_sec + (double)up.tv_nsec / 1e9;
  if (total <= 0.0) {
    struct timeval bt; size_t len = sizeof bt;
    if (sysctlbyname("kern.boottime", &bt, &len, NULL, 0) == 0) {
      struct timeval now; gettimeofday(&now, NULL);
      total = (double)(now.tv_sec - bt.tv_sec) + (double)(now.tv_usec - bt.tv_usec) / 1e6;
    }
  }
  if (total < 0.0) total = 0.0;
  double idle = total * 0.85;                    // plausible idle split (mostly-idle desktop)
  snprintf(b, cap, "%.2f %.2f\n", total, idle);
}

// /proc/loadavg — the host's real 1/5/15-minute load average (same source as sysinfo(2)).
static void proc_loadavg(char* b, size_t cap) {
  double la[3] = {0.08, 0.12, 0.09};
  if (getloadavg(la, 3) != 3) { la[0] = 0.08; la[1] = 0.12; la[2] = 0.09; }
  snprintf(b, cap, "%.2f %.2f %.2f 1/1 %d\n", la[0], la[1], la[2], (int)getpid());
}

// /proc/stat — a minimal but parseable system-wide CPU accounting line plus the standard
// trailers top(1)/vmstat(8)/node_exporter read. `cpu` carries the aggregate jiffies.
static void proc_stat_system(char* b, size_t cap) {
  struct timespec up;
  if (clock_gettime(CLOCK_MONOTONIC, &up) != 0) { up.tv_sec = 0; up.tv_nsec = 0; }
  unsigned long jiffies = (unsigned long)up.tv_sec * 100 + (unsigned long)(up.tv_nsec / 10000000);
  unsigned long idle = jiffies * 85 / 100;
  unsigned long busy = jiffies - idle;
  unsigned long user = busy / 3, system = busy / 3, nice = busy - user - system;
  long btime = (long)time(NULL) - (long)up.tv_sec;
  snprintf(b, cap,
    "cpu  %lu %lu %lu %lu 0 0 0 0 0 0\n"
    "intr 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
    "ctxt 0\n"
    "btime %ld\n"
    "processes 0\n"
    "procs_running 1\n"
    "procs_blocked 0\n",
    user, nice, system, idle, btime);
}

// /proc/version — what uname(1) prints as its "Linux version" line; glibc's own uname() goes
// through the uname(2) syscall, but `cat /proc/version` and several shells parse this file.
static void proc_version(char* b, size_t cap) {
  snprintf(b, cap,
    "Linux version 6.10.10-rdshim (builder@rdshim) (aarch64) #1 SMP PREEMPT_DYNAMIC "
    "Mon Jan  1 00:00:00 UTC 2026\n");
}

// /proc/self/status — ps(1)/pgrep(1)/systemd read the identity fields here. The pid/ppid/uid/gid
// are the real process numbers (g_uid0 fakes uid/gid 0 exactly like the getuid(2) syscall does).
static void proc_status_for(char* b, size_t cap, long pid, long ppid, const char* name) {
  struct rusage ru; memset(&ru, 0, sizeof ru);
  getrusage(RUSAGE_SELF, &ru);
  unsigned long rss = (unsigned long)(ru.ru_maxrss / 1024);   // macOS ru_maxrss is in bytes
  int uid = g_uid0 ? 0 : (int)getuid(), gid = g_uid0 ? 0 : (int)getgid();
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpu < 1)  ncpu = 1;
  if (ncpu > 32) ncpu = 32;
  unsigned long mask = (ncpu >= 64) ? ~0UL : ((1UL << ncpu) - 1);
  snprintf(b, cap,
    "Name:\t%s\n"
    "Umask:\t0022\n"
    "State:\tR (running)\n"
    "Tgid:\t%d\n"
    "Pid:\t%d\n"
    "PPid:\t%d\n"
    "TracerPid:\t0\n"
    "Uid:\t%d\t%d\t%d\t%d\n"
    "Gid:\t%d\t%d\t%d\t%d\n"
    "FDSize:\t256\n"
    "Groups:\t%d \n"
    "VmPeak:\t%10lu kB\n"
    "VmSize:\t%10lu kB\n"
    "VmLck:\t         0 kB\n"
    "VmHWM:\t%10lu kB\n"
    "VmRSS:\t%10lu kB\n"
    "RssAnon:\t%10lu kB\n"
    "RssFile:\t         0 kB\n"
    "RssShmem:\t         0 kB\n"
    "Threads:\t1\n"
    "SigQ:\t0/0\n"
    "SigPnd:\t0000000000000000\n"
    "ShdPnd:\t0000000000000000\n"
    "SigBlk:\t0000000000000000\n"
    "SigIgn:\t0000000000000000\n"
    "SigCgt:\t0000000000000000\n"
    "CapInh:\t0000000000000000\n"
    "CapPrm:\t0000000000000000\n"
    "CapEff:\t0000000000000000\n"
    "CapBnd:\t0000000000000000\n"
    "CapAmb:\t0000000000000000\n"
    "Seccomp:\t0\n"
    "Cpus_allowed:\t%lx\n"
    "Cpus_allowed_list:\t0-%ld\n"
    "Mems_allowed:\t1\n"
    "Mems_allowed_list:\t0\n"
    "voluntary_ctxt_switches:\t1\n"
    "nonvoluntary_ctxt_switches:\t0\n",
    name,
    (int)pid, (int)pid, (int)ppid,
    uid, uid, uid, uid,
    gid, gid, gid, gid,
    gid,
    rss, rss * 8, rss, rss, rss,
    mask, ncpu - 1);
}

static void proc_self_status(char* b, size_t cap) {
  const char* name = strrchr(g_exe, '/');
  name = name ? name + 1 : g_exe;
  if (!*name) name = "guest";
  proc_status_for(b, cap, syscalls_guest_pid(), syscalls_get_guest_ppid(), name);
}

// /proc/self/environ — NUL-separated K=V block, exactly the shape the kernel exposes. Returns the
// byte length (mem_fd must be handed the length, not strlen, because the body embeds NULs).
static size_t proc_self_environ(char* b, size_t cap) {
  extern char** environ;
  size_t o = 0;
  if (environ) {
    for (char** e = environ; *e; e++) {
      size_t n = strlen(*e) + 1;
      if (o + n > cap) break;
      memcpy(b + o, *e, n);
      o += n;
    }
  }
  return o;
}

// /proc/self/auxv — a plausible aarch64 Elf64_auxv_t stream (u64 type/value pairs, AT_NULL ended).
// The identity values (pagesz/hwcap/clk_tck/uid/gid) match what bootstrap.c puts on the guest's
// real initial stack, so getauxval()-based probing sees a consistent picture.
static size_t proc_self_auxv(char* b, size_t cap) {
  static const uint64_t av[][2] = {
    {  3, 0x100000040ULL },   // AT_PHDR
    {  4, 56ULL },            // AT_PHENT
    {  5, 11ULL },            // AT_PHNUM
    {  6, 16384ULL },         // AT_PAGESZ (GUEST_PAGESZ)
    {  7, 0ULL },             // AT_BASE (static)
    {  8, 0ULL },             // AT_FLAGS
    {  9, 0x100000580ULL },   // AT_ENTRY
    { 11, 501ULL },           // AT_UID
    { 12, 501ULL },           // AT_EUID
    { 13, 20ULL },            // AT_GID
    { 14, 20ULL },            // AT_EGID
    { 16, 0x103ULL },         // AT_HWCAP (FP | ASIMD | ATOMICS)
    { 17, 100ULL },           // AT_CLKTCK
    { 23, 0ULL },             // AT_SECURE
    { 25, 0x7ffffff000ULL },  // AT_RANDOM
    { 26, 0ULL },             // AT_HWCAP2
    { 31, 0x7ffffff010ULL },  // AT_EXECFN
    { 33, 0ULL },             // AT_SYSINFO_EHDR
    {  0, 0ULL },             // AT_NULL (terminator)
  };
  size_t n = sizeof av / sizeof av[0];
  size_t bytes = n * 16;
  size_t w = bytes < cap ? bytes : cap - (cap % 16);
  for (size_t i = 0; i + 16 <= w; i += 16) {
    memcpy(b + i,     &av[i / 16][0], 8);
    memcpy(b + i + 8, &av[i / 16][1], 8);
  }
  return w;
}

// /sys/devices/system/cpu/{possible,online,present,offline} — the CPU-set leaves lscpu and every
// topology walker read first. The count is the SAME clamp sys_cpu_entries() uses so the sets agree
// with the cpu0..cpuN directories. A contiguous 0..N-1 set is the range "0-(N-1)".
static void sys_cpu_list(char* b, size_t cap, int offline) {
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpu < 1)  ncpu = 1;
  if (ncpu > 32) ncpu = 32;
  if (offline) { snprintf(b, cap, "\n"); return; }     // everything online -> empty set
  if (ncpu == 1) snprintf(b, cap, "0\n");
  else           snprintf(b, cap, "0-%ld\n", ncpu - 1);
}

static long guest_uts_gethostname(char name[65]);
static long guest_uts_getdomainname(char name[65]);

// Returns 1 if the path is a virtual /proc (or synthetic /sys leaf) file we serve; *fd is then the
// result (or -errno).
static int proc_open(const char* g, long* out) {
  char buf[4096];
  if (getenv("RDEXECDBG") && strncmp(g, "/proc/", 6) == 0)
    fprintf(stderr, "[proc-open] '%s'\n", g);

  // Parse /proc/<digits>/<leaf>. The identity leaves stat/status (and fdinfo) depend on WHICH pid
  // is named: a child pid must produce a CHILD record, not the reader's own. Everything else keeps
  // the historical "one guest process per host process -> /proc/<pid>/* == /proc/self/*" rule
  // (systemd reads its own identity through /proc/1/* during early boot).
  long pidnum = -1;                 // parsed pid, or -1 if not /proc/<digits>/<leaf>
  const char* leaf = NULL;          // the "/leaf..." suffix when pidnum >= 0
  if (strncmp(g, "/proc/", 6) == 0) {
    const char* p = g + 6;
    if (*p >= '0' && *p <= '9') {
      const char* q = p;
      long v = 0;
      while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); if (v > 0x7fffffff) v = 0x7fffffff; q++; }
      if (*q == '/' && q[1]) { pidnum = v; leaf = q; }
    }
  }

  // /proc/<pid>/stat and /proc/<pid>/status — the record is for THAT pid (systemd's
  // pid_get_ppid/pid_is_my_child read these for arbitrary child pids).
  if (pidnum >= 0 && leaf && (strcmp(leaf, "/stat") == 0 || strcmp(leaf, "/status") == 0)) {
    long rp, rpp;
    int is_self = (pidnum == syscalls_guest_pid() || pidnum == (long)getpid());
    proc_pid_identity(pidnum, is_self, &rp, &rpp);
    if (strcmp(leaf, "/stat") == 0) proc_stat_for(buf, sizeof buf, rp, rpp, "guest");
    else                            proc_status_for(buf, sizeof buf, rp, rpp, "guest");
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }

  // /proc/self/fdinfo/<N> and /proc/<pid>/fdinfo/<N> — the pidfd metadata surface. systemd's
  // pidfd_get_pid() falls back here after PIDFD_GET_INFO and maps a missing file to -EBADF, so the
  // "Pid:" line must exist for pidfds (and only pidfds) or every pidref_verify() fails.
  {
    int fdn = -1;
    if (strncmp(g, "/proc/self/fdinfo/", 18) == 0) fdn = atoi(g + 18);
    else if (pidnum >= 0 && leaf && strncmp(leaf, "/fdinfo/", 8) == 0) fdn = atoi(leaf + 8);
    if (fdn >= 0) {
      pid_t pfd = pidfd_shim_pid(fdn);
      if (pfd > 0) {
        long rp, rpp;
        proc_pid_identity((long)pfd, 0, &rp, &rpp);
        snprintf(buf, sizeof buf,
                 "pos:\t0\n"
                 "flags:\t02000000\n"
                 "mnt_id:\t0\n"
                 "ino:\t0\n"
                 "Pid:\t%ld\n"
                 "NSpid:\t%ld\n", rp, rp);
      } else {
        snprintf(buf, sizeof buf, "pos:\t0\nflags:\t02000000\nmnt_id:\t0\nino:\t0\n");
      }
      int fd = mem_fd(buf, strlen(buf));
      *out = fd < 0 ? herrno() : fd;
      return 1;
    }
  }

  // Normalize the remaining /proc/<digits>/<leaf> forms to /proc/self/<leaf>.
  char norm[GPATH_MAX];
  if (pidnum >= 0 && leaf) { snprintf(norm, sizeof norm, "/proc/self%s", leaf); g = norm; }
  if (strcmp(g, "/proc/cmdline") == 0) {
    // The kernel command line systemd parses for `systemd.*`/`init.*` options. "ro" is enough for a
    // clean boot; systemd's own log level/target arrive via argv (or its compiled-in defaults).
    snprintf(buf, sizeof buf, "ro\n");
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
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
  if (strcmp(g, "/proc/self/mounts") == 0) {
    // /etc/mtab normally points here. Package managers inspect this table before
    // committing a transaction; only the guest-visible root and proc are listed.
    snprintf(buf, sizeof buf, "rootfs / ext4 rw,relatime 0 0\n"
                              "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n");
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
  if (strcmp(g, "/proc/uptime") == 0) {
    proc_uptime(buf, sizeof buf);
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/loadavg") == 0) {
    proc_loadavg(buf, sizeof buf);
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/stat") == 0) {
    proc_stat_system(buf, sizeof buf);
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/version") == 0) {
    proc_version(buf, sizeof buf);
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  // /proc/consoles + /sys/class/tty/console/active — systemd's getty generator calls
  // get_kernel_consoles() to learn which virtual consoles to spawn getty on. Without these it logs
  // "Failed to get active kernel consoles" and never starts getty@tty1, so no login prompt.
  if (strcmp(g, "/proc/consoles") == 0) {
    snprintf(buf, sizeof buf, "tty1                 -W- (EC p    )    4:1\n");
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/sys/class/tty/console/active") == 0) {
    snprintf(buf, sizeof buf, "tty1\n");
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/sys/class/tty/tty0/active") == 0) {
    snprintf(buf, sizeof buf, "tty1\n");
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  // /proc/sys/kernel/* — systemd/journald read these at startup; the OPEN-FAIL log showed
  // osrelease/hostname/boot_id all ENOENT, and journald treats the missing hostname as fatal.
  if (strcmp(g, "/proc/sys/kernel/hostname") == 0) {
    char hostname[65];
    long e = guest_uts_gethostname(hostname);
    if (e < 0) { *out = e; return 1; }
    snprintf(buf, sizeof buf, "%s\n", hostname);
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/sys/kernel/domainname") == 0) {
    char domainname[65];
    long e = guest_uts_getdomainname(domainname);
    if (e < 0) { *out = e; return 1; }
    snprintf(buf, sizeof buf, "%s\n", domainname);
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/sys/kernel/osrelease") == 0) {
    snprintf(buf, sizeof buf, "6.1.0-shim\n");
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/sys/kernel/random/boot_id") == 0) {
    snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012x\n",
             (unsigned)getpid(), 0x4d4d, 0x5d5d, 0x6d6d, (unsigned)(getpid() * 2654435761u));
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/self/status") == 0) {
    proc_self_status(buf, sizeof buf);
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/self/cgroup") == 0) {
    // cgroup v2 unified hierarchy, root membership. systemd reads /proc/1/cgroup to learn its own
    // cgroup; "0::/" = hierarchy 0, no bound controllers, path "/" — the honest no-cgroup answer.
    snprintf(buf, sizeof buf, "0::/\n");
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  // /proc/self/oom_score_adj (+ oom_score/oom_adj/coredump_filter): systemd's exec_context_apply()
  // writes the unit's OOMScoreAdjust/CoredumpFilter here. The shim has no OOM killer and no
  // coredump filtering, so accept the write (mem_fd is a read/write temp file) and report "0".
  // MEASURED: systemd-executor exits EXIT_OOM_ADJUST (206) when /proc/self/oom_score_adj can't be
  // opened, which aborted journald (OOMScoreAdjust=-900) before it could start.
  if (strcmp(g, "/proc/self/oom_score_adj") == 0 ||
      strcmp(g, "/proc/self/oom_score")     == 0 ||
      strcmp(g, "/proc/self/oom_adj")       == 0 ||
      strcmp(g, "/proc/self/coredump_filter") == 0) {
    snprintf(buf, sizeof buf, "0\n");
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/cgroups") == 0) {
    // No cgroup v1 controllers are bound (the shim is v2-only / no-cgroup). systemd reads this to
    // decide between the legacy and unified hierarchy; an empty table means "use cgroup v2".
    snprintf(buf, sizeof buf, "#subsys_name\thierarchy\tnum_cgroups\tenabled\n");
    int fd = mem_fd(buf, strlen(buf));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/self/environ") == 0) {
    static char envb[65536];
    size_t n = proc_self_environ(envb, sizeof envb);
    int fd = mem_fd(envb, n);
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (strcmp(g, "/proc/self/auxv") == 0) {
    static char auxb[1024];
    size_t n = proc_self_auxv(auxb, sizeof auxb);
    int fd = mem_fd(auxb, n);
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  // /sys/devices/system/cpu/{possible,online,present,offline}
  if (strcmp(g, "/sys/devices/system/cpu/possible") == 0 ||
      strcmp(g, "/sys/devices/system/cpu/online")   == 0 ||
      strcmp(g, "/sys/devices/system/cpu/present")  == 0 ||
      strcmp(g, "/sys/devices/system/cpu/offline")  == 0) {
    sys_cpu_list(buf, sizeof buf, strcmp(g, "/sys/devices/system/cpu/offline") == 0);
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
  if (lf & LO_NOFOLLOW) {
    // Linux open(symlink, O_PATH|O_NOFOLLOW) returns an O_PATH fd to the symlink INODE,
    // and systemd's chase() relies on exactly that (open the component, statx it, S_ISLNK ->
    // readlinkat). macOS O_NOFOLLOW instead fails with ELOOP on a symlink, so for the O_PATH
    // combination open the link itself with O_SYMLINK, whose fstat() reports S_IFLNK.
    if (lf & LO_PATH) mf |= O_SYMLINK;
    else              mf |= O_NOFOLLOW;   // plain O_NOFOLLOW: ELOOP on a symlink, as on Linux
  }
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

/* A private socket group identifies one guest boot, including existing sibling
 * shims and re-execed programs. The lock serializes readers and writers;
 * rename publishes a complete record even if a writer dies during staging. */
#define GUEST_UTS_MAGIC 0x52445554u
#define GUEST_UTS_DOMAIN_MAGIC 0x52445544u
struct guest_uts_record { uint32_t magic, length; char name[65]; };
static int guest_uts_dir(void) {
  const char *group = getenv("RDSOCKETGROUP");
  if (!group || group[0] != '/') { errno = ENOTSUP; return -1; }
  int dir = open(group, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dir < 0) return -1;
  struct stat st;
  if (fstat(dir, &st) < 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & 077) != 0) {
    close(dir); errno = EACCES; return -1;
  }
  return dir;
}
static int guest_uts_lock(int dir, int mode) {
  int fd = openat(dir, ".rdx-uts.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & 077) != 0 || st.st_nlink != 1 ||
      flock(fd, mode) < 0) {
    close(fd); errno = EACCES; return -1;
  }
  return fd;
}
static long guest_uts_getname(char name[65], const char *fallback,
                              const char *record_path, uint32_t magic) {
  snprintf(name, 65, "%s", fallback);
  if (!getenv("RDSOCKETGROUP")) return 0;
  int dir = guest_uts_dir();
  if (dir < 0) return herrno();
  int lock = guest_uts_lock(dir, LOCK_SH);
  if (lock < 0) { long e = herrno(); close(dir); return e; }
  int fd = openat(dir, record_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0 && errno == ENOENT) { close(lock); close(dir); return 0; }
  if (fd < 0) { long e = herrno(); close(lock); close(dir); return e; }
  struct stat st;
  struct guest_uts_record rec;
  int valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
              st.st_uid == geteuid() && (st.st_mode & 077) == 0 && st.st_nlink == 1 &&
              st.st_size == sizeof rec && pread(fd, &rec, sizeof rec, 0) == sizeof rec &&
              rec.magic == magic && rec.length <= 64 && rec.name[rec.length] == 0;
  close(fd); close(lock); close(dir);
  if (!valid) return -LX_EIO;
  memcpy(name, rec.name, 65);
  return 0;
}
static long guest_uts_gethostname(char name[65]) {
  return guest_uts_getname(name, "localhost", ".rdx-uts", GUEST_UTS_MAGIC);
}
static long guest_uts_getdomainname(char name[65]) {
  return guest_uts_getname(name, "(none)", ".rdx-domain", GUEST_UTS_DOMAIN_MAGIC);
}
static long guest_uts_setname(const char *name, size_t length,
                              const char *record_path, const char *temp_prefix,
                              uint32_t magic) {
  if (length > 64) return -LX_EINVAL;
  if (!getenv("RDSOCKETGROUP")) return -LX_EOPNOTSUPP;
  if ((g_uid0 ? guest_cred_uid(1) : (unsigned)geteuid()) != 0) return -LX_EPERM;
  int dir = guest_uts_dir();
  if (dir < 0) return herrno();
  int lock = guest_uts_lock(dir, LOCK_EX);
  if (lock < 0) { long e = herrno(); close(dir); return e; }
  struct guest_uts_record rec = { .magic = magic, .length = (uint32_t)length };
  if (length) memcpy(rec.name, name, length);
  char temp[64];
  int fd = -1;
  for (int i = 0; i < 8; ++i) {
    snprintf(temp, sizeof temp, "%s-%ld-%08x", temp_prefix, (long)getpid(), arc4random());
    fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0 || errno != EEXIST) break;
  }
  if (fd < 0) { long e = herrno(); close(lock); close(dir); return e; }
  ssize_t written = write(fd, &rec, sizeof rec);
  int saved = written < 0 ? errno : (written == sizeof rec ? 0 : EIO);
  if (!saved && fsync(fd) < 0) saved = errno;
  if (!saved && renameat(dir, temp, dir, record_path) < 0) saved = errno;
  close(fd);
  if (saved) unlinkat(dir, temp, 0);
  close(lock); close(dir);
  if (saved) { errno = saved; return herrno(); }
  return 0;
}
long syscalls_guest_sethostname(const char *name, size_t length) {
  return guest_uts_setname(name, length, ".rdx-uts", ".rdx-uts",
                           GUEST_UTS_MAGIC);
}
long syscalls_guest_setdomainname(const char *name, size_t length) {
  return guest_uts_setname(name, length, ".rdx-domain", ".rdx-domain",
                           GUEST_UTS_DOMAIN_MAGIC);
}

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
static int stat_m2l(const struct stat* s, struct linux_stat* l) {
  memset(l, 0, sizeof *l);
  l->st_dev        = (uint64_t)s->st_dev;
  l->st_ino        = (uint64_t)s->st_ino;
  l->st_mode       = (uint32_t)s->st_mode;
  l->st_nlink      = (uint32_t)s->st_nlink;
  // RDUID0 defaults untracked files to root; virtual chown metadata overrides
  // that default without changing Darwin ownership. Metadata errors propagate.
  l->st_uid        = g_uid0 ? 0 : (uint32_t)s->st_uid;
  l->st_gid        = g_uid0 ? 0 : (uint32_t)s->st_gid;
  if (g_uid0 && guest_ownership_get(s, &l->st_uid, &l->st_gid)<0) return -1;
  l->st_rdev       = (uint64_t)s->st_rdev;
  l->st_size       = (int64_t)s->st_size;
  l->st_blksize    = (int32_t)s->st_blksize;
  l->st_blocks     = (int64_t)s->st_blocks;
  l->st_atime_sec  = s->st_atimespec.tv_sec;  l->st_atime_nsec = s->st_atimespec.tv_nsec;
  l->st_mtime_sec  = s->st_mtimespec.tv_sec;  l->st_mtime_nsec = s->st_mtimespec.tv_nsec;
  l->st_ctime_sec  = s->st_ctimespec.tv_sec;  l->st_ctime_nsec = s->st_ctimespec.tv_nsec;
  return 0;
}

// Linux filesystem magic by mount point. libudev's sd_device refuses a syspath whose
// fstatfs() is not SYSFS_MAGIC ("outside of sysfs"), so /sys must report sysfs, not ext4.
static int64_t fs_magic_for(const char* g) {
  // cgroup2 MUST report CGROUP2_SUPER_MAGIC or systemd's cg_unified() decides it is running
  // without a unified hierarchy and skips cgroup setup (no controllers, no init.scope, no units).
  if (g && strncmp(g, "/sys/fs/cgroup", 14) == 0 && (g[14] == '/' || g[14] == 0))
    return 0x63677270;                          // CGROUP2_SUPER_MAGIC
  if (g && strncmp(g, "/sys", 4) == 0 && (g[4] == '/' || g[4] == 0)) return 0x62656572;  // SYSFS_MAGIC
  if (g && strncmp(g, "/proc", 5) == 0 && (g[5] == '/' || g[5] == 0)) return 0x9fa0;     // PROC_SUPER_MAGIC
  return 0xEF53;                               // claim ext2/3/4; nothing sane keys off this
}
static void statfs_m2l(const struct statfs* s, struct linux_statfs* l, const char* gpath) {
  memset(l, 0, sizeof *l);
  l->f_type    = fs_magic_for(gpath);
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
  if (getenv("RDFX_NODEATH")) return;   // DIAG: keep SIGTERM/SIGQUIT/etc at DFL so signalfd can claim them
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
  if (getenv("RDEXECDBG")) {
    FILE* f = fopen("/tmp/rd_exec_dbg.txt", "a");
    if (f) { fprintf(f, "pid=%d FUTEX-WAKE uaddr=%#lx n=%u bits=%#x -> woke=%d here=%d mates=%d\n",
                     (int)getpid(), uaddr, n, bits, woke, here, mates); fclose(f); }
  }
  return woke;
}

// futex_wake(2), the four-argument futex2 syscall (aarch64 #454). Its mask is the
// equivalent of FUTEX_WAKE_BITSET's val3, but precedes the wake count in the ABI.
// Our futex queues are process-local and keyed by virtual address, so only the
// private 32-bit form can be implemented by this backend. Do not claim a shared
// wake succeeded when another process may own the waiter.
static long do_futex_wake2(unsigned long uaddr, unsigned long mask,
                           int nr, unsigned int flags) {
  enum { FU2_SIZE_MASK = 3, FU2_SIZE_U32 = 2, FU2_NUMA = 4,
         FU2_MPOL = 8, FU2_PRIVATE = 128 };
  if (flags & ~(FU2_SIZE_MASK | FU2_NUMA | FU2_MPOL | FU2_PRIVATE)) return -LX_EINVAL;
  if ((flags & FU2_SIZE_MASK) != FU2_SIZE_U32 || mask > UINT32_MAX) return -LX_EINVAL;
  if (!mask) return -LX_EINVAL;
  if (uaddr & (sizeof(uint32_t) - 1)) return -LX_EINVAL;
  if (!uaddr) return -LX_EFAULT;
  if (!(flags & FU2_PRIVATE) || (flags & (FU2_NUMA | FU2_MPOL))) return -95; // EOPNOTSUPP
  // Linux's strict futex2 wake returns zero for nr=0. For a negative int the
  // kernel wakes the first matching waiter, since its post-increment count
  // immediately meets the negative limit.
  return fu_wake(uaddr, nr < 0 ? 1u : (unsigned int)nr, (uint32_t)mask);
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
        if (getenv("RDEXECDBG")) {
          FILE* f = fopen("/tmp/rd_exec_dbg.txt", "a");
          if (f) { fprintf(f, "pid=%d FUTEX-EAGAIN uaddr=%#lx val=%u word=%u op=%#x\n",
                           (int)getpid(), uaddr, val, seen, op); fclose(f); }
        }
        return -LX_EAGAIN;
      }

      struct fu_waiter w = { uaddr, bits, 0, b->head, guest_tid(), fx_now() };
      b->head = &w;
      FX("%.3f WAIT uaddr=%#lx bkt=%d tid=%d op=%#x val=%u bits=%#x to=%d -> PARK\n",
         w.fx_t0, uaddr, (int)(b - g_fu), w.fx_tid, op, val, bits, have_to);
      if (getenv("RDEXECDBG")) {
        FILE* f = fopen("/tmp/rd_exec_dbg.txt", "a");
        if (f) { fprintf(f, "pid=%d FUTEX-WAIT uaddr=%#lx tid=%d val=%u word=%u op=%#x bits=%#x -> PARK\n",
                         (int)getpid(), uaddr, guest_tid(), val, *p, op, bits); fclose(f); }
      }

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
      if (getenv("RDEXECDBG")) {
        FILE* f = fopen("/tmp/rd_exec_dbg.txt", "a");
        if (f) { fprintf(f, "pid=%d FUTEX-WAIT-DONE uaddr=%#lx tid=%d -> %s word_now=%u\n",
                         (int)getpid(), uaddr, w.fx_tid,
                         woken ? "WOKEN" : (timedout ? "TIMEDOUT" : "SPURIOUS"), *p); fclose(f); }
      }

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

// futex_wait(2), aarch64 #455.  Reuse the existing private 32-bit
// FUTEX_WAIT_BITSET queue: both ABIs wait for an absolute deadline and wake
// only when the caller's bitset intersects the wake mask.
static long do_futex_wait2(unsigned long uaddr, unsigned long val,
                           unsigned long mask, unsigned int flags,
                           unsigned long timeout_addr, int clockid) {
  enum { FU2_SIZE_MASK = 3, FU2_SIZE_U32 = 2, FU2_NUMA = 4,
         FU2_MPOL = 8, FU2_PRIVATE = 128 };
  if (flags & ~(FU2_SIZE_MASK | FU2_NUMA | FU2_MPOL | FU2_PRIVATE)) return -LX_EINVAL;
  if ((flags & FU2_SIZE_MASK) != FU2_SIZE_U32 ||
      val > UINT32_MAX || mask > UINT32_MAX || !mask) return -LX_EINVAL;
  if (uaddr & (sizeof(uint32_t) - 1)) return -LX_EINVAL;
  if (!uaddr) return -LX_EFAULT;
  if (!(flags & FU2_PRIVATE) || (flags & (FU2_NUMA | FU2_MPOL))) return -95; // EOPNOTSUPP

  uint32_t snapshot;
  if (gmem_guest_export_copyin(NULL, uaddr, &snapshot, sizeof snapshot) != 0)
    return -LX_EFAULT;
  struct linux_timespec timeout;
  const struct linux_timespec* to = NULL;
  if (timeout_addr) {
    if (clockid != LX_CLOCK_MONOTONIC && clockid != LX_CLOCK_REALTIME)
      return -LX_EINVAL;
    if (gmem_guest_export_copyin(NULL, timeout_addr, &timeout, sizeof timeout) != 0)
      return -LX_EFAULT;
    if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000L)
      return -LX_EINVAL;
    to = &timeout;
  }
  int op = FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG;
  if (clockid == LX_CLOCK_REALTIME && to) op |= FUTEX_CLOCK_REALTIME;
  return do_futex(uaddr, op, (uint32_t)val, to, 0, (uint32_t)mask);
}

// futex_waitv(2), aarch64 #449. A one-element vector is equivalent to the
// private U32 futex2 wait above, with an all-bits wake mask and result index 0.
// The current waiter/bucket model cannot atomically park on several addresses;
// reject larger vectors rather than risk a lost wake.
static long do_futex_waitv(unsigned long waiters_addr, unsigned int count,
                           unsigned int flags, unsigned long timeout_addr,
                           int clockid) {
  struct fu2_waiter_arg {
    uint64_t val, uaddr;
    uint32_t flags, reserved;
  } waiter;
  if (flags || !waiters_addr || !count || count > 128) return -LX_EINVAL;
  if (count != 1) return -95; // EOPNOTSUPP: multi-address atomic wait needs new queue state.
  if (gmem_guest_export_copyin(NULL, waiters_addr, &waiter, sizeof waiter) != 0)
    return -LX_EFAULT;
  if (waiter.reserved) return -LX_EINVAL;
  return do_futex_wait2((unsigned long)waiter.uaddr, (unsigned long)waiter.val,
                        FU_BITS_ANY, waiter.flags, timeout_addr, clockid);
}

// futex_requeue(2), aarch64 #456. The current queue keeps a sleeping thread on
// one bucket/condition variable, so moving it to another address would need a
// queue redesign. nr_requeue == 0 has exact wake-only semantics and can be
// handled under the source bucket lock, including the source-word comparison.
static long do_futex_requeue2(unsigned long waiters_addr, unsigned int flags,
                              int nr_wake, int nr_requeue) {
  struct fu2_waiter_arg {
    uint64_t val, uaddr;
    uint32_t flags, reserved;
  } pair[2];
  enum { FU2_SIZE_U32 = 2, FU2_PRIVATE = 128 };
  if (flags || !waiters_addr || nr_wake < 0 || nr_requeue < 0)
    return -LX_EINVAL;
  if (gmem_guest_export_copyin(NULL, waiters_addr, pair, sizeof pair) != 0)
    return -LX_EFAULT;
  for (int i = 0; i < 2; ++i) {
    if (pair[i].reserved || pair[i].val > UINT32_MAX ||
        pair[i].flags != (FU2_SIZE_U32 | FU2_PRIVATE)) return -LX_EINVAL;
    if (pair[i].uaddr & (sizeof(uint32_t) - 1)) return -LX_EINVAL;
    if (!pair[i].uaddr) return -LX_EFAULT;
    uint32_t snapshot;
    if (gmem_guest_export_copyin(NULL, pair[i].uaddr, &snapshot, sizeof snapshot) != 0)
      return -LX_EFAULT;
  }
  if (nr_requeue) return -95; // EOPNOTSUPP: moving sleeping waiters is not implemented.

  unsigned long source = (unsigned long)pair[0].uaddr;
  struct fu_bucket* b = fu_bucket(source);
  pthread_mutex_lock(&b->mu);
  if (*(volatile uint32_t*)source != (uint32_t)pair[0].val) {
    pthread_mutex_unlock(&b->mu);
    return -LX_EAGAIN;
  }
  int woke = 0;
  for (struct fu_waiter* w = b->head; w && woke < nr_wake; w = w->next)
    if (w->uaddr == source && !w->woken) { w->woken = 1; ++woke; }
  if (woke) pthread_cond_broadcast(&b->cv);
  pthread_mutex_unlock(&b->mu);
  return woke;
}

// ===========================================================================================
// Virtual standard signals. Linux signal masks are still process-wide in this shim;
// real per-thread delivery and realtime queues need separate work. A blocked self-signal
// or parent-death signal can be consumed by rt_sigtimedwait without touching host PIDs.
// ===========================================================================================

static struct linux_sigaction g_sigacts[65];
static uint64_t g_sigmask;
static uint64_t g_sigpending;
static int g_pdeathsig;
static pid_t g_pdeath_host_parent;
static int g_pdeath_fired;
static uint64_t g_pdeath_generation;
static int g_pdeath_watch_fd = -1;
static pid_t g_sigchld_notified_pid;
static void virtual_signal_check_parent(void);
static void virtual_signal_deliver_unblocked(void);

struct pdeath_watch {
  int fd, signo;
  pid_t parent;
  uint64_t generation;
  int ready;
};

static void virtual_signal_default(int signo) {
  // Custom guest handlers are rejected for a parent-death signal below. SIG_IGN
  // discards generation; default disposition is delegated to the real host signal.
  uint64_t action = g_sigacts[signo].handler;
  if (action == 1) return;
  int host = sig_l2m(signo);
  if (host > 0) {
    struct sigaction sa = {.sa_handler = SIG_DFL};
    sigemptyset(&sa.sa_mask);
    sigaction(host, &sa, NULL);
    sigset_t unblocked;
    sigemptyset(&unblocked);
    sigaddset(&unblocked, host);
    pthread_sigmask(SIG_UNBLOCK, &unblocked, NULL);
    raise(host);
  }
}
static void virtual_signal_emit_parent(int signo, uint64_t generation) {
  if (__atomic_load_n(&g_pdeath_generation, __ATOMIC_ACQUIRE) != generation ||
      !__sync_bool_compare_and_swap(&g_pdeath_fired, 0, 1)) return;
  if (g_sigacts[signo].handler == 1) return; // SIG_IGN discards at generation
  uint64_t bit = 1ULL << (signo - 1);
  if (__atomic_load_n(&g_sigmask, __ATOMIC_ACQUIRE) & bit) {
    if (!signalfd_shim_queue_linux(signo))
      __atomic_fetch_or(&g_sigpending, bit, __ATOMIC_ACQ_REL);
  }
  else virtual_signal_default(signo);
}
static void* virtual_signal_parent_watch(void* arg) {
  struct pdeath_watch* watch = arg;
  while (!__atomic_load_n(&watch->ready, __ATOMIC_ACQUIRE)) {
    struct timespec brief = {0, 1000000};
    nanosleep(&brief, NULL);
  }
  struct kevent event;
  struct timespec timeout = {0, 250000000};
  for (;;) {
    if (__atomic_load_n(&g_pdeath_generation, __ATOMIC_ACQUIRE) != watch->generation) break;
    int n = kevent(watch->fd, NULL, 0, &event, 1, &timeout);
    if (n == 1 && (event.fflags & NOTE_EXIT)) {
      virtual_signal_emit_parent(watch->signo, watch->generation);
      break;
    }
    if (n < 0 && errno != EINTR) break;
    if (getppid() != watch->parent) {
      virtual_signal_emit_parent(watch->signo, watch->generation);
      break;
    }
  }
  if (__atomic_load_n(&g_pdeath_watch_fd, __ATOMIC_ACQUIRE) == watch->fd)
    __sync_bool_compare_and_swap(&g_pdeath_watch_fd, watch->fd, -1);
  shim_fd_unreserve(watch->fd);
  close(watch->fd);
  free(watch);
  return NULL;
}
static long virtual_signal_arm_parent(int signo, pid_t parent) {
  int fd = kqueue();
  if (fd < 0) return -linux_errno_from_host(errno);
  fd = shim_fd_reserve(fd);
  struct kevent event;
  EV_SET(&event, parent, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, NULL);
  if (kevent(fd, &event, 1, NULL, 0, NULL) < 0) {
    int err = linux_errno_from_host(errno);
    shim_fd_unreserve(fd);
    close(fd);
    return -err;
  }
  struct pdeath_watch* watch = calloc(1, sizeof *watch);
  if (!watch) { shim_fd_unreserve(fd); close(fd); return -LX_ENOMEM; }
  watch->fd = fd; watch->signo = signo; watch->parent = parent;
  pthread_t thread;
  int rc = pthread_create(&thread, NULL, virtual_signal_parent_watch, watch);
  if (rc) {
    shim_fd_unreserve(fd); close(fd); free(watch);
    return -linux_errno_from_host(rc);
  }
  watch->generation = __atomic_add_fetch(&g_pdeath_generation, 1, __ATOMIC_ACQ_REL);
  g_pdeathsig = signo;
  g_pdeath_host_parent = parent;
  g_pdeath_fired = 0;
  __atomic_store_n(&g_pdeath_watch_fd, fd, __ATOMIC_RELEASE);
  __atomic_store_n(&watch->ready, 1, __ATOMIC_RELEASE);
  pthread_detach(thread);
  return 0;
}
long syscalls_signal_set_pdeathsig(int linux_signo) {
  if (linux_signo < 0 || linux_signo > 64) return -LX_EINVAL;
  if (linux_signo && sig_l2m(linux_signo) < 0) return -LX_ENOSYS;
  if (linux_signo && g_sigacts[linux_signo].handler > 1) return -LX_ENOSYS;
  if (!linux_signo) {
    __atomic_add_fetch(&g_pdeath_generation, 1, __ATOMIC_ACQ_REL);
    g_pdeathsig = 0; g_pdeath_host_parent = 0; g_pdeath_fired = 0;
    return 0;
  }
  return virtual_signal_arm_parent(linux_signo, getppid());
}
int syscalls_signal_get_pdeathsig(void) { return g_pdeathsig; }
void syscalls_signal_after_fork(void) {
  // Linux inherits the mask but not pending signals or PR_SET_PDEATHSIG.
  __atomic_store_n(&g_sigpending, 0, __ATOMIC_RELEASE);
  g_pdeathsig = 0;
  g_pdeath_host_parent = 0;
  g_pdeath_fired = 0;
  g_pdeath_generation++;
  // The worker thread did not survive fork. Remove its inherited kqueue from the
  // child-only descriptor registry without touching a possibly locked host mutex.
  int fd = g_pdeath_watch_fd;
  if (fd >= 0) {
    for (int i = 0; i < g_shim_nfd; i++) if (g_shim_fds[i] == fd) {
      g_shim_fds[i] = g_shim_fds[--g_shim_nfd]; break;
    }
    close(fd);
  }
  g_pdeath_watch_fd = -1;
  g_sigchld_notified_pid = 0;
}
void syscalls_signal_child_reaped(long pid) {
  if (pid > 0 && g_sigchld_notified_pid == (pid_t)pid)
    g_sigchld_notified_pid = 0;
}
long syscalls_signal_sigsuspend(unsigned long long mask) {
  uint64_t temporary = (uint64_t)mask &
      ~((1ULL << (9 - 1)) | (1ULL << (19 - 1)));
  uint64_t previous = __atomic_exchange_n(&g_sigmask, temporary, __ATOMIC_ACQ_REL);
  for (;;) {
    virtual_signal_check_parent();
    virtual_signal_deliver_unblocked();
    // A caught, newly exited guest child is a real SIGCHLD wake source. WNOWAIT
    // leaves it for the caller's waitpid() loop to reap after EINTR. No handler
    // is synthesized here; callers that need handler side effects remain outside
    // the current signal compatibility contract.
    if (!(temporary & (1ULL << (17 - 1))) && g_sigacts[17].handler > 1) {
      siginfo_t si = {0};
      if (waitid(P_ALL, 0, &si, WEXITED | WNOHANG | WNOWAIT) == 0 &&
          si.si_pid > 0 && si.si_pid != g_sigchld_notified_pid &&
          guest_child_known((int)si.si_pid)) {
        g_sigchld_notified_pid = si.si_pid;
        __atomic_store_n(&g_sigmask, previous, __ATOMIC_RELEASE);
        return -LX_EINTR;
      }
    }
    struct timespec pause = {0, 100000000};
    nanosleep(&pause, NULL);
  }
}
void syscalls_signal_exec_snapshot(char* out, size_t cap, int forked_child) {
  snprintf(out, cap, "%016llx,%016llx,%d,%ld",
           (unsigned long long)__atomic_load_n(&g_sigmask, __ATOMIC_ACQUIRE),
           (unsigned long long)(forked_child ? 0 : __atomic_load_n(&g_sigpending, __ATOMIC_ACQUIRE)),
           forked_child ? 0 : g_pdeathsig,
           forked_child ? 0L : (long)g_pdeath_host_parent);
}
void syscalls_signal_exec_restore(const char* snapshot) {
  if (!snapshot) return;
  unsigned long long mask, pending;
  int signo; long parent;
  if (sscanf(snapshot, "%llx,%llx,%d,%ld", &mask, &pending, &signo, &parent) != 4) return;
  mask &= ~((1ULL << (9 - 1)) | (1ULL << (19 - 1)));
  __atomic_store_n(&g_sigmask, (uint64_t)mask, __ATOMIC_RELEASE);
  __atomic_store_n(&g_sigpending, (uint64_t)pending, __ATOMIC_RELEASE);
  if (signo > 0 && signo <= 31 && parent > 0 && g_sigacts[signo].handler <= 1) {
    if (getppid() != (pid_t)parent) {
      g_pdeathsig = signo; g_pdeath_host_parent = (pid_t)parent;
      virtual_signal_emit_parent(signo, g_pdeath_generation);
    } else if (virtual_signal_arm_parent(signo, (pid_t)parent) < 0) {
      // exec has replaced the original image. Continuing without its promised
      // parent-death notification would silently weaken process cleanup.
      fprintf(stderr, "[shim] cannot restore parent-death signal after exec\n");
      _exit(127);
    }
  }
}
static void virtual_signal_check_parent(void) {
  int signo = g_pdeathsig;
  if (signo && getppid() != g_pdeath_host_parent)
    virtual_signal_emit_parent(signo, g_pdeath_generation);
}
static void virtual_signal_deliver_unblocked(void) {
  for (;;) {
    uint64_t pending = __atomic_load_n(&g_sigpending, __ATOMIC_ACQUIRE);
    uint64_t mask = __atomic_load_n(&g_sigmask, __ATOMIC_ACQUIRE);
    uint64_t ready = pending & ~mask;
    if (!ready) return;
    uint64_t bit = ready & (~ready + 1);
    uint64_t old = pending;
    if (__atomic_compare_exchange_n(&g_sigpending, &old, pending & ~bit, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      virtual_signal_default(__builtin_ctzll(bit) + 1);
  }
}
unsigned long long syscalls_signal_pending(void) {
  virtual_signal_check_parent();
  return __atomic_load_n(&g_sigpending, __ATOMIC_ACQUIRE);
}
static int virtual_signal_take(uint64_t wanted) {
  for (;;) {
    uint64_t old = __atomic_load_n(&g_sigpending, __ATOMIC_ACQUIRE);
    uint64_t hit = old & wanted;
    if (!hit) return 0;
    uint64_t bit = hit & (~hit + 1);
    if (__atomic_compare_exchange_n(&g_sigpending, &old, old & ~bit, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return __builtin_ctzll(bit) + 1;
  }
}

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
    case L_FIONREAD:  { int n = 0; if (!arg) return -LX_EFAULT;
                        if (gmem_guest_range_writable(arg, sizeof n) != 0) return -LX_EFAULT;
                        if (ioctl(fd, FIONREAD, &n) < 0) return herrno();
                        return gmem_guest_copyout_checked(arg, &n, sizeof n) == 0
                                   ? 0 : -LX_EFAULT; }
    case L_FIONBIO:   { int on = *(const int*)arg; return herr(ioctl(fd, FIONBIO, &on)); }
    case L_FIOCLEX:   return herr(fcntl(fd, F_SETFD, FD_CLOEXEC));
    case L_FIONCLEX:  return herr(fcntl(fd, F_SETFD, 0));
    case L_TIOCGPGRP: { int virtual_p = 0; pid_t p = vt_get_foreground(fd, &virtual_p) == 0 ? (pid_t)virtual_p : tcgetpgrp(fd);
                        if (p < 0) return herrno();
                        *(int*)arg = (int)p; return 0; }
    case L_TIOCSPGRP: { int want=*(const int*)arg; int rc=vt_owns_fd(fd) ? vt_set_foreground(fd,want) : tcsetpgrp(fd,(pid_t)want);
                        return herr(rc); }
    case L_TIOCGSID:  { int virtual_s = 0; pid_t s = vt_get_session(fd, &virtual_s) == 0 ? (pid_t)virtual_s : getsid(0); if (s < 0) return herrno();
                        *(int*)arg = (int)s; return 0; }
    case L_TIOCEXCL:  return herr(ioctl(fd, TIOCEXCL));
    case L_TIOCNXCL:  return herr(ioctl(fd, TIOCNXCL));
    case L_TIOCSCTTY: { int rc=vt_owns_fd(fd) ? vt_claim_ctty(fd) : ioctl(fd,TIOCSCTTY,0);
                        return herr(rc); }
    case L_TIOCNOTTY: return herr(ioctl(fd, TIOCNOTTY));
    case L_TIOCOUTQ: {
      if (!arg) return -LX_EFAULT;
      int n = 0;
      if (ioctl(fd, TIOCOUTQ, &n) < 0) {
        int saved = errno;
        // Darwin rejects this on AF_UNIX sockets. Inspect the kernel peer's
        // receive-buffer charge, which includes queued SCM_RIGHTS descriptors.
        // Fail closed when the peer cannot be identified or inspected.
        const char* outq_mode = getenv("RDSOCKETOUTQ_COMPAT");
        if (saved != EOPNOTSUPP ||
            !(outq_mode ? *outq_mode == '1' : getenv("RDNATIVE_SESSION") != NULL))
          return -(long)linux_errno_from_host(saved);
        if (rd_unix_peer_outq(fd, &n) < 0) return herrno();
      }
      *(int*)arg = n;
      return 0;
    }
    // Current Linux PIDFD_GET_INFO uses _IOWR(0xFF, 11, struct pidfd_info) with an
    // 88-byte layout. Older builds used the 124-byte command below. Both must answer
    // from the same pidfd registry; forwarding either to Darwin returns ENOTTY.
    case 0xC058FF0B: {
      if (!arg) return -LX_EFAULT;
      pid_t pfd = pidfd_shim_pid(fd);
      if (pfd <= 0) return -LX_EBADF;
      long rp, rpp;
      proc_pid_identity((long)pfd, 0, &rp, &rpp);
      struct rd_pidfd_info_v2 {
        uint64_t mask, cgroupid;
        uint32_t pid, tgid, ppid;
        uint32_t ruid, rgid, euid, egid, suid, sgid, fsuid, fsgid;
        int32_t exit_code;
        uint32_t coredump_mask, coredump_signal, coredump_code, coredump_pad;
        uint64_t supported_mask;
      } *inf = (struct rd_pidfd_info_v2*)arg;
      inf->mask = RD_PIDFD_INFO_PID | RD_PIDFD_INFO_CREDS | RD_PIDFD_INFO_CGROUPID;
      inf->cgroupid = 0;
      inf->pid = inf->tgid = (uint32_t)rp;
      inf->ppid = (uint32_t)rpp;
      inf->ruid = inf->rgid = inf->euid = inf->egid = 0;
      inf->suid = inf->sgid = inf->fsuid = inf->fsgid = 0;
      inf->exit_code = 0;
      inf->coredump_mask = inf->coredump_signal = inf->coredump_code = inf->coredump_pad = 0;
      inf->supported_mask = 0;
      return 0;
    }
    // PIDFD_GET_INFO (Linux 6.13+, _IOWR(0xFF, 9, struct pidfd_info) == 0xC07CFF09). systemd 261's
    // pidfd_get_ppid()/pidfd_get_pid() issue this on a pidfd and read .pid/.ppid back; without it
    // they fall back to /proc/self/fdinfo/<fd>. Emulate it so pidref_verify() has a direct answer.
    case 0xC07CFF09: {
      if (!arg) return -LX_EFAULT;
      pid_t pfd = pidfd_shim_pid(fd);
      if (pfd <= 0) return -LX_EBADF;              // not a pidfd we issued (or stale sentinel)
      long rp, rpp;
      proc_pid_identity((long)pfd, 0, &rp, &rpp);
      struct rd_pidfd_info* inf = (struct rd_pidfd_info*)arg;
      uint64_t want = inf->mask;
      inf->mask = want | RD_PIDFD_INFO_PID | RD_PIDFD_INFO_CREDS;
      inf->pid  = (uint32_t)rp;
      inf->ppid = (uint32_t)rpp;
      inf->ruid = inf->rgid = inf->euid = inf->egid = 0;
      inf->suid = inf->sgid = inf->fsuid = inf->fsgid = 0;
      inf->cgroupid = 0;
      return 0;
    }
    default:
      if (snd_owns_fd(fd)) return snd_ioctl(fd, req, arg);
      if (drm_owns_fd(fd)) return drm_ioctl(fd, req, arg);
      if (cam_owns_fd(fd)) return cam_ioctl(fd, req, arg);
      if (evdev_owns_fd(fd)) return evdev_ioctl(fd, req, arg);
      if (vt_owns_fd(fd)) return vt_ioctl(fd, req, arg);   // KDGKBMODE/KDGKBTYPE/VT_*
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
#define LF_OFD_GETLK      36
#define LF_OFD_SETLK      37
#define LF_OFD_SETLKW     38
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
      int allowed = inotify_watch_can_dup(fd, -1);
      if (allowed < 0) return allowed;
      int nf = fcntl(fd, cmd == LF_DUPFD ? F_DUPFD : F_DUPFD_CLOEXEC, (int)arg);
      if (nf >= 0) inotify_watch_dup(fd, nf);
      if (nf >= 0) rd_rtnl_dup(fd, nf);
      if (nf >= 0 && pidfd_shim_pid(fd) > 0) pidfd_shim_register(nf, pidfd_shim_pid(fd));
      if (nf >= 0 && drm_owns_fd(fd)) drm_register_fd_copy(nf, fd);
      if (nf >= 0 && snd_owns_fd(fd)) snd_dup_fd(fd, nf);    // a dup of a snd node keeps its role
      if (nf >= 0 && evdev_owns_fd(fd)) evdev_register_fd(nf, evdev_device_of_fd(fd));
      return herr(nf);
    }
    case LF_GETFD:         return herr(fcntl(fd, F_GETFD));
    case LF_SETFD:         return herr(fcntl(fd, F_SETFD, (int)arg));
    case LF_GETFL: {
      if (net_opath_socket_is(fd)) return LO_PATH;
      int r = fcntl(fd, F_GETFL);
      return r < 0 ? herrno() : open_flags_m2l(r);
    }
    case LF_SETFL: {
      if (net_opath_socket_is(fd)) return -LX_EBADF;
      int mf = 0;
      if (arg & LO_NONBLOCK) mf |= O_NONBLOCK;
      if (arg & LO_APPEND)   mf |= O_APPEND;
      if (arg & LO_ASYNC)    mf |= O_ASYNC;
      return herr(fcntl(fd, F_SETFL, mf));
    }
    case LF_GETLK: case LF_SETLK: case LF_SETLKW:
    case LF_OFD_GETLK: case LF_OFD_SETLK: case LF_OFD_SETLKW: {
      struct linux_flock lf;
      if (!arg || gmem_guest_export_copyin(NULL, arg, &lf, sizeof lf) != 0)
        return -LX_EFAULT;
      const int query = cmd == LF_GETLK || cmd == LF_OFD_GETLK;
      if (query && gmem_guest_range_writable(arg, sizeof lf) != 0)
        return -LX_EFAULT;
      // Linux OFD locks belong to the open file description rather than a PID.
      // macOS provides matching F_OFD_* operations; do not map these to POSIX
      // process locks, which would release at different times after dup/fork.
      const int ofd = cmd == LF_OFD_GETLK || cmd == LF_OFD_SETLK || cmd == LF_OFD_SETLKW;
      if (ofd && lf.l_pid != 0) return -LX_EINVAL;
      if (lf.l_type != 0 && lf.l_type != 1 && lf.l_type != 2) return -LX_EINVAL;
      struct flock mf;
      memset(&mf, 0, sizeof mf);
      mf.l_start  = lf.l_start;
      mf.l_len    = lf.l_len;
      mf.l_pid    = lf.l_pid;
      mf.l_type   = (short)flock_type_l2m(lf.l_type);
      mf.l_whence = lf.l_whence;
      int mcmd;
      switch (cmd) {
        case LF_GETLK: mcmd = F_GETLK; break;
        case LF_SETLK: mcmd = F_SETLK; break;
        case LF_SETLKW: mcmd = F_SETLKW; break;
        case LF_OFD_GETLK: mcmd = F_OFD_GETLK; break;
        case LF_OFD_SETLK: mcmd = F_OFD_SETLK; break;
        default: mcmd = F_OFD_SETLKW; break;
      }
      if (fcntl(fd, mcmd, &mf) < 0) return herrno();
      if (query) {
        lf.l_type   = (int16_t)flock_type_m2l(mf.l_type);
        lf.l_whence = (int16_t)mf.l_whence;
        lf.l_start  = mf.l_start;
        lf.l_len    = mf.l_len;
        lf.l_pid    = mf.l_pid;
        if (gmem_guest_copyout_checked(arg, &lf, sizeof lf) != 0)
          return -LX_EFAULT;
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

static long readlink_guest_path(uint64_t address, char* out, size_t cap) {
  if (!address) return -LX_EFAULT;
  for (size_t i = 0; i < cap; ++i) {
    if (gmem_guest_export_copyin(NULL, address + i, out + i, 1) != 0)
      return -LX_EFAULT;
    if (!out[i]) return 0;
  }
  return -LX_ENAMETOOLONG;
}

static long readlink_guest_result(uint64_t address, size_t cap,
                                  const char* bytes, size_t length) {
  if (length > cap) length = cap;
  if (length && gmem_guest_copyout_checked(address, bytes, length) != 0)
    return -LX_EFAULT;
  return (long)length; // readlink does not append a NUL
}

static long readlink_guest_text(uint64_t address, size_t cap, const char* text) {
  return readlink_guest_result(address, cap, text, strlen(text));
}

// ===========================================================================================
// Synthesized /proc + /sys + /dev directory enumeration
// ===========================================================================================
//
// getdents64 on /proc, /sys, /dev (and their synthetic subdirs) must NOT fall through to the host
// readdir: the rootfs dirs ship empty (or hold placeholder regular files), so `ls /proc`, lscpu,
// libudev's udev_enumerate_scan_devices, and systemd all enumerate nothing. The leaf files these
// tools then read are already served one path at a time (proc_open, drmasahi.c, snd_alsa.c,
// evdev.c, vt.c); what is missing is the DIRECTORY STRUCTURE that lets the tools find them. These
// tables are that structure, and do_getdents64() serves them instead of the host readdir.

struct synth_ent { const char* name; uint8_t type; };

// Static listings. DT_* numbering is identical on Linux and Darwin, so the dirent.h constants are
// used verbatim. DT_LNK marks /proc magic links; DT_CHR/DT_BLK the synthetic device nodes; DT_DIR
// the class/subdir entries (which stat as S_IFDIR via the *_fake_stat handlers).

static const struct synth_ent PROC_PID_ENTS[] = {       // /proc/self and /proc/<pid>
  { "auxv",    DT_REG },
  { "cmdline", DT_REG },
  { "cwd",     DT_LNK },
  { "environ", DT_REG },
  { "exe",     DT_LNK },
  { "fd",      DT_DIR },
  { "maps",    DT_REG },
  { "mounts",  DT_REG },
  { "root",    DT_LNK },
  { "stat",    DT_REG },
  { "status",  DT_REG },
};

static const struct synth_ent PROC_SYS_ENTS[] = {       // /proc/sys
  { "kernel", DT_DIR },
  { "net",    DT_DIR },
  { "vm",     DT_DIR },
};

static const struct synth_ent SYS_CLASS_ENTS[] = {      // /sys/class
  { "block", DT_DIR },
  { "drm",   DT_DIR },
  { "input", DT_DIR },
  { "sound", DT_DIR },
};

static const struct synth_ent SYS_CLASS_DRM_ENTS[] = {  // /sys/class/drm
  { "card0",      DT_DIR },
  { "renderD128", DT_DIR },
};

static const struct synth_ent SYS_CLASS_DRM_NODE_ENTS[] = {
  { "device", DT_DIR }, { "dev", DT_REG }, { "uevent", DT_REG },
  { "subsystem", DT_LNK },
};

static const struct synth_ent SYS_CLASS_DRM_DEVICE_ENTS[] = {
  { "drm", DT_CHR }, { "subsystem", DT_LNK }, { "uevent", DT_REG },
  { "vendor", DT_REG }, { "device", DT_REG }, { "revision", DT_REG },
  { "subsystem_vendor", DT_REG }, { "subsystem_device", DT_REG },
};

static const struct synth_ent SYS_CLASS_SOUND_ENTS[] = { // /sys/class/sound
  { "card0", DT_DIR },
};

static const struct synth_ent SYS_CLASS_INPUT_ENTS[] = { // /sys/class/input
  { "event0", DT_DIR },
  { "event1", DT_DIR },
};

static const struct synth_ent SYS_CLASS_BLOCK_ENTS[] = { // /sys/class/block
  { "sda", DT_DIR },
};

static const struct synth_ent SYS_DEVICES_ENTS[] = {    // /sys/devices
  { "system", DT_DIR },
};

static const struct synth_ent SYS_DEVICES_SYSTEM_ENTS[] = { // /sys/devices/system
  { "cpu", DT_DIR },
};

static const struct synth_ent DEV_ENTS[] = {            // /dev
  { "console", DT_CHR },
  { "dri",     DT_DIR },
  { "fd",      DT_LNK },
  { "full",    DT_CHR },
  { "input",   DT_DIR },
  { "null",    DT_CHR },
  { "ptmx",    DT_CHR },
  { "pts",     DT_DIR },
  { "random",  DT_CHR },
  { "sda",     DT_BLK },
  { "shm",     DT_DIR },
  { "snd",     DT_DIR },
  { "stdin",   DT_LNK },
  { "stdout",  DT_LNK },
  { "stderr",  DT_LNK },
  { "tty",     DT_CHR },
  { "tty0",    DT_CHR },
  { "tty1",    DT_CHR },
  { "tty2",    DT_CHR },
  { "tty3",    DT_CHR },
  { "tty4",    DT_CHR },
  { "tty5",    DT_CHR },
  { "tty6",    DT_CHR },
  { "urandom", DT_CHR },
  { "zero",    DT_CHR },
};

static const struct synth_ent DEV_DRI_ENTS[] = {        // /dev/dri
  { "card0",      DT_CHR },
  { "renderD128", DT_CHR },
};

static const struct synth_ent DEV_INPUT_ENTS[] = {      // /dev/input
  { "event0", DT_CHR },
  { "event1", DT_CHR },
};

static const struct synth_ent DEV_SND_ENTS[] = {        // /dev/snd
  { "controlC0", DT_CHR },
  { "pcmC0D0c",  DT_CHR },
  { "pcmC0D0p",  DT_CHR },
  { "pcmC0D1c",  DT_CHR },
  { "pcmC0D1p",  DT_CHR },
};

// Dynamic listings (rebuilt on each lookup into static buffers).

static struct synth_ent g_proc_ents[64];
static char           g_proc_names[64][32];

static int proc_top_entries(struct synth_ent* out) {
  int n = 0;
  #define ADD(nm, ty) do { snprintf(g_proc_names[n], sizeof g_proc_names[n], "%s", nm); \
                           out[n].name = g_proc_names[n]; out[n].type = (ty); n++; } while (0)
  ADD("self", DT_LNK);
  ADD("1", DT_DIR);
  int self = (int)getpid(), parent = (int)getppid();
  if (self != 1)    { char b[32]; snprintf(b, sizeof b, "%d", self);    ADD(b, DT_DIR); }
  if (parent > 1 && parent != self) { char b[32]; snprintf(b, sizeof b, "%d", parent); ADD(b, DT_DIR); }
  ADD("cpuinfo", DT_REG);
  ADD("loadavg", DT_REG);
  ADD("meminfo", DT_REG);
  ADD("stat",    DT_REG);
  ADD("sys",     DT_DIR);
  ADD("uptime",  DT_REG);
  ADD("version", DT_REG);
  #undef ADD
  return n;
}

static struct synth_ent g_cpu_ents[64];
static char           g_cpu_names[64][16];

static int sys_cpu_entries(struct synth_ent* out) {
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpu < 1)  ncpu = 1;
  if (ncpu > 32) ncpu = 32;
  int n = 0;
  // The CPU-set leaves come first; lscpu reads these before it ever descends into cpu0..cpuN.
  static const char* const leaves[] = { "possible", "online", "present", "offline" };
  for (size_t i = 0; i < sizeof leaves / sizeof leaves[0]; i++) {
    snprintf(g_cpu_names[n], sizeof g_cpu_names[n], "%s", leaves[i]);
    out[n].name = g_cpu_names[n];
    out[n].type = DT_REG;
    n++;
  }
  for (long i = 0; i < ncpu; i++) {
    snprintf(g_cpu_names[n], sizeof g_cpu_names[n], "cpu%ld", i);
    out[n].name = g_cpu_names[n];
    out[n].type = DT_DIR;
    n++;
  }
  return n;
}

// /proc/self/fd (and /proc/<pid>/fd) — the open-fd table. systemd's fdset_new_fill() enumerates
// this directory to build its fd set at boot; with no listing it fails ("Failed to allocate fd
// set") and the boot freezes. The guest shares the host fd table, so the honest listing is the set
// of host fds currently open (F_GETFD < 0 == closed).
static struct synth_ent g_fd_ents[512];
static char           g_fd_names[512][16];

static int proc_self_fd_entries(struct synth_ent* out) {
  int n = 0;
  int maxfd = (int)getdtablesize();
  if (maxfd > 512) maxfd = 512;
  for (int fd = 0; fd < maxfd && n < 512; fd++) {
    // Internal descriptors are not guest fds: do not expose them to systemd
    // CLOEXEC sweeps or activation-fd serialization.
    if (shim_fd_reserved(fd) || fcntl(fd, F_GETFD) < 0) continue;
    snprintf(g_fd_names[n], sizeof g_fd_names[n], "%d", fd);
    out[n].name = g_fd_names[n];
    out[n].type = DT_LNK;                              // /proc/self/fd/<N> is a magic symlink
    n++;
  }
  return n;
}

// ===========================================================================================
// cgroup v2 (/sys/fs/cgroup) synthesis — an in-memory tree, never the rootfs
// ===========================================================================================
//
// systemd-as-PID-1 mounts cgroup2 (the mount() is a no-op success) and then reads the root
// cgroup's cgroup.controllers / cgroup.procs and creates child cgroups (init.scope, system.slice,
// user.slice, getty@tty1.service, ...) as it starts units. The rootfs's /sys/fs/cgroup ships
// EMPTY, so the whole tree is synthesized here:
//
//   * the DIRECTORY STRUCTURE is a small in-memory registry — mkdir/rmdir/renameat under
//     /sys/fs/cgroup mutate it and never touch the rootfs;
//   * the LEAF FILES (cgroup.controllers, cgroup.procs, memory.max, ...) are served as mem_fd
//     content one path at a time, exactly like /proc's proc_open().

#define CGROUP_PREFIX "/sys/fs/cgroup"
#define CGROUP_PREFIX_LEN 14          // strlen("/sys/fs/cgroup")

// Leaf-file basenames a cgroup directory can hold: the generic v2 control files first, then the
// per-controller interface files systemd touches during boot (cpu, cpuset, memory, pids, io).
static const char* const CGROUP_FILES[] = {
  "cgroup.controllers", "cgroup.subtree_control", "cgroup.procs", "cgroup.events",
  "cgroup.max.depth", "cgroup.max.descendants", "cgroup.stat", "cgroup.freeze",
  "cgroup.type", "cgroup.threads", "cgroup.kill",
  "cpu.max", "cpu.weight", "cpu.stat", "cpu.pressure",
  "cpuset.cpus", "cpuset.mems", "cpuset.cpus.effective", "cpuset.mems.effective",
  "io.max", "io.stat", "io.pressure",
  "memory.current", "memory.max", "memory.min", "memory.low", "memory.high",
  "memory.swap.max", "memory.events", "memory.stat", "memory.pressure",
  "pids.current", "pids.max", "pids.events",
};
#define CGROUP_NFILES ((int)(sizeof(CGROUP_FILES)/sizeof(CGROUP_FILES[0])))

// In-memory registry of child cgroup directories (full guest paths). The root ("/sys/fs/cgroup")
// always exists and is not stored here.
#define CGROUP_MAX 128
static char g_cgroup_dirs[CGROUP_MAX][GPATH_MAX];
static int  g_cgroup_ndirs;

static int cgroup_is_root(const char* g) { return strcmp(g, CGROUP_PREFIX) == 0; }
static int cgroup_under(const char* g) {
  if (!g) return 0;
  return strncmp(g, CGROUP_PREFIX, CGROUP_PREFIX_LEN) == 0 &&
         (g[CGROUP_PREFIX_LEN] == '/' || g[CGROUP_PREFIX_LEN] == 0);
}

// basename of a cgroup leaf file, or NULL if `g` is not one of the synthesized files.
static const char* cgroup_file_name(const char* g) {
  if (!cgroup_under(g)) return 0;
  const char* base = strrchr(g, '/');
  if (!base) return 0;
  base++;
  for (int i = 0; i < CGROUP_NFILES; i++)
    if (strcmp(base, CGROUP_FILES[i]) == 0) return base;
  return 0;
}

// True if `g` names an existing cgroup directory (root or a registered child / descendant).
static int cgroup_is_dir(const char* g) {
  if (cgroup_is_root(g)) return 1;
  for (int i = 0; i < g_cgroup_ndirs; i++) {
    size_t dl = strlen(g_cgroup_dirs[i]);
    if (strncmp(g, g_cgroup_dirs[i], dl) == 0 && (g[dl] == '/' || g[dl] == 0)) return 1;
  }
  return 0;
}

// The parent directory of `g`, as a guest path ("" for none). Always returns root for the root.
static const char* cgroup_parent(const char* g, char* out, size_t cap) {
  size_t gl = strlen(g);
  while (gl > 1 && g[gl - 1] == '/') gl--;
  const char* s = g + gl - 1;
  while (s > g && *s != '/') s--;
  if (s == g) { snprintf(out, cap, "/"); return out; }     // "/sys/fs/cgroup/x" -> parent below
  size_t pl = (size_t)(s - g);
  if (pl + 1 > cap) return 0;
  memcpy(out, g, pl); out[pl] = 0;
  return out;
}

static int cgroup_register(const char* g) {
  if (cgroup_is_root(g)) return 0;
  for (int i = 0; i < g_cgroup_ndirs; i++)
    if (strcmp(g_cgroup_dirs[i], g) == 0) return 0;         // already there (EEXIST is the caller's)
  if (g_cgroup_ndirs >= CGROUP_MAX) return -1;
  snprintf(g_cgroup_dirs[g_cgroup_ndirs], GPATH_MAX, "%s", g);
  g_cgroup_ndirs++;
  return 0;
}

// Remove `g` and every descendant. Returns 1 if anything was removed.
static int cgroup_unregister(const char* g) {
  int removed = 0;
  size_t gl = strlen(g);
  for (int i = 0; i < g_cgroup_ndirs; ) {
    if (strncmp(g_cgroup_dirs[i], g, gl) == 0 &&
        (g_cgroup_dirs[i][gl] == '/' || g_cgroup_dirs[i][gl] == 0)) {
      for (int j = i; j < g_cgroup_ndirs - 1; j++)
        snprintf(g_cgroup_dirs[j], GPATH_MAX, "%s", g_cgroup_dirs[j + 1]);
      g_cgroup_ndirs--;
      removed = 1;
    } else {
      i++;
    }
  }
  return removed;
}

// content of a cgroup leaf file, or NULL. Written into the caller's `buf` (a static is NOT used
// because the value is handed straight to mem_fd and must survive across cgroup_open calls).
const char* cgroup_file_content(const char* g, char* buf, size_t cap) {
  const char* base = cgroup_file_name(g);
  if (!base) return 0;
  if (strcmp(base, "cgroup.controllers") == 0)
    snprintf(buf, cap, "cpuset cpu io memory hugetlb pids\n");
  else if (strcmp(base, "cgroup.subtree_control") == 0)
    snprintf(buf, cap, "%s", "");                          // controllers enabled in children: none yet
  else if (strcmp(base, "cgroup.procs") == 0)
    snprintf(buf, cap, "%s", cgroup_is_root(g) ? "1\n" : "");
  else if (strcmp(base, "cgroup.events") == 0)
    snprintf(buf, cap, "populated 0\nfrozen 0\n");
  else if (strcmp(base, "cgroup.max.depth") == 0)
    snprintf(buf, cap, "max\n");
  else if (strcmp(base, "cgroup.max.descendants") == 0)
    snprintf(buf, cap, "max\n");
  else if (strcmp(base, "cgroup.stat") == 0)
    snprintf(buf, cap, "nr_descendants 0\nnr_dying_descendants 0\n");
  else if (strcmp(base, "cgroup.freeze") == 0)
    snprintf(buf, cap, "0\n");
  else if (strcmp(base, "cgroup.type") == 0)
    snprintf(buf, cap, "domain\n");
  else if (strcmp(base, "cgroup.threads") == 0 || strcmp(base, "cgroup.kill") == 0)
    snprintf(buf, cap, "%s", "");
  else if (strcmp(base, "memory.current") == 0 || strcmp(base, "memory.min") == 0 ||
           strcmp(base, "memory.low") == 0 || strcmp(base, "memory.swap.current") == 0)
    snprintf(buf, cap, "0\n");
  else if (strcmp(base, "memory.max") == 0 || strcmp(base, "memory.high") == 0 ||
           strcmp(base, "memory.swap.max") == 0)
    snprintf(buf, cap, "max\n");
  else if (strcmp(base, "memory.events") == 0)
    snprintf(buf, cap, "low 0\nhigh 0\nmax 0\noom 0\noom_kill 0\n");
  else if (strcmp(base, "memory.stat") == 0)
    snprintf(buf, cap, "anon 0\nfile 0\nkernel 0\nslab 0\n");
  else if (strcmp(base, "cpu.max") == 0)
    snprintf(buf, cap, "max 100000\n");
  else if (strcmp(base, "cpu.weight") == 0)
    snprintf(buf, cap, "100\n");
  else if (strcmp(base, "cpu.stat") == 0)
    snprintf(buf, cap, "usage_usec 0\nuser_usec 0\nsystem_usec 0\n");
  else if (strcmp(base, "pids.max") == 0)
    snprintf(buf, cap, "max\n");
  else if (strcmp(base, "pids.current") == 0)
    snprintf(buf, cap, "0\n");
  else if (strcmp(base, "pids.events") == 0)
    snprintf(buf, cap, "max 0\n");
  else if (strcmp(base, "cpuset.cpus") == 0 || strcmp(base, "cpuset.cpus.effective") == 0) {
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN); if (ncpu < 1) ncpu = 1; if (ncpu > 8) ncpu = 8;
    snprintf(buf, cap, "0-%ld\n", ncpu - 1);
  } else if (strcmp(base, "cpuset.mems") == 0 || strcmp(base, "cpuset.mems.effective") == 0)
    snprintf(buf, cap, "0\n");
  else if (strcmp(base, "io.max") == 0 || strcmp(base, "io.stat") == 0)
    snprintf(buf, cap, "%s", "");
  else if (strstr(base, "pressure") != 0)
    snprintf(buf, cap, "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"
                       "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
  else
    snprintf(buf, cap, "%s", "");
  return buf;
}

// 0 = not a cgroup path, 1 = cgroup directory, 2 = cgroup leaf file. Exported for syscalls_ext.c.
int cgroup_path_kind(const char* g) {
  if (!cgroup_under(g)) return 0;
  if (cgroup_file_name(g)) return 2;
  if (cgroup_is_dir(g)) return 1;
  return 0;                       // under /sys/fs/cgroup but not (yet) an existing dir or known file
}

// Per-fd backing store for cgroup directory fds. A child cgroup is not materialized in the rootfs
// (there is no host dir to open), so openat(O_DIRECTORY) returns a real fd opened on the host's
// /sys/fs/cgroup (a dirfd that exists) and this table remembers WHICH guest cgroup dir it stands
// for. do_getdents64 consults it before F_GETPATH.
#define CGDIR_MAX 64
static struct { int fd; char path[GPATH_MAX]; } g_cgdir_fd[CGDIR_MAX];

static int cgroup_dir_open(const char* g) {
  int slot = -1;
  for (int i = 0; i < CGDIR_MAX; i++) if (g_cgdir_fd[i].fd <= 0) { slot = i; break; }
  if (slot < 0) { errno = EMFILE; return -1; }
  char host[PATH_MAX];
  if (guest_to_host(CGROUP_PREFIX, host, sizeof host) != 0) { errno = ENOENT; return -1; }
  int fd = open(host, O_RDONLY | O_DIRECTORY);
  if (fd < 0) return -1;
  g_cgdir_fd[slot].fd = fd;
  snprintf(g_cgdir_fd[slot].path, GPATH_MAX, "%s", g);
  return fd;
}

// 0 and fills `out` if `fd` is a cgroup dir fd, else nonzero.
static int cgroup_dir_path(int fd, char* out, size_t cap) {
  for (int i = 0; i < CGDIR_MAX; i++)
    if (g_cgdir_fd[i].fd == fd && fd > 0) { snprintf(out, cap, "%s", g_cgdir_fd[i].path); return 0; }
  return -1;
}

static void cgroup_dir_drop(int fd) {
  for (int i = 0; i < CGDIR_MAX; i++)
    if (g_cgdir_fd[i].fd == fd) { g_cgdir_fd[i].fd = 0; g_cgdir_fd[i].path[0] = 0; return; }
}

// Serve openat() of a cgroup leaf file (mem_fd content) or child directory (backing fd). Returns
// 1 and sets *out when handled; 0 means "not a cgroup object, use the normal host open path".
static int cgroup_open(const char* g, long* out) {
  if (!cgroup_under(g)) return 0;
  char buf[2048];
  const char* c = cgroup_file_content(g, buf, sizeof buf);
  if (c) {
    int fd = mem_fd(c, strlen(c));
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  if (cgroup_is_dir(g) && !cgroup_is_root(g)) {     // root opens the real host dir (F_GETPATH works)
    int fd = cgroup_dir_open(g);
    *out = fd < 0 ? herrno() : fd;
    return 1;
  }
  return 0;
}

// getdents64 entry table for a cgroup directory: the static control files + the child cgroups
// currently registered (direct children only, like a real cgroupfs readdir).
static struct synth_ent g_cg_ents[160];
static char           g_cg_names[160][48];

static int cgroup_entries(const char* g, struct synth_ent* out) {
  int n = 0;
  #define CGADD(nm, ty) do { snprintf(g_cg_names[n], sizeof g_cg_names[n], "%s", nm); \
                             out[n].name = g_cg_names[n]; out[n].type = (ty); n++; } while (0)
  for (int i = 0; i < CGROUP_NFILES; i++) CGADD(CGROUP_FILES[i], DT_REG);
  size_t gl = strlen(g);
  for (int i = 0; i < g_cgroup_ndirs; i++) {
    const char* d = g_cgroup_dirs[i];
    if (strncmp(d, g, gl) != 0 || d[gl] != '/') continue;
    const char* child = d + gl + 1;
    if (strchr(child, '/')) continue;                  // only DIRECT children
    CGADD(child, DT_DIR);
  }
  #undef CGADD
  return n;
}

// Return the synthesized entry table for guest path `g`, or NULL if `g` is not a synthetic dir.
// *count receives the entry count (0 for a synthetic empty dir such as /dev/shm). Dynamic tables
// are rebuilt into static buffers on each call.
static const struct synth_ent* synth_entries(const char* g, int* count) {
  #define RET(tab) do { *count = (int)(sizeof(tab) / sizeof((tab)[0])); return (tab); } while (0)
  if (!g) return 0;
  if (strcmp(g, "/proc") == 0) { *count = proc_top_entries(g_proc_ents); return g_proc_ents; }
  if (strcmp(g, "/proc/self") == 0) RET(PROC_PID_ENTS);
  if (strncmp(g, "/proc/", 6) == 0) {                    // /proc/<digits>
    const char* p = g + 6;
    if (*p >= '0' && *p <= '9') {
      while (*p >= '0' && *p <= '9') p++;
      if (*p == 0) RET(PROC_PID_ENTS);
      if (strcmp(p, "/fd") == 0) { *count = proc_self_fd_entries(g_fd_ents); return g_fd_ents; }
    }
  }
  if (strcmp(g, "/proc/self/fd") == 0) { *count = proc_self_fd_entries(g_fd_ents); return g_fd_ents; }
  if (strcmp(g, "/proc/sys") == 0) RET(PROC_SYS_ENTS);
  if (strcmp(g, "/sys/class") == 0) RET(SYS_CLASS_ENTS);
  if (strcmp(g, "/sys/class/drm") == 0) RET(SYS_CLASS_DRM_ENTS);
  if (strcmp(g, "/sys/class/drm/card0") == 0 ||
      strcmp(g, "/sys/class/drm/renderD128") == 0) RET(SYS_CLASS_DRM_NODE_ENTS);
  if (strcmp(g, "/sys/class/drm/card0/device") == 0 ||
      strcmp(g, "/sys/class/drm/renderD128/device") == 0) RET(SYS_CLASS_DRM_DEVICE_ENTS);
  if (strcmp(g, "/sys/class/sound") == 0) RET(SYS_CLASS_SOUND_ENTS);
  if (strcmp(g, "/sys/class/input") == 0) RET(SYS_CLASS_INPUT_ENTS);
  if (strcmp(g, "/sys/class/block") == 0) RET(SYS_CLASS_BLOCK_ENTS);
  if (strcmp(g, "/sys/devices") == 0) RET(SYS_DEVICES_ENTS);
  if (strcmp(g, "/sys/devices/system") == 0) RET(SYS_DEVICES_SYSTEM_ENTS);
  if (strcmp(g, "/sys/devices/system/cpu") == 0) { *count = sys_cpu_entries(g_cpu_ents); return g_cpu_ents; }
  if (cgroup_is_dir(g)) { *count = cgroup_entries(g, g_cg_ents); return g_cg_ents; }
  if (strcmp(g, "/dev") == 0) RET(DEV_ENTS);
  if (strcmp(g, "/dev/dri") == 0) RET(DEV_DRI_ENTS);
  if (strcmp(g, "/dev/input") == 0) RET(DEV_INPUT_ENTS);
  if (strcmp(g, "/dev/snd") == 0) RET(DEV_SND_ENTS);
  if (strcmp(g, "/dev/shm") == 0) { *count = 0; return (const struct synth_ent*)(uintptr_t)1; }
  return 0;
  #undef RET
}

// Per-fd getdents64 cursor for synthetic dirs. The guest is effectively single-threaded, so a
// small fixed table keyed by fd is enough; close() drops the entry so an fd-number reuse restarts.
#define SYNCUR_MAX 64
static struct { int fd; int pos; } g_syncur[SYNCUR_MAX];

static int syncur_slot(int fd) {
  for (int i = 0; i < SYNCUR_MAX; i++) if (g_syncur[i].fd == fd) return i;
  for (int i = 0; i < SYNCUR_MAX; i++) if (g_syncur[i].fd <= 0) { g_syncur[i].fd = fd; g_syncur[i].pos = 0; return i; }
  return -1;
}
static void syncur_drop(int fd) {
  for (int i = 0; i < SYNCUR_MAX; i++) if (g_syncur[i].fd == fd) { g_syncur[i].fd = 0; g_syncur[i].pos = 0; return; }
}

// Synthetic getdents64 emits d_off = next entry index + 1 (and offset zero
// before the first entry). Host lseek only moves the empty backing directory;
// route guest directory seeks through the cursor that getdents64 actually uses.
static long syncur_lseek(int fd, int64_t offset, int whence, int count) {
  int slot = syncur_slot(fd);
  if (slot < 0) return -LX_EMFILE;
  int64_t current = g_syncur[slot].pos ? (int64_t)g_syncur[slot].pos + 1 : 0;
  int64_t target;
  if (whence == SEEK_SET) target = offset;
  else if (whence == SEEK_CUR) {
    if (__builtin_add_overflow(current, offset, &target)) return -LX_EOVERFLOW;
  } else return -LX_EINVAL;
  if (target == 0) g_syncur[slot].pos = 0;
  else if (target >= 2 && target <= (int64_t)count + 1)
    g_syncur[slot].pos = (int)(target - 1);
  else return -LX_EINVAL; // only zero and emitted directory cookies are valid
  return (long)target;
}

// Idempotently create a host directory and any missing parents (absolute path). EEXIST counts as
// success, so it is safe to call for /proc, /sys/class, /dev that install.sh already placed.
static int mkdir_p(const char* path) {
  char tmp[PATH_MAX];
  size_t n = strlen(path);
  if (n == 0 || n >= sizeof tmp) { errno = ENAMETOOLONG; return -1; }
  memcpy(tmp, path, n + 1);
  while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
  for (char* p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
      *p = '/';
    }
  }
  if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
  return 0;
}

// logind resolves an evdev node through its input parent, then requires that parent to belong
// to seat0. These are virtual devices, so no kernel uevent will populate that relationship.
static void evdev_materialize_udev(void) {
  char host[PATH_MAX], path[PATH_MAX];
  static const char* const dirs[] = { "/sys/devices/virtual/input", "/sys/bus/input",
                                      "/run/udev/data", "/run/udev/tags/seat" };
  for (size_t j = 0; j < sizeof dirs / sizeof dirs[0]; j++)
    if (guest_to_host(dirs[j], host, sizeof host) || mkdir_p(host)) return;
  for (int i = 0; i < 2; i++) {
    snprintf(path, sizeof path, "/sys/devices/virtual/input/input%d/event%d", i, i);
    if (guest_to_host(path, host, sizeof host) || mkdir_p(host)) return;
    const char* suffixes[] = { "/uevent", "/dev", "/subsystem" };
    for (int level = 0; level < 2; level++) {
      snprintf(path, sizeof path, level ? "/sys/devices/virtual/input/input%d/event%d" :
               "/sys/devices/virtual/input/input%d", i, i);
      for (int k = 0; k < 3; k++) {
        if (!level && k == 1) continue;
        char item[PATH_MAX];
        if (snprintf(item, sizeof item, "%s%s", path, suffixes[k]) >= (int)sizeof item ||
            guest_to_host(item, host, sizeof host)) return;
        if (k == 2) {
          if (symlink("/sys/bus/input", host) && errno != EEXIST) return;
        } else {
          int fd = open(host, O_RDONLY | O_CREAT, 0444);
          if (fd < 0) return;
          close(fd);
        }
      }
    }
    snprintf(path, sizeof path, "/sys/class/input/event%d", i);
    if (guest_to_host(path, host, sizeof host)) return;
    (void)rmdir(host); // only replace an empty legacy placeholder; preserve actual guest data
    char target[96];
    snprintf(target, sizeof target, "../../devices/virtual/input/input%d/event%d", i, i);
    if (symlink(target, host) && errno != EEXIST) return;
    char existing[96];
    ssize_t n = readlink(host, existing, sizeof existing - 1);
    if (n < 0) return;
    existing[n] = 0;
    if (strcmp(existing, target)) return;

    snprintf(path, sizeof path, "/run/udev/data/+input:input%d", i);
    if (guest_to_host(path, host, sizeof host)) return;
    char prior[512] = {0};
    int fd = open(host, O_RDONLY);
    if (fd >= 0) { (void)read(fd, prior, sizeof prior - 1); close(fd); }
    if (fd >= 0 && !strstr(prior, "E:SHIMMY_VIRTUAL_INPUT_PARENT=1\n")) return;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return;
    unsigned long long usec = (unsigned long long)ts.tv_sec * 1000000ULL +
                              (unsigned long long)ts.tv_nsec / 1000ULL;
    char db[512];
    int len = snprintf(db, sizeof db, "I:%llu\nE:DEVPATH=/devices/virtual/input/input%d\n"
                       "E:SUBSYSTEM=input\nE:ID_SEAT=seat0\nE:SHIMMY_VIRTUAL_INPUT_PARENT=1\n"
                       "G:seat\nQ:seat\n", usec, i);
    if (len < 0 || len >= (int)sizeof db) return;
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.shimmy-%d", host, (int)getpid()) >= (int)sizeof tmp) return;
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return;
    ssize_t written = write(fd, db, (size_t)len);
    close(fd);
    if (written != len || rename(tmp, host)) { unlink(tmp); return; }
    snprintf(path, sizeof path, "/run/udev/tags/seat/+input:input%d", i);
    if (guest_to_host(path, host, sizeof host)) return;
    fd = open(host, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) close(fd);
  }
}

// The DRM card is supplied by Shimmy, so udev has no kernel add event from which to build its
// database. logind enumerates /run/udev/tags/master-of-seat and resolves c226:0 through sysfs;
// a /dev/dri/card0 placeholder alone never makes seat0 graphical. Keep these records in the
// private guest root. The tag describes the virtual card, not a host GPU device.
static void drm_materialize_udev(void) {
  static const char* const dirs[] = {
    "/sys/dev/char", "/run/udev/data", "/run/udev/tags/seat",
    "/run/udev/tags/master-of-seat",
  };
  char host[PATH_MAX];
  for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
    if (guest_to_host(dirs[i], host, sizeof host) == 0) mkdir_p(host);

  static const char* const files[] = {
    "/run/udev/data/c226:0", "/run/udev/tags/seat/c226:0",
    "/run/udev/tags/master-of-seat/c226:0",
  };
  if (!drm_enabled()) {
    // RDDRM=0 must not leave a graphical seat from an earlier launch of this root.
    if (guest_to_host(files[0], host, sizeof host) == 0) {
      char db[512] = {0};
      int fd = open(host, O_RDONLY);
      if (fd >= 0) { (void)read(fd, db, sizeof db - 1); close(fd); }
      if (strstr(db, "E:SHIMMY_VIRTUAL_DRM=1\n")) {
        for (size_t i = 0; i < sizeof files / sizeof files[0]; i++)
          if (guest_to_host(files[i], host, sizeof host) == 0) unlink(host);
        if (guest_to_host("/run/udev/data/c226:128", host, sizeof host) == 0) {
          char render_db[512] = {0};
          int render_fd = open(host, O_RDONLY);
          if (render_fd >= 0) { (void)read(render_fd, render_db, sizeof render_db - 1); close(render_fd); }
          if (strstr(render_db, "E:SHIMMY_VIRTUAL_DRM=1\n")) unlink(host);
        }
        if (guest_to_host("/sys/dev/char/226:0", host, sizeof host) == 0) {
          char target[64];
          ssize_t n = readlink(host, target, sizeof target - 1);
          if (n >= 0) {
            target[n] = 0;
            if (strcmp(target, "../../class/drm/card0") == 0) unlink(host);
          }
        }
        if (guest_to_host("/sys/dev/char/226:128", host, sizeof host) == 0) {
          char target[64];
          ssize_t n = readlink(host, target, sizeof target - 1);
          if (n >= 0) {
            target[n] = 0;
            if (strcmp(target, "../../class/drm/renderD128") == 0) unlink(host);
          }
        }
      }
    }
    return;
  }

  if (guest_to_host("/sys/dev/char/226:0", host, sizeof host) != 0) return;
  if (symlink("../../class/drm/card0", host) < 0 && errno != EEXIST) return;
  char target[64];
  ssize_t target_len = readlink(host, target, sizeof target - 1);
  if (target_len < 0) return;
  target[target_len] = 0;
  if (strcmp(target, "../../class/drm/card0") != 0) return;

  int db_ready = 0;
  if (guest_to_host(files[0], host, sizeof host) == 0) {
    // Do not replace a database entry authored by the guest's actual udev service.
    char prior[512] = {0};
    int old_fd = open(host, O_RDONLY);
    if (old_fd >= 0) {
      (void)read(old_fd, prior, sizeof prior - 1);
      close(old_fd);
      if (!strstr(prior, "E:SHIMMY_VIRTUAL_DRM=1\n")) return;
    }
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return;
    unsigned long long usec = (unsigned long long)ts.tv_sec * 1000000ULL +
                              (unsigned long long)ts.tv_nsec / 1000ULL;
    char db[384];
    int len = snprintf(db, sizeof db,
                       "I:%llu\nE:DEVNAME=/dev/dri/card0\nE:DEVPATH=/class/drm/card0\n"
                       "E:SUBSYSTEM=drm\nE:DEVTYPE=drm_minor\nE:ID_SEAT=seat0\n"
                       "E:SHIMMY_VIRTUAL_DRM=1\nG:seat\nG:master-of-seat\n"
                       "Q:seat\nQ:master-of-seat\n", usec);
    if (len < 0 || len >= (int)sizeof db) return;
    // Readers must never observe a partially written udev record during parallel service starts.
    char tmp[PATH_MAX];
    int tmp_len = snprintf(tmp, sizeof tmp, "%s.shimmy-%d", host, (int)getpid());
    if (tmp_len > 0 && tmp_len < (int)sizeof tmp) {
      int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
      if (fd >= 0) {
        ssize_t n = write(fd, db, (size_t)len);
        close(fd);
        if (n == (ssize_t)len && rename(tmp, host) == 0) db_ready = 1;
        else unlink(tmp);
      }
    }
  }
  if (!db_ready) return;
  for (size_t i = 1; i < sizeof files / sizeof files[0]; i++) {
    if (guest_to_host(files[i], host, sizeof host) != 0) continue;
    int fd = open(host, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) close(fd);
  }
  // Render nodes have a separate devnum and udev identity. They do not own a seat or serve as
  // master-of-seat; the primary card above remains the graphical seat device.
  if (guest_to_host("/sys/dev/char/226:128", host, sizeof host) != 0) return;
  if (symlink("../../class/drm/renderD128", host) < 0 && errno != EEXIST) return;
  target_len = readlink(host, target, sizeof target - 1);
  if (target_len < 0) return;
  target[target_len] = 0;
  if (strcmp(target, "../../class/drm/renderD128") != 0) return;
  if (guest_to_host("/run/udev/data/c226:128", host, sizeof host) != 0) return;
  char prior[512] = {0};
  int old_fd = open(host, O_RDONLY);
  if (old_fd >= 0) {
    (void)read(old_fd, prior, sizeof prior - 1);
    close(old_fd);
    if (!strstr(prior, "E:SHIMMY_VIRTUAL_DRM=1\n")) return;
  }
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return;
  unsigned long long usec = (unsigned long long)ts.tv_sec * 1000000ULL +
                            (unsigned long long)ts.tv_nsec / 1000ULL;
  char db[384];
  int len = snprintf(db, sizeof db,
                     "I:%llu\nE:DEVNAME=/dev/dri/renderD128\nE:DEVPATH=/class/drm/renderD128\n"
                     "E:SUBSYSTEM=drm\nE:DEVTYPE=drm_minor\nE:SHIMMY_VIRTUAL_DRM=1\n", usec);
  if (len < 0 || len >= (int)sizeof db) return;
  char tmp[PATH_MAX];
  int tmp_len = snprintf(tmp, sizeof tmp, "%s.shimmy-%d", host, (int)getpid());
  if (tmp_len < 0 || tmp_len >= (int)sizeof tmp) return;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) return;
  ssize_t n = write(fd, db, (size_t)len);
  close(fd);
  if (n == (ssize_t)len && rename(tmp, host) == 0) return;
  unlink(tmp);
}

// Materialize every synthesized directory in the rootfs, once, at startup. Eager (not lazy in each
// syscall) so statx/newfstatat/openat/faccessat all find the dirs and F_GETPATH maps their fds back
// to the guest path for do_getdents64. The set is static plus the live pid/cpu counts, all known
// here. mkdir_p is idempotent; the dirs that install.sh already placed are left untouched.
static void synth_materialize_all(void) {
  // /proc ships mode 0555 (a real procfs mount), which would block mkdir of its synthetic subdirs.
  // Relax the three mountpoints to 0755 first so the tree can be built; the guest only reads these
  // dirs, so the mode difference is cosmetic.
  static const char* const mnts[] = { "/proc", "/sys", "/dev" };
  char host[PATH_MAX], p[GPATH_MAX];
  for (size_t i = 0; i < sizeof mnts / sizeof mnts[0]; i++)
    if (guest_to_host(mnts[i], host, sizeof host) == 0) chmod(host, 0755);

  static const char* const dirs[] = {
    "/proc", "/proc/self", "/proc/1", "/proc/sys", "/proc/self/fd", "/proc/1/fd",
    "/sys/class", "/sys/class/drm", "/sys/class/sound", "/sys/class/input", "/sys/class/block",
    "/sys/devices", "/sys/devices/system", "/sys/devices/system/cpu",
    "/dev", "/dev/dri", "/dev/input", "/dev/snd", "/dev/shm", "/dev/pts",
  };
  for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
    if (guest_to_host(dirs[i], host, sizeof host) == 0) mkdir_p(host);
  if (drm_enabled()) {
    static const char* const drm_dirs[] = {
      "/sys/class/drm/card0/device", "/sys/class/drm/renderD128/device",
    };
    for (size_t i = 0; i < sizeof drm_dirs / sizeof drm_dirs[0]; i++)
      if (guest_to_host(drm_dirs[i], host, sizeof host) == 0) mkdir_p(host);
  }

  snprintf(p, sizeof p, "/proc/%d", (int)getpid());
  if (guest_to_host(p, host, sizeof host) == 0) mkdir_p(host);
  int pp = (int)getppid();
  if (pp > 1) {
    snprintf(p, sizeof p, "/proc/%d", pp);
    if (guest_to_host(p, host, sizeof host) == 0) mkdir_p(host);
  }

  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpu < 1)  ncpu = 1;
  if (ncpu > 32) ncpu = 32;
  for (long i = 0; i < ncpu; i++) {
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%ld", i);
    if (guest_to_host(p, host, sizeof host) == 0) mkdir_p(host);
  }
}

// ===========================================================================================
// getdents64
// ===========================================================================================

static long do_getdents64(int fd, unsigned char* buf, size_t cap) {
  if (!buf) return -LX_EFAULT;

  // Synthetic /proc, /sys, /dev directory: serve the entry table, not the (empty) host rootfs dir.
  // The fd was opened against a real rootfs dir (materialized by synth_materialize_all at startup),
  // so F_GETPATH maps it back to the guest path; g_syncur tracks how far a repeated getdents64()
  // has advanced.
  char dpath[GPATH_MAX];
  dpath[0] = 0;
  // A cgroup dir fd is backed by the host's /sys/fs/cgroup dirfd (see cgroup_dir_open), so its
  // F_GETPATH is wrong; the fd->guest-path table is authoritative first.
  if (cgroup_dir_path(fd, dpath, sizeof dpath) != 0)
    (void)guest_path_of_fd(fd, dpath, sizeof dpath);
  if (dpath[0]) {
    int count = 0;
    const struct synth_ent* ents = synth_entries(dpath, &count);
    if (ents) {
      int slot = syncur_slot(fd);
      if (slot < 0) return -LX_EMFILE;
      size_t used = 0;
      int pos = g_syncur[slot].pos;
      while (pos < count) {
        const char* name = ents[pos].name;
        size_t nl = strlen(name);
        size_t rec = (19 + nl + 1 + 7) & ~(size_t)7;   // 8-align, room for the NUL
        if (used + rec > cap) {
          if (used == 0) return -LX_EINVAL;            // buffer too small for even one entry
          break;
        }
        unsigned char* p = buf + used;
        memset(p, 0, rec);
        uint64_t ino = (uint64_t)(pos + 1);            // synthetic, stable within this dir
        int64_t  off = (int64_t)(pos + 2);             // cookie of the NEXT entry
        uint16_t rl  = (uint16_t)rec;
        uint8_t  ty  = ents[pos].type;
        memcpy(p + 0, &ino, 8);
        memcpy(p + 8, &off, 8);
        memcpy(p + 16, &rl, 2);
        memcpy(p + 18, &ty, 1);
        memcpy(p + 19, name, nl + 1);
        used += rec;
        pos++;
      }
      g_syncur[slot].pos = pos;
      return (long)used;
    }
  }

  DIR* d = dirmap_open(fd);
  if (!d) return herrno();
  // /dev/snd's placeholder files are regular files in the rootfs, but the guest must see them as
  // char devices: pipewire's ALSA monitor (spa/plugins/alsa/alsa-udev.c get_num_pcm_devices)
  // filters readdir() on d_type == DT_CHR. Without this the monitor reports "no PCM devices"
  // and never auto-creates the sink node.
  char dpath2[PATH_MAX];
  int is_snddir = (guest_path_of_fd(fd, dpath2, sizeof dpath2) == 0 &&
                   strcmp(dpath2, "/dev/snd") == 0);
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
    if (is_snddir && snd_is_node_name(e->d_name)) ty = DT_CHR;
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
    if (gmem_guest_copyout_checked((uint64_t)(uintptr_t)buf, g_cwd, n + 1) != 0)
      return -LX_EFAULT;
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
  case 0x2002:
    return (getenv("RDMETAL_LEDGER_MOCK") &&
            !strcmp(getenv("RDMETAL_LEDGER_MOCK"),"1")) ?
        rdgpu_mock_gate(a0, a1, a2, a3, a4, a5) :
        rdgpu_metal_gate(a0, a1, a2, a3, a4, a5);
#endif

  // ---------- 0x2003 rdmetal VAAPI/VideoToolbox media-engine bridge ----------
  case RDVA_GUEST_SYSCALL: return rdva_bridge(a0, a1, a2, a3, a4, a5);

  // ---------- 32 flock ----------  (LOCK_SH/EX/NB/UN values match)
  case 32: return herr(flock((int)a0, (int)a1));

  // ---------- 34 mkdirat ----------
  case 34: {
    // Snapshot the pathname before the resolver dereferences guest memory.
    // A bad guest pointer must return EFAULT without creating a host folder.
    if (!a1) return -LX_EFAULT;
    char guest_path[PATH_MAX];
    size_t i;
    for (i = 0; i < sizeof guest_path; ++i) {
      if (gmem_guest_export_copyin(NULL, a1 + i, &guest_path[i], 1) != 0)
        return -LX_EFAULT;
      if (!guest_path[i]) break;
    }
    if (i == sizeof guest_path) return -LX_ENAMETOOLONG;
    if ((r = resolve_f((int)a0, guest_path, RSV_NOFOLLOW, host, sizeof host, gabs, sizeof gabs)) < 0)
      return r;
    // cgroup v2 child cgroups live in the in-memory registry, not the rootfs.
    if (cgroup_under(gabs)) {
      if (cgroup_file_name(gabs)) return -LX_EEXIST;         // mkdir of a control file
      if (cgroup_is_dir(gabs)) return -LX_EEXIST;
      char pbuf[GPATH_MAX];
      const char* par = cgroup_parent(gabs, pbuf, sizeof pbuf);
      if (par && !cgroup_is_dir(par)) return -LX_ENOENT;     // no parent cgroup yet
      if (cgroup_register(gabs) < 0) return -LX_ENOMEM;
      return 0;
    }
    {
      long mr = herr(mkdir(host, (mode_t)a2));
      // Darwin creates this with the host user's UID. Under RDUID0 that is
      // not the guest creator: an untracked inode otherwise appears owned by
      // root. D-Bus checks its newly created XDG_RUNTIME_DIR/dbus-1 against
      // the non-root guest UID and refuses to start if this record is absent.
      if (mr == 0 && g_uid0 && (guest_cred_uid(1) || guest_cred_gid(1))) {
        struct stat created;
        if (lstat(host, &created) < 0 ||
            guest_ownership_set(&created, guest_cred_uid(1), guest_cred_gid(1)) < 0) {
          long creation_error = herrno();
          rmdir(host); // leave no misleading root-owned directory on failure
          return creation_error;
        }
      }
      if (getenv("RDEXECDBG") && mr < 0) {
        FILE* f = fopen("/tmp/rd_exec_dbg.txt", "a");
        if (f) { fprintf(f, "pid=%d MKDIR-FAIL %s host=%s -> %ld\n", (int)getpid(), gabs, host, mr); fclose(f); }
      }
      return mr;
    }
  }

  // ---------- 35 unlinkat ----------
  case 35: {
    char guest_path[PATH_MAX];
    if ((r = readlink_guest_path(a1, guest_path, sizeof guest_path)) < 0)
      return r;
    if ((r = resolve_f((int)a0, guest_path, RSV_NOFOLLOW, host, sizeof host, gabs, sizeof gabs)) < 0)
      return r;
    if (cgroup_under(gabs)) {
      if (a2 & LX_AT_REMOVEDIR) {                            // rmdir of a child cgroup
        if (cgroup_is_root(gabs)) return -LX_EBUSY;
        if (!cgroup_is_dir(gabs)) return -LX_ENOENT;
        cgroup_unregister(gabs);
        return 0;
      }
      return -LX_ENOENT;                                      // control files are never unlinked
    }
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
    unsigned flags = (unsigned)a4;
    // Linux accepts FOLLOW and EMPTY_PATH here. The latter names the source
    // by fd, which the pathname resolver below cannot represent. Reject it
    // rather than silently hard-linking a different object. Check flags
    // before resolving either path or changing the host filesystem.
    if (flags & ~(unsigned)(LX_AT_SYMLINK_FOLLOW | LX_AT_EMPTY_PATH))
      return -LX_EINVAL;
    if (flags & LX_AT_EMPTY_PATH) return -LX_EOPNOTSUPP;
    char host2[PATH_MAX];
    char old_path[PATH_MAX], new_path[PATH_MAX];
    // The resolver takes host-readable strings, not raw guest addresses.
    // Check both names before either resolution or hard-link creation.
    if ((r = readlink_guest_path(a1, old_path, sizeof old_path)) < 0) return r;
    if ((r = readlink_guest_path(a3, new_path, sizeof new_path)) < 0) return r;
    int oldnf = (flags & LX_AT_SYMLINK_FOLLOW) ? 0 : RSV_NOFOLLOW;
    if ((r = resolve_f((int)a0, old_path, oldnf, host, sizeof host, 0, 0)) < 0) return r;
    if ((r = resolve_f((int)a2, new_path, RSV_NOFOLLOW, host2, sizeof host2, 0, 0)) < 0)
      return r;
    // Darwin link() follows a symbolic source even when Linux linkat(..., 0)
    // must link the symlink inode itself. Darwin linkat() takes the follow
    // flag explicitly, so use it after the guest-root paths are resolved.
    return herr(linkat(AT_FDCWD, host, AT_FDCWD, host2,
                       (flags & LX_AT_SYMLINK_FOLLOW) ? AT_SYMLINK_FOLLOW : 0));
  }

  // ---------- 38 renameat ----------
  case 38: {
    char host2[PATH_MAX], g2[GPATH_MAX];
    char old_path[PATH_MAX], new_path[PATH_MAX];
    // Snapshot both guest strings before resolving either path or changing
    // the filesystem. In particular, an invalid destination must not let a
    // host dereference guest memory or rename the source.
    if ((r = readlink_guest_path(a1, old_path, sizeof old_path)) < 0) return r;
    if ((r = readlink_guest_path(a3, new_path, sizeof new_path)) < 0) return r;
    if ((r = resolve_f((int)a0, old_path, RSV_NOFOLLOW, host, sizeof host, gabs, sizeof gabs)) < 0)
      return r;
    if ((r = resolve_f((int)a2, new_path, RSV_NOFOLLOW, host2, sizeof host2, g2, sizeof g2)) < 0)
      return r;
    if (cgroup_under(gabs) || cgroup_under(g2)) {
      if (cgroup_is_root(gabs)) return -LX_EBUSY;             // never rename the root cgroup
      if (!cgroup_is_dir(gabs)) return -LX_ENOENT;
      if (cgroup_is_dir(g2)) return -LX_EEXIST;
      char pbuf[GPATH_MAX];
      const char* par = cgroup_parent(g2, pbuf, sizeof pbuf);
      if (par && !cgroup_is_dir(par)) return -LX_ENOENT;
      size_t gl = strlen(gabs);
      for (int i = 0; i < g_cgroup_ndirs; i++) {
        if (strncmp(g_cgroup_dirs[i], gabs, gl) == 0 &&
            (g_cgroup_dirs[i][gl] == '/' || g_cgroup_dirs[i][gl] == 0)) {
          char np[GPATH_MAX];
          snprintf(np, sizeof np, "%s%s", g2, g_cgroup_dirs[i] + gl);
          snprintf(g_cgroup_dirs[i], GPATH_MAX, "%s", np);
        }
      }
      return 0;
    }
    return herr(rename(host, host2));
  }

  // ---------- 43 statfs / 44 fstatfs ----------
  case 43: {
    struct statfs s;
    // The resolver expects a host-readable string. Copy the guest pathname
    // through checked memory first so a bad guest address returns EFAULT.
    char guest_path[PATH_MAX];
    if ((r = readlink_guest_path(a0, guest_path, sizeof guest_path)) < 0) return r;
    if ((r = resolve(LX_AT_FDCWD, guest_path, host, sizeof host, gabs, sizeof gabs)) < 0) return r;
    if (statfs(host, &s) < 0) return herrno();
    if (!a1) return -LX_EFAULT;
    struct linux_statfs out;
    statfs_m2l(&s, &out, gabs);
    return gmem_guest_copyout_checked(a1, &out, sizeof out) == 0 ? 0 : -LX_EFAULT;
  }
  case 44: {
    struct linux_statfs out = {0};
    // Virtual pidfds are kqueues on Darwin, where fstatfs(kqueue) fails with EINVAL.
    // Linux pidfds predating pidfs live on anon_inodefs. Report that filesystem,
    // rather than inventing pidfs inode identity that this shim cannot provide.
    if (pidfd_shim_pid((int)a0) > 0) {
      if (!a1) return -LX_EFAULT;
      out.f_type = 0x09041934; // ANON_INODE_FS_MAGIC
      out.f_bsize = 4096;
      out.f_namelen = 255;
      out.f_frsize = 4096;
      return gmem_guest_copyout_checked(a1, &out, sizeof out) == 0 ? 0 : -LX_EFAULT;
    }
    struct statfs s;
    if (fstatfs((int)a0, &s) < 0) return herrno();
    if (!a1) return -LX_EFAULT;
    char gpath[PATH_MAX]; gpath[0] = 0;
    if (guest_path_of_fd((int)a0, gpath, sizeof gpath) < 0) gpath[0] = 0;
    statfs_m2l(&s, &out, gpath);
    return gmem_guest_copyout_checked(a1, &out, sizeof out) == 0 ? 0 : -LX_EFAULT;
  }

  // ---------- 46 ftruncate ----------
  case 46: {                                          // ftruncate
    // Linux rejects a negative signed off_t before inspecting the fd or its
    // seals. A sealed fd must not turn that invalid request into EPERM.
    if ((int64_t)a1 < 0) return -LX_EINVAL;
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
    // The shared resolver reads a C string. Snapshot it from admitted guest
    // memory first, so an unmapped pointer returns EFAULT instead of
    // faulting the host shim during an ordinary access check.
    if (!a1) return -LX_EFAULT;
    char guest_path[PATH_MAX];
    size_t i;
    for (i = 0; i < sizeof guest_path; ++i) {
      if (gmem_guest_export_copyin(NULL, a1 + i, &guest_path[i], 1) != 0)
        return -LX_EFAULT;
      if (!guest_path[i]) break;
    }
    if (i == sizeof guest_path) return -LX_ENAMETOOLONG;
    if ((r = resolve((int)a0, guest_path, host, sizeof host, gabs, sizeof gabs)) < 0)
      return r;
    if (strcmp(gabs, "/proc/self/exe") == 0 || strcmp(gabs, "/proc/self/maps") == 0 ||
        strcmp(gabs, "/proc/self/cmdline") == 0 || strcmp(gabs, "/proc/self/stat") == 0 ||
        strcmp(gabs, "/proc/self/status") == 0 || strcmp(gabs, "/proc/self/environ") == 0 ||
        strcmp(gabs, "/proc/self/auxv") == 0 || strcmp(gabs, "/proc/self/mounts") == 0 || strcmp(gabs, "/proc/cpuinfo") == 0 ||
        strcmp(gabs, "/proc/meminfo") == 0 || strcmp(gabs, "/proc/uptime") == 0 ||
        strcmp(gabs, "/proc/loadavg") == 0 || strcmp(gabs, "/proc/stat") == 0 ||
        strcmp(gabs, "/proc/version") == 0 || strcmp(gabs, "/proc/consoles") == 0 ||
        strcmp(gabs, "/sys/class/tty/console/active") == 0 ||
        strcmp(gabs, "/sys/devices/system/cpu/possible") == 0 ||
        strcmp(gabs, "/sys/devices/system/cpu/online") == 0 ||
        strcmp(gabs, "/sys/devices/system/cpu/present") == 0 ||
        strcmp(gabs, "/sys/devices/system/cpu/offline") == 0)
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
    // The resolver consumes a C string and dereferences its first byte.
    // Snapshot a bounded guest path so an unmapped pointer returns EFAULT
    // instead of faulting the host shim while changing guest cwd.
    if (!a0) return -LX_EFAULT;
    char guest_path[PATH_MAX];
    size_t i;
    for (i = 0; i < sizeof guest_path; ++i) {
      if (gmem_guest_export_copyin(NULL, a0 + i, &guest_path[i], 1) != 0)
        return -LX_EFAULT;
      if (!guest_path[i]) break;
    }
    if (i == sizeof guest_path) return -LX_ENAMETOOLONG;
    if ((r = resolve(LX_AT_FDCWD, guest_path, host, sizeof host, gabs, sizeof gabs)) < 0)
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

  // ---------- 50 fchdir ----------
  case 50: {
    int fd = (int)a0;
    if (fd < 0 || shim_fd_reserved(fd)) return -LX_EBADF;
    struct stat opened, mapped;
    if (fstat(fd, &opened) < 0) return herrno();
    if (!S_ISDIR(opened.st_mode)) return -LX_ENOTDIR;
    // F_GETPATH alone can name a host directory outside the guest root.
    // Map the fd into the guest namespace, then require that resolving that
    // guest name leads back to the very same directory before changing cwd.
    if ((r = guest_path_of_fd(fd, gabs, sizeof gabs)) < 0) return r;
    if (strnlen(gabs, sizeof gabs) == sizeof gabs) return -LX_ENAMETOOLONG;
    if ((r = resolve(LX_AT_FDCWD, gabs, host, sizeof host, NULL, 0)) < 0)
      return r;
    if (stat(host, &mapped) < 0) return herrno();
    if (opened.st_dev != mapped.st_dev || opened.st_ino != mapped.st_ino)
      return -LX_EOPNOTSUPP;
    if (access(host, X_OK) < 0) return herrno();
    snprintf(g_cwd, sizeof g_cwd, "%s", gabs);
    return 0;
  }

  // ---------- 56 openat ----------
  case 437: { // openat2: validated no-RESOLVE subset uses the existing guest openat path
    struct rd_open_how how;
    long status = rd_openat2_decode((const void *)a2, (size_t)a3, &how);
    if (status < 0) return status;
    return linux_syscall(56, a0, a1, (unsigned long)how.flags,
                         (unsigned long)how.mode, 0, 0);
  }
  case 56: {
    // The resolver expects a host-readable pathname. Snapshot the bounded
    // guest string first so a bad guest address returns EFAULT rather than
    // faulting the shim during this common filesystem operation.
    char guest_path[PATH_MAX];
    if ((r = readlink_guest_path(a1, guest_path, sizeof guest_path)) < 0) return r;
    // O_NOFOLLOW must stop the GUEST-side resolver too, not only the host open(): if we resolved
    // the final component here, the host would be handed a non-link and O_NOFOLLOW would silently
    // become a no-op instead of the ELOOP the guest asked for.
    if ((r = resolve_f((int)a0, guest_path, ((int)a2 & LO_NOFOLLOW) ? RSV_NOFOLLOW : 0,
                       host, sizeof host, gabs, sizeof gabs)) < 0)
      return r;
    // RDPATHS=1 names the FILE. RDVERBOSE prints pointers, which is useless when the question is
    // "what is the guest looking for" -- the answer is always a string it never shows you.
    // (From Q2, who needed it to identify a forked child; it never reached main and I then spent
    //  a diagnosis session unable to see what an app was searching for.)
    { static int on = -1;
      if (on < 0) on = getenv("RDPATHS") != 0;
      if (on) fprintf(stderr, "[path] tid=%d openat %s\n", guest_tid(), gabs); }
    if (getenv("RDEXECDBG")) {
      FILE* pf = fopen("/tmp/rd_exec_dbg.txt", "a");
      if (pf) { fprintf(pf, "pid=%d OPENAT %s\n", (int)getpid(), gabs); fclose(pf); }
    }
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
      // A DRM render/master node. The fd is a dup of the shim's event-pipe read end (so
      // page-flip events arrive via read() and poll correctly), and every ioctl on it is
      // answered by drmasahi.c.
      int dfd = drm_open_fd();
      if (dfd >= 0) { drm_register_fd_path(dfd, gabs); return dfd; }
      return herrno();
    }
    // /sys/firmware/qemu_fw_cfg/by_name/opt/io.systemd.credentials — the qemu VM credential store.
    // systemd loads this absolute-path credential with missing_ok=false, so a real ENOENT aborts
    // every service spawn at "step CREDENTIALS". Serve it (and any qemu_fw_cfg file) as an empty
    // file: the shim is not a VM, so there are no VM credentials to offer.
    if (strncmp(gabs, "/sys/firmware/qemu_fw_cfg/", 27) == 0) {
      int ufd = mem_fd("", 0);
      if (ufd >= 0) return ufd;
      return herrno();
    }
    // /sys/dev/char/226:*/uevent — libdrm's drmGetDeviceNameFromFd2() reads DEVNAME= here to
    // name the GPU. An empty guest /sys makes it return NULL, which aquamarine dereferences.
    if (drm_sysfs_kind(gabs) == 2) {
      const char* c = drm_sysfs_uevent(gabs);
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
    // /sys/class/drm/{card0,renderD128}/{uevent,dev} — the libudev enumeration surface
    // aquamarine's scanGPUs() walks to find the DRM card (see drmasahi.c kinds 9/10).
    if (drm_sysfs_kind(gabs) == 9 || drm_sysfs_kind(gabs) == 10) {
      const char* c = (drm_sysfs_kind(gabs) == 9) ? drm_sysfs_uevent_for(gabs)
                                                  : drm_sysfs_dev_for(gabs);
      int ufd = mem_fd(c, strlen(c));
      if (ufd >= 0) return ufd;
      return herrno();
    }
    // /sys/class/sound/card0/{uevent,dev} and /run/udev/data/c116:0 — the synthetic ALSA card's
    // sysfs/udev surface for WirePlumber's ALSA monitor (see snd_alsa.c). Served in-process so no
    // hand-placed rootfs files are needed.
    {
      int sk = snd_sysfs_kind(gabs);
      if (sk == 1 || sk == 2 || sk == 6 || sk == 7) {
        const char* c = snd_sysfs_content(gabs);
        if (!c) return -LX_EIO;
        int ufd = mem_fd(c, strlen(c));
        if (ufd >= 0) return ufd;
        return herrno();
      }
    }
    // /sys/class/input/event0|1/{uevent,dev} and /run/udev/data/c13:64|65 — the synthetic evdev
    // input surface for libinput's udev backend (see evdev.c). Served in-process.
    {
      int ek = evdev_sysfs_kind(gabs);
      if (ek == 1 || ek == 2 || ek == 4 || ek == 5 || ek == 9) {
        if ((ek == 1 || ek == 9) && (a2 & LO_ACCMODE) != 0) return -LX_EROFS;
        const char* c = evdev_sysfs_content(gabs);
        if (!c) return -LX_EIO;
        int ufd = mem_fd(c, strlen(c));
        if (ufd >= 0) return ufd;
        return herrno();
      }
    }
    // /dev/input/event0|1 — the synthetic evdev nodes. The fd is a dup of the device's
    // event-pipe read end, so read()/poll() behave like a real evdev fd.
    if (evdev_is_node(gabs)) {
      int efd = evdev_open_node(gabs);
      if (efd >= 0) return efd;
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
    // /dev/kmsg — systemd (and its executor, hardcoded --log-target kmsg) writes its log here.
    // A missing node made the write fall back to the journal socket and block forever (no journald
    // reader yet). A per-boot append file accepts the writes so the real error stays visible.
    if (strcmp(gabs, "/dev/kmsg") == 0) return kmsg_open(a2);
    // /dev/tty1..tty6, /dev/console, /dev/tty0 — the Linux virtual consoles. Each is the SLAVE
    // of a shim-owned pty pair (vt.c); the shim renders the master side to the console surface.
    if (vt_is_node(gabs)) {
      int vfd = vt_open_node(gabs);
      if (vfd >= 0) return vfd;
      return vfd;                                   // negative Linux errno
    }
    long vres;
    if (proc_open(gabs, &vres)) return vres;
    if (cgroup_open(gabs, &vres)) return vres;
    // /proc/self/fd/<N> — reopen the referenced fd with the guest's flags. systemd fd_reopen()
    // relies on this to read sysfs files it just chased open as O_PATH. On Linux this is a
    // symlink to the inode; here we reopen via the fd's own path (falling back to dup for
    // sockets/pipes that have no path).
    {
      int pfd = proc_fd_number(gabs);
      if (pfd >= 0) {
        int mf2 = open_flags_l2m((int)a2);
        char hp[PATH_MAX];
        if (fcntl(pfd, F_GETPATH, hp) == 0) {
          int fd2 = open(hp, mf2, (mode_t)a3);
          if (fd2 >= 0) return fd2;
          // F_GETPATH names the path the file was OPENED with, which is stale for an unlinked
          // anon file (mem_fd's mkstemp+unlink). libudev's device_read_uevent_file() chases the
          // synthetic sysfs uevent open as O_PATH and then fd_reopen()s it through this path;
          // without the dup fallback that reopen returned ENOENT and the uevent body was never
          // parsed (SUBSYSTEM survived only via the subsystem symlink, DEVTYPE/MAJOR/MINOR/DEVNAME
          // were all lost, and the ALSA card was never enumerated). Fall through to dup.
        }
        int fd2 = fcntl(pfd, F_DUPFD_CLOEXEC, 0);
        if (fd2 >= 0) return fd2;
        return herrno();
      }
    }
    int mf = open_flags_l2m((int)a2);
    if ((a2 & LO_PATH) && !(a2 & LO_NOFOLLOW)) {
      long socket_fd = net_open_bound_socket_opath(gabs, (a2 & LO_CLOEXEC) != 0);
      if (socket_fd != NET_OPATH_NOT_BOUND) return socket_fd;
    }
    int fd = open(host, mf, (mode_t)a3);
    if (fd < 0) {
      if (getenv("RDEXECDBG")) {
        FILE* f = fopen("/tmp/rd_exec_dbg.txt", "a");
        if (f) { fprintf(f, "pid=%d OPEN-FAIL gabs=%s host=%s flags=%#x mf=%#x errno=%d\n",
                         (int)getpid(), gabs, host, (unsigned)a2, mf, errno); fclose(f); }
      }
      return herrno();
    }
    if (strcmp(gabs,"/dev/ptmx")==0) pty_register(fd);
    return fd;
  }

  // ---------- 57 close ----------
  case 57:
    if (inotify_watch_is_fd((int)a0)) {
      net_fd_closed((int)a0);
      return inotify_watch_close((int)a0) ? 0 : -LX_EBADF;
    }
    // Shim-owned descriptors (the six pty MASTERs, the private stderr dup) are not the guest's to
    // close. systemd's fd cleanup closes 3..8 at boot (it sees them as leaked init fds); letting
    // it close the pty masters leaves the console slave with no master, and the NEXT
    // open(/dev/console) blocks forever waiting for a master that will never come back. Refuse
    // vacuously (as close_range already does) so the guest believes the close succeeded.
    if (shim_fd_reserved((int)a0)) {
      // Timerfd read ends are reserved to protect the parent's fd table
      // across vfork, but remain guest-owned for an ordinary close.
      if (!guest_is_vfork_child() && timerfd_shim_closed((int)a0)) {
        net_fd_closed((int)a0);
        return herr(close((int)a0));
      }
      // A signalfd read end is reserved only to survive the shared host fd
      // table of a vfork child. An ordinary guest close owns this descriptor:
      // retire its backend and epoll interest before closing the read end.
      // A vfork child must leave the parent's shared fd table intact.
      if (!guest_is_vfork_child() && signalfd_shim_closed((int)a0)) {
        net_fd_closed((int)a0);
        return herr(close((int)a0));
      }
      return 0;
    }
    // A pidfd (kqueue) is never actually closed: systemd recycles the pidfd NUMBER across its
    // pidref lifecycle (open -> close -> reopen the same number for a DIFFERENT pid), so closing it
    // here would let the fd number be recycled and a later waitid(P_PIDFD, fd) would resolve the
    // wrong pid. Leak it instead: the raw fd stays unique, so the registry's fd->pid mapping stays
    // correct. The leak is bounded (~1 per pidfd_open, well under the 512-slot registry).
    if (pidfd_shim_pid((int)a0) > 0) {
      pidfd_shim_closed((int)a0); // retain host fd for waitid, close it logically for signaling
      return 0;
    }
    { int p=pty_index((int)a0); if(p>=0) g_guest_pty[p].master=0; }
    seals_set((int)a0, 0);
    if (getenv("IDLE_PIPE_TRACE") && (int)a0 == 11) {
      struct stat st;
      int ok = fstat((int)a0, &st);
      fprintf(stderr, "[idle-socket] pid=%d op=close fd=11 valid=%d ino=%llu\n",
              getpid(), ok == 0, ok == 0 ? (unsigned long long)st.st_ino : 0ULL);
    }
    dirmap_close((int)a0);
    syncur_drop((int)a0);
    cgroup_dir_drop((int)a0);
    signalfd_shim_closed((int)a0);
    timerfd_shim_closed((int)a0);
    pidfd_shim_closed((int)a0);
    snd_forget_fd((int)a0);
    evdev_forget_fd((int)a0);
    // A synthetic DRM render-node fd MUST leave the registry when it is closed. Without this the
    // fd number stays "owned" forever, so the next regular file to reuse that number (glibc's
    // dlopen of libGLESv2 did exactly this) is misclassified: its mmap() is routed to drm_mmap_bo
    // instead of gmem_mmap and fails EINVAL -> "failed to map segment from shared object".
    drm_forget_fd((int)a0);
    net_fd_closed((int)a0);            // drop from every epoll/kqueue (Linux does this on close)
    {
      long cr = herr(close((int)a0));
      if (getenv("RDEXECDBG") && (int)a0 >= 10 && (int)a0 < 20) {
        static int ncl;
        if (ncl < 100) { ncl++;
          int fl = fcntl((int)a0, F_GETFL);
          FILE* f = fopen("/tmp/rd_exec_dbg.txt", "a");
          if (f) { fprintf(f, "pid=%d CLOSE fd=%d ret=%ld recheck_fl=%d\n", (int)getpid(), (int)a0, cr, fl); fclose(f); }
        }
      }
      return cr;
    }

  // ---------- 61 getdents64 ----------
  case 61: {
    int fd = (int)a0;
    if (fd < 0 || shim_fd_reserved(fd)) return -LX_EBADF;
    if (!a1) return -LX_EFAULT;
    // A guest ELF/JIT output may not be writable through the host mapping.
    // Preflight before changing either the synthetic or host DIR cursor.
    size_t cap = (size_t)a2;
    if (cap > 1024 * 1024) cap = 1024 * 1024; // legal short result
    if (cap && gmem_guest_range_writable(a1, cap) != 0)
      return -LX_EFAULT;
    unsigned char *staged = malloc(cap ? cap : 1);
    if (!staged) return -LX_ENOMEM;
    long n = do_getdents64(fd, staged, cap);
    if (n > 0 && gmem_guest_copyout_checked(a1, staged, (size_t)n) != 0)
      n = -LX_EFAULT; // Mapping may change after enumeration.
    free(staged);
    return n;
  }

  // ---------- 62 lseek ----------
  case 62: {
    int w = lseek_whence_l2m((int)a2);
    if (w < 0) return -LX_EINVAL;
    char dpath[GPATH_MAX] = {0};
    if (cgroup_dir_path((int)a0, dpath, sizeof dpath) != 0)
      (void)guest_path_of_fd((int)a0, dpath, sizeof dpath);
    if (dpath[0]) {
      int count = 0;
      if (synth_entries(dpath, &count))
        return syncur_lseek((int)a0, (int64_t)a1, w, count);
    }
    off_t o = lseek((int)a0, (off_t)a1, w);
    return o < 0 ? herrno() : (long)o;
  }

  // ---------- 63 read / 64 write ----------
  case 63: {
    if (getenv("RDREADDBG")) {
      int fl = fcntl((int)a0, F_GETFL);
      struct stat st; int t = fstat((int)a0, &st) == 0 ? st.st_mode : 0;
      if (t != 0 || 1) {
        static int nr; if (nr < 800) { nr++;
          FILE* f = fopen("/tmp/rd_read_dbg.txt", "a");
          if (f) { fprintf(f, "pid=%d READ fd=%ld n=%ld nonblock=%d type=%s\n",
                           (int)getpid(), a0, (long)a2, fl>=0 ? (fl&O_NONBLOCK)!=0 : -1,
                           S_ISFIFO(t)?"pipe":S_ISSOCK(t)?"sock":S_ISREG(t)?"file":"other"); fclose(f); }
        }
      }
    }
    // A guest ELF/JIT destination may not be writable through the host VM
    // mapping. Check it before consuming a pipe/socket, then stage the read
    // and copy only bytes the host actually returned.
    size_t read_count = (size_t)a2;
    if (read_count > 1024 * 1024) return -LX_EOPNOTSUPP;
    if (read_count && gmem_guest_range_writable(a1, read_count) != 0)
      return -LX_EFAULT;
    char *read_staged = read_count ? malloc(read_count) : NULL;
    if (read_count && !read_staged) return -LX_ENOMEM;
    long r = herr((long)read((int)a0, read_staged, read_count));
    if (r > 0 && gmem_guest_copyout_checked(a1, read_staged, (size_t)r) != 0)
      r = -LX_EFAULT; // A guest mapping may change after the preflight.
    if (getenv("RDREADDBG") && (int)a0 == 69) {
      FILE* f = fopen("/tmp/rd_read_dbg.txt", "a");
      if (f) { fprintf(f, "pid=%d READ69 ret=%ld\n", (int)getpid(), r); fclose(f); }
    }
    if (getenv("RDDESERDBG") && (int)a0 == 74 && r > 0) {
      FILE* f = fopen("/tmp/rd_fd74.bin", "a");
      if (f) { fwrite(read_staged, 1, (size_t)r, f); fclose(f); }
    }
    if (getenv("RDEXECDBG") && (int)a0 >= 60) {
      static int nread;
      if (nread < 200) { nread++;
        FILE* f = fopen("/tmp/rd_exec_dbg.txt", "a");
        if (f) { fprintf(f, "pid=%d READ fd=%ld n=%ld ret=%ld\n", (int)getpid(), a0, (long)a2, r); fclose(f); }
      }
    }
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
    free(read_staged);
    return r;
  }
  case 64: {
    // Guest ELF/JIT bytes are not always readable by Darwin's write path.
    // Snapshot the complete request before one host write, preserving record
    // and pipe atomicity instead of emitting a partial prefix on bad input.
    if (a2 > SSIZE_MAX) return -LX_EINVAL;
    if (a2 > 1024 * 1024) return -LX_EOPNOTSUPP;
    if (a2 && !a1) return -LX_EFAULT;
    char* staged = NULL;
    if (a2) {
      staged = malloc((size_t)a2);
      if (!staged) return -LX_ENOMEM;
      if (gmem_guest_export_copyin(NULL, a1, staged, (size_t)a2) != 0) {
        free(staged);
        return -LX_EFAULT;
      }
    }
    long r = herr((long)write((int)a0, staged, (size_t)a2));
    if (getenv("RDWRDUMP") && (int)a0 < 32 && r > 0) {
      const char* dump_path = getenv("RDWRDUMPFILE");
      FILE* df = fopen(dump_path ? dump_path : "/tmp/rd_wrdump.txt", "a");
      if (df) {
        fprintf(df, "write fd=%ld n=%ld: \"", a0, r);
        for (long j = 0; j < r; j++) {
          unsigned char c = (unsigned char)staged[j];
          fputc(c >= 0x20 && c < 0x7f ? c : '.', df);
        }
        fprintf(df, "\"\n");
        fclose(df);
      }
    }
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
    free(staged);
    return r;
  }

  // ---------- 65 readv / 66 writev ----------
  case 65: {
    // Host readv cannot safely write through guest ELF/JIT pointers. Admit
    // every destination before consuming fd data, read once into host-owned
    // buffers, then copy only the returned prefix to checked guest ranges.
    if (a2 > 1024) return -LX_EINVAL; // Linux UIO_MAXIOV
    if (!a2) return herr((long)readv((int)a0, NULL, 0));
    if (!a1) return -LX_EFAULT;
    struct iovec guest[1024], host[1024];
    size_t nvec = (size_t)a2;
    if (gmem_guest_export_copyin(NULL, a1, guest, nvec * sizeof guest[0]) != 0)
      return -LX_EFAULT;
    size_t requested = 0;
    for (size_t i = 0; i < nvec; ++i) {
      if (guest[i].iov_len > SSIZE_MAX - requested) return -LX_EINVAL;
      requested += guest[i].iov_len;
    }
    // A truncated datagram read would lose the rest of that message. Refuse
    // large requests instead of treating an unknown fd as a byte stream.
    if (requested > 1024 * 1024) return -LX_EOPNOTSUPP;
    for (size_t i = 0; i < nvec; ++i)
      if (guest[i].iov_len &&
          gmem_guest_range_writable((uint64_t)(uintptr_t)guest[i].iov_base,
                                    guest[i].iov_len) != 0)
        return -LX_EFAULT;
    if (!requested) return herr((long)readv((int)a0, NULL, 0));
    char* staged = malloc(requested);
    if (!staged) return -LX_ENOMEM;
    size_t nhost = 0, offset = 0;
    for (size_t i = 0; i < nvec; ++i) {
      if (!guest[i].iov_len) continue;
      host[nhost].iov_base = staged + offset;
      host[nhost++].iov_len = guest[i].iov_len;
      offset += guest[i].iov_len;
    }
    long r = herr((long)readv((int)a0, host, (int)nhost));
    if (r > 0) {
      size_t copied = 0;
      for (size_t i = 0; i < nvec && copied < (size_t)r; ++i) {
        size_t part = guest[i].iov_len;
        if (part > (size_t)r - copied) part = (size_t)r - copied;
        if (!part) continue;
        if (gmem_guest_copyout_checked((uint64_t)(uintptr_t)guest[i].iov_base,
                                       staged + copied, part) != 0) {
          free(staged);
          return copied ? (long)copied : -LX_EFAULT;
        }
        copied += part;
      }
    }
    free(staged);
    return r;
  }
  case 66: {
    // The guest iovec layout matches Darwin's, but guest ELF/JIT payloads
    // cannot necessarily be read by the host writev kernel path. Snapshot
    // every byte we will submit into admitted host memory before writing.
    // One host writev keeps pipe/socket record and atomic-write behavior.
    if (a2 > 1024) return -LX_EINVAL; // Linux UIO_MAXIOV
    if (!a2) return herr((long)writev((int)a0, NULL, 0));
    if (!a1) return -LX_EFAULT;
    struct iovec guest[1024], host[1024];
    size_t nvec = (size_t)a2;
    if (gmem_guest_export_copyin(NULL, a1, guest, nvec * sizeof guest[0]) != 0)
      return -LX_EFAULT;
    size_t requested = 0;
    for (size_t i = 0; i < nvec; ++i) {
      if (guest[i].iov_len > SSIZE_MAX - requested) return -LX_EINVAL;
      requested += guest[i].iov_len;
    }
    // A partial datagram/seqpacket send would change the message itself.
    // Refuse the bound for every fd instead of guessing that it is a stream.
    if (requested > 1024 * 1024) return -LX_EOPNOTSUPP;
    size_t count = requested;
    if (!count) return herr((long)writev((int)a0, NULL, 0));
    char* staged = malloc(count);
    if (!staged) return -LX_ENOMEM;
    size_t gathered = 0, nhost = 0;
    for (size_t i = 0; i < nvec && gathered < count; ++i) {
      size_t part = guest[i].iov_len;
      if (part > count - gathered) part = count - gathered;
      if (!part) continue;
      if (gmem_guest_export_copyin(NULL, (uint64_t)(uintptr_t)guest[i].iov_base,
                                   staged + gathered, part) != 0) {
        free(staged);
        return -LX_EFAULT;
      }
      host[nhost].iov_base = staged + gathered;
      host[nhost++].iov_len = part;
      gathered += part;
    }
    long r = herr((long)writev((int)a0, host, (int)nhost));
    if (getenv("RDWRDUMP") && (int)a0 < 32 && r > 0) {
      const char* dump_path = getenv("RDWRDUMPFILE");
      FILE* df = fopen(dump_path ? dump_path : "/tmp/rd_wrdump.txt", "a");
      if (df) {
        fprintf(df, "writev fd=%ld n=%ld: \"", a0, r);
        for (long i = 0; i < r; ++i) {
          unsigned char c = (unsigned char)staged[i];
          fputc(c >= 0x20 && c < 0x7f ? c : '.', df);
        }
        fprintf(df, "\"\n");
        fclose(df);
      }
    }
    free(staged);
    return r;
  }

  // ---------- 67 pread64 / 68 pwrite64 ----------
  case 67: { // pread64: stage regular-file output before checked guest copyout
    int fd = (int)a0;
    int64_t pos = (int64_t)a3; // ARM64 carries the full signed offset in x3
    if (pos < 0) return -LX_EINVAL;
    if (fd < 0 || shim_fd_reserved(fd)) return -LX_EBADF;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || (fl & O_ACCMODE) == O_WRONLY) return -LX_EBADF;
    struct stat st;
    if (fstat(fd, &st) < 0) return herrno();
    if (!S_ISREG(st.st_mode)) return -LX_ESPIPE;
    size_t count = (size_t)a2;
    if (!count) return 0;
    if (!a1) return -LX_EFAULT;
    if (count > 1024 * 1024) return -LX_EOPNOTSUPP;
    if (pos > INT64_MAX - (int64_t)count) return -LX_EINVAL;
    if (gmem_guest_range_writable(a1, count) != 0) return -LX_EFAULT;
    char* staged = malloc(count);
    if (!staged) return -LX_ENOMEM;
    long r = herr((long)pread(fd, staged, count, (off_t)pos));
    if (r > 0 && gmem_guest_copyout_checked(a1, staged, (size_t)r) != 0)
      r = -LX_EFAULT; // mapping could change after preflight
    free(staged);
    return r;
  }
  case 68: { // pwrite64: export guest bytes before any host write
    int fd = (int)a0;
    int64_t pos = (int64_t)a3;
    if (pos < 0) return -LX_EINVAL;
    if (fd < 0 || shim_fd_reserved(fd)) return -LX_EBADF;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || (fl & O_ACCMODE) == O_RDONLY) return -LX_EBADF;
    if (fl & O_APPEND) return -LX_EOPNOTSUPP;
    struct stat st;
    if (fstat(fd, &st) < 0) return herrno();
    if (!S_ISREG(st.st_mode)) return -LX_ESPIPE;
    size_t count = (size_t)a2;
    if (!count) return 0;
    if (!a1) return -LX_EFAULT;
    if (count > 1024 * 1024) return -LX_EOPNOTSUPP;
    if (pos > INT64_MAX - (int64_t)count) return -LX_EINVAL;
    char* staged = malloc(count);
    if (!staged) return -LX_ENOMEM;
    if (gmem_guest_export_copyin(NULL, a1, staged, count) != 0) {
      free(staged);
      return -LX_EFAULT;
    }
    ssize_t n = pwrite(fd, staged, count, (off_t)pos);
    int saved_errno = errno;
    free(staged);
    errno = saved_errno;
    return herr((long)n);
  }

  // ---------- 78 readlinkat ----------
  case 78: {
    size_t sz = (size_t)a3;
    if (!a2 || (long)sz <= 0) return -LX_EINVAL;
    char guest_path[GPATH_MAX];
    r = readlink_guest_path(a1, guest_path, sizeof guest_path);
    if (r < 0) return r;
    if ((r = resolve_f((int)a0, guest_path, RSV_NOFOLLOW, host, sizeof host,
                       gabs, sizeof gabs)) < 0)
      return r;
    if (strcmp(gabs, "/proc/self/exe") == 0)
      return readlink_guest_text(a2, sz, g_exe);
    if (strcmp(gabs, "/proc/self/cwd") == 0)
      return readlink_guest_text(a2, sz, g_cwd);
    // /proc/self/fd/<N> and /proc/<pid>/fd/<N> — readlink of the fd-reopen magic link returns the
    // fd's target. systemd's fdset_new_fill() readlink()s every /proc/self/fd entry to learn where
    // each fd points; returning the real path (or a synthetic anon_inode target for pathless fds)
    // keeps that enumeration from stalling on ENOENT.
    {
      int pfd = proc_fd_number(gabs);
      if (pfd >= 0) {
        int tty = vt_number_for_fd(pfd);
        if (tty) {
          char t[24];
          snprintf(t, sizeof t, "/dev/tty%d", tty);
          return readlink_guest_text(a2, sz, t);
        }
        char hp[PATH_MAX];
        if (fcntl(pfd, F_GETPATH, hp) == 0) {
          const char* t = hp;
          if (g_rootlen && strncmp(hp, g_root, g_rootlen) == 0 && hp[g_rootlen] == '/')
            t = hp + g_rootlen;                    // report in guest coordinates
          else if (strncmp(hp, "/private/", 9) == 0)
            t = hp + 8;                            // macOS /tmp,/var,/etc symlink canonical form
          return readlink_guest_text(a2, sz, t);
        }
        const char* t = "anon_inode:[shim]";       // socket/pipe/memfd: no path
        return readlink_guest_text(a2, sz, t);
      }
    }
    // /sys/dev/char/226:*/device/subsystem — libdrm's drmParseSubsystemType() readlink()s this to
    // learn the bus type; report "/pci" so the GPU is treated as a PCI device.
    if (drm_sysfs_kind(gabs) == 4) {
      const char* t = drm_sysfs_link();
      return readlink_guest_text(a2, sz, t);
    }
    // /sys/class/drm/<name>/subsystem — libudev's subsystem probe for the synthetic DRM card.
    if (drm_sysfs_kind(gabs) == 11) {
      const char* t = drm_sysfs_class_link();
      return readlink_guest_text(a2, sz, t);
    }
    if (drm_sysfs_kind(gabs) == 12) {
      const char* t = strstr(gabs, "226:128") ? "../../class/drm/renderD128" :
                                                    "../../class/drm/card0";
      return readlink_guest_text(a2, sz, t);
    }
    // /sys/class/sound/card0/subsystem — libudev's subsystem probe for the synthetic sound card.
    if (snd_sysfs_kind(gabs) == 3) {
      const char* t = snd_sysfs_link();
      return readlink_guest_text(a2, sz, t);
    }
    // /sys/class/input/eventN/subsystem — libudev's subsystem probe for the synthetic input device.
    if (evdev_sysfs_kind(gabs) == 3 || evdev_sysfs_kind(gabs) == 10) {
      const char* t = evdev_sysfs_link();
      return readlink_guest_text(a2, sz, t);
    }
    char staged[GPATH_MAX];
    // Read the full bounded target before applying the guest's output cap:
    // an absolute host-root prefix may need stripping even for a tiny buffer.
    ssize_t n = readlink(host, staged, sizeof staged);
    if (n < 0) return herrno();
    // A symlink target that points into the rootfs must be reported in guest coordinates.
    if (g_rootlen && (size_t)n > g_rootlen && strncmp(staged, g_root, g_rootlen) == 0 &&
        staged[g_rootlen] == '/') {
      memmove(staged, staged + g_rootlen, (size_t)n - g_rootlen);
      n -= (ssize_t)g_rootlen;
    }
    return readlink_guest_result(a2, sz, staged, (size_t)n);
  }

  // ---------- 79 newfstatat / 80 fstat ----------
  case 79: {
    struct stat st;
    struct linux_stat staged = {0};
    struct linux_stat* out = &staged;
    if (!a2) return -LX_EFAULT;
    // NULL is valid only for the existing AT_EMPTY_PATH fd form. For every
    // pathname form, snapshot guest bytes before either the empty-path test
    // or the resolver dereferences them.
    char guest_path[PATH_MAX];
    const char* p = NULL;
    if (a1) {
      if ((r = readlink_guest_path(a1, guest_path, sizeof guest_path)) < 0) return r;
      p = guest_path;
    }
    if (!p && !(a3 & LX_AT_EMPTY_PATH)) return -LX_EFAULT;
    if ((a3 & LX_AT_EMPTY_PATH) && (!p || !*p)) {
      int fd = (int)a0;
      if (!net_opath_socket_stat(fd, &st) && fstat(fd, &st) < 0) return herrno();
      if (stat_m2l(&st, out)<0) return herrno();
      // No path was resolved here. The former gabs lookup read an uninitialized
      // buffer; use the registered synthetic fd identity instead.
      unsigned fake_mode = out->st_mode;
      unsigned long fake_rdev = (unsigned long)out->st_rdev;
      if (drm_owns_fd(fd))
        drm_fake_stat(&fake_mode, &fake_rdev,
                      drm_minor_of_fd(fd) == 0 ? "/dev/dri/card0" : "/dev/dri/renderD128");
      if (cam_owns_fd(fd)) cam_fake_stat(&fake_mode, &fake_rdev);
      if (snd_owns_fd(fd)) snd_fake_stat(&fake_mode, &fake_rdev, "controlC0");
      if (evdev_owns_fd(fd)) {
        int dev = evdev_device_of_fd(fd);
        evdev_fake_stat(&fake_mode, &fake_rdev,
                        dev == EVDEV_DEV_PTR ? "event1" : "event0");
      }
      if (vt_owns_fd(fd)) {
        char tty[24];
        snprintf(tty, sizeof tty, "/dev/tty%d", vt_number_for_fd(fd));
        vt_fake_stat(&fake_mode, &fake_rdev, tty);
        vt_stat_identity(tty, &out->st_dev, &out->st_ino);
      }
      out->st_mode = fake_mode;
      out->st_rdev = fake_rdev;
      goto stat79_copyout;
    }
    if ((r = resolve_f((int)a0, p, (a3 & LX_AT_SYMLINK_NOFOLLOW) ? RSV_NOFOLLOW : 0,
                       host, sizeof host, gabs, sizeof gabs)) < 0) return r;
    if (strcmp(gabs, "/proc/self/exe") == 0) guest_to_host(g_exe, host, sizeof host);
    if (strcmp(gabs, "/proc/self/mounts") == 0) {
      memset(out, 0, sizeof *out);
      out->st_mode = S_IFREG | 0444;
      out->st_nlink = 1; out->st_uid = 0; out->st_gid = 0;
      out->st_blksize = 4096; out->st_ino = 3;
      goto stat79_copyout;
    }
    // Synthetic sysfs nodes under /sys/dev/char/226:*/ — answer from drmasahi.c, not the host fs.
    if (drm_sysfs_kind(gabs)) {
      memset(out, 0, sizeof *out);
      drm_fake_stat(&out->st_mode, &out->st_rdev, gabs);
      if (drm_sysfs_kind(gabs) == 12 && !(a3 & LX_AT_SYMLINK_NOFOLLOW))
        out->st_mode = S_IFDIR | 0755;
      goto stat79_copyout;
    }
    // Synthetic /sys/class/sound/* and /run/udev/data/c116:0 — the ALSA card's sysfs/udev surface.
    if (snd_sysfs_kind(gabs)) {
      memset(out, 0, sizeof *out);
      snd_fake_stat(&out->st_mode, &out->st_rdev, gabs);
      goto stat79_copyout;
    }
    // Synthetic /sys/class/input/* and /run/udev/data/c13:* — the evdev input surface.
    if (evdev_sysfs_kind(gabs)) {
      memset(out, 0, sizeof *out);
      evdev_fake_stat(&out->st_mode, &out->st_rdev, gabs);
      goto stat79_copyout;
    }
    // /dev/dri/card0 and /dev/dri/renderD128 are synthetic char nodes; the host rootfs only has
    // empty placeholder files so libdrm's opendir("/dev/dri") has something to list. Fake their
    // stat() as the 226:0 / 226:128 char device drmGetDevices2() requires.
    if (drm_is_node(gabs)) {
      memset(out, 0, sizeof *out);
      drm_fake_stat(&out->st_mode, &out->st_rdev, gabs);
      goto stat79_copyout;
    }
    // /dev/input/eventN — synthetic evdev char nodes (major 13).
    if (evdev_is_node(gabs)) {
      memset(out, 0, sizeof *out);
      evdev_fake_stat(&out->st_mode, &out->st_rdev, gabs);
      goto stat79_copyout;
    }
    // /dev/snd/* — synthetic ALSA char nodes (major 116).
    if (snd_is_node(gabs)) {
      memset(out, 0, sizeof *out);
      snd_fake_stat(&out->st_mode, &out->st_rdev, gabs);
      goto stat79_copyout;
    }
    // /dev/ttyN / /dev/console / /dev/tty0 — virtual consoles (major 4).
    if (vt_is_node(gabs)) {
      memset(out, 0, sizeof *out);
      vt_fake_stat(&out->st_mode, &out->st_rdev, gabs);
      vt_stat_identity(gabs, &out->st_dev, &out->st_ino);
      goto stat79_copyout;
    }
    // cgroup v2 files/dirs are synthesized; the host rootfs has none of them, so stat them from
    // the in-memory tree (dirs as S_IFDIR, control files as S_IFREG with the served size).
    if (cgroup_under(gabs)) {
      if (cgroup_file_name(gabs)) {
        char cbuf[2048];
        const char* c = cgroup_file_content(gabs, cbuf, sizeof cbuf);
        memset(out, 0, sizeof *out);
        out->st_mode = S_IFREG | 0644;
        out->st_size = c ? (int64_t)strlen(c) : 0;
        out->st_nlink = 1; out->st_uid = 0; out->st_gid = 0; out->st_blksize = 4096; out->st_ino = 1;
        goto stat79_copyout;
      }
      if (cgroup_is_dir(gabs)) {
        memset(out, 0, sizeof *out);
        out->st_mode = S_IFDIR | 0755;
        out->st_nlink = 2; out->st_uid = 0; out->st_gid = 0; out->st_blksize = 4096; out->st_ino = 2;
        goto stat79_copyout;
      }
      return -LX_ENOENT;
    }
    int rc = (a3 & LX_AT_SYMLINK_NOFOLLOW) ? lstat(host, &st) : stat(host, &st);
    if (rc < 0) {
      if (getenv("RDEXECDBG")) {
        FILE* f = fopen("/tmp/rd_exec_dbg.txt", "a");
        if (f) { fprintf(f, "pid=%d STAT-FAIL dirfd=%ld gabs=%s host=%s errno=%d\n",
                         (int)getpid(), (long)a0, gabs, host, errno); fclose(f); }
      }
      return herrno();
    }
    // RDERRDBG=1: name the /dev|/run path that lstat() reports as a non-symlink, which is what
    // makes udev-node's node_create_symlink() return SYNTHETIC_ERRNO(EEXIST).
    if (getenv("RDERRDBG") && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) &&
        ((a3 & LX_AT_SYMLINK_NOFOLLOW)) &&
        (!strncmp(gabs, "/dev", 4) || !strncmp(gabs, "/run/udev", 9))) {
      FILE* f = fopen("/tmp/rd_errno_dbg.txt", "a");
      if (f) { fprintf(f, "pid=%d STAT-NONLNK path=%s mode=%#o\n", (int)getpid(), gabs, st.st_mode); fclose(f); }
    }
    if (stat_m2l(&st, out)<0) return herrno();
  stat79_copyout:
    return gmem_guest_copyout_checked(a2, out, sizeof *out) == 0
               ? 0 : -LX_EFAULT;
  }
  case 80: {
    struct stat st;
    struct linux_stat out;
    if (!a1) return -LX_EFAULT;
    if (!net_opath_socket_stat((int)a0, &st) && fstat((int)a0, &st) < 0) return herrno();
    if (stat_m2l(&st, &out)<0) return herrno();
    if (drm_owns_fd((int)a0)) {
      drm_fake_stat(&out.st_mode, &out.st_rdev,
                    drm_minor_of_fd((int)a0) == 0 ? "/dev/dri/card0" : "/dev/dri/renderD128");
    }
    if (snd_owns_fd((int)a0)) {
      snd_fake_stat(&out.st_mode, &out.st_rdev, "controlC0");
    }
    if (evdev_owns_fd((int)a0)) {
      int dev = evdev_device_of_fd((int)a0);
      evdev_fake_stat(&out.st_mode, &out.st_rdev, dev == EVDEV_DEV_PTR ? "event1" : "event0");
    }
    if (vt_owns_fd((int)a0)) {
      char tty[24];
      snprintf(tty, sizeof tty, "/dev/tty%d", vt_number_for_fd((int)a0));
      vt_fake_stat(&out.st_mode, &out.st_rdev, tty);
    }
    return gmem_guest_copyout_checked(a1, &out, sizeof out) == 0 ? 0 : -LX_EFAULT;
  }

  // ---------- 93 exit / 94 exit_group ----------
  // exit() ends the CALLING THREAD; exit_group() ends the process. That distinction only started
  // to matter once clone() worked: musl's pthread_exit issues SYS_exit, and terminating the whole
  // process there would kill the program the moment any thread finished.
  case 93:
    FX("%.3f EXIT(thread) tid=%d code=%d vfork=%d child=%d\n",
       fx_now(), guest_tid(), (int)a0, guest_is_vfork_child(), guest_is_child());
    if (getenv("RDEXECDBG"))
      fprintf(stderr, "[shim-exit] exit(thread) tid=%d code=%d vfork=%d child=%d pid=%d\n",
              guest_tid(), (int)a0, guest_is_vfork_child(), guest_is_child(), (int)getpid());
    if (guest_is_vfork_child()) guest_vfork_finish(-1, (int)a0);   // never returns
    if (guest_is_child()) {
      guest_thread_exiting();          // CLONE_CHILD_CLEARTID + futex wake, so join() completes
      fastcall_thread_cleanup();
      pthread_exit(NULL);
    }
    fflush(NULL);
    _exit((int)a0);
  case 94:
    FX("%.3f EXIT_GROUP tid=%d code=%d vfork=%d child=%d\n",
       fx_now(), guest_tid(), (int)a0, guest_is_vfork_child(), guest_is_child());
    if (fx_on()) fflush(g_fx_f);
    if (getenv("RDEXECDBG")) {
      fprintf(stderr, "[shim-exit] exit_group tid=%d code=%d vfork=%d child=%d pid=%d\n",
              guest_tid(), (int)a0, guest_is_vfork_child(), guest_is_child(), (int)getpid());
      FILE* xf = fopen("/tmp/rd_exec_dbg.txt", "a");
      if (xf) { fprintf(xf, "pid=%d EXIT-GROUP code=%d vfork=%d child=%d\n",
                        (int)getpid(), (int)a0, guest_is_vfork_child(), guest_is_child()); fclose(xf); }
    }
    // A CLONE_VM|CLONE_VFORK child (glibc posix_spawn) runs as a THREAD sharing this process.
    // When its exec fails it calls _exit()/exit_group(127) — but that must end the CHILD's
    // process (which the blocked vfork parent is waiting on), NOT tear down the whole shim
    // (systemd PID 1) with it. Route it through guest_vfork_finish so the parent wakes with
    // the child's status, exactly as the exit() case above already does.
    if (guest_is_vfork_child()) guest_vfork_finish(-1, (int)a0);   // never returns
    fflush(NULL);
    _exit((int)a0);

  // ---------- 96 set_tid_address ----------
  case 96: guest_set_tid_address(a0); return (long)guest_tid();

  // ---------- 98 futex ----------
  case 98: return do_futex(a0, (int)a1, (unsigned int)a2,
                           (const struct linux_timespec*)a3, a4, (unsigned int)a5);

  // ---------- 449 futex_waitv (one private U32 waiter) ----------
  case 449: return do_futex_waitv(a0, (unsigned int)a1, (unsigned int)a2, a3, (int)a4);

  // ---------- 454 futex_wake (futex2) ----------
  case 454: return do_futex_wake2(a0, a1, (int)a2, (unsigned int)a3);

  // ---------- 455 futex_wait (futex2) ----------
  case 455: return do_futex_wait2(a0, a1, a2, (unsigned int)a3, a4, (int)a5);

  // ---------- 456 futex_requeue (futex2) ----------
  case 456: return do_futex_requeue2(a0, (unsigned int)a1, (int)a2, (int)a3);

  // ---------- 99 set_robust_list ----------
  case 99: return guest_set_robust_list(a0, a1);

  // ---------- 113 clock_gettime ----------
  case 113: {
    int mc = clockid_l2m((int)a0);
    if (mc < 0) return -LX_EINVAL;
    if (!a1) return -LX_EFAULT;
    struct timespec t;
    if (clock_gettime((clockid_t)mc, &t) < 0) return herrno();
    struct linux_timespec lt = {.tv_sec=t.tv_sec, .tv_nsec=t.tv_nsec};
    return gmem_guest_copyout_checked(a1, &lt, sizeof lt) == 0 ? 0 : -LX_EFAULT;
  }

  // ---------- 114 clock_getres ----------
  case 114: {
    int mc = clockid_l2m((int)a0);
    if (mc < 0) return -LX_EINVAL;
    struct timespec t;
    if (clock_getres((clockid_t)mc, &t) < 0) return herrno();
    if (a1) {
      struct linux_timespec lt = {.tv_sec=t.tv_sec, .tv_nsec=t.tv_nsec};
      if (gmem_guest_copyout_checked(a1, &lt, sizeof lt) != 0) return -LX_EFAULT;
    }
    return 0;
  }

  // ---------- 115 clock_nanosleep ----------
  case 115: {
    if ((int)a1 & ~LX_TIMER_ABSTIME) return -LX_EINVAL;
    if ((int)a0 == LX_CLOCK_THREAD_CPUTIME_ID) return -LX_EINVAL;
    // Sleeping wall time does not advance a CPU clock; accepting these IDs
    // would return success before the requested CPU time had elapsed.
    if ((int)a0 == LX_CLOCK_PROCESS_CPUTIME_ID ||
        (int)a0 == LX_CLOCK_MONOTONIC_RAW ||
        (int)a0 == LX_CLOCK_REALTIME_COARSE ||
        (int)a0 == LX_CLOCK_MONOTONIC_COARSE) return -LX_EOPNOTSUPP;
    int mc = clockid_l2m((int)a0);
    if (mc < 0) return -LX_EINVAL;
    struct linux_timespec req;
    if (!a2 || gmem_guest_export_copyin(NULL, a2, &req, sizeof req) != 0)
      return -LX_EFAULT;
    if (req.tv_nsec < 0 || req.tv_nsec >= 1000000000L || req.tv_sec < 0)
      return -LX_EINVAL;
    struct timespec want = { (time_t)req.tv_sec, (long)req.tv_nsec };
    if ((int)a1 & LX_TIMER_ABSTIME) {
      struct timespec now;
      if (clock_gettime((clockid_t)mc, &now) < 0) return herrno();
      if (req.tv_sec < now.tv_sec ||
          (req.tv_sec == now.tv_sec && req.tv_nsec <= now.tv_nsec)) return 0;
      want.tv_sec  -= now.tv_sec;
      want.tv_nsec -= now.tv_nsec;
      if (want.tv_nsec < 0) { want.tv_nsec += 1000000000L; want.tv_sec--; }
    }
    struct timespec rem;
    if (nanosleep(&want, &rem) < 0) {
      int saved = errno;
      if (saved == EINTR && !((int)a1 & LX_TIMER_ABSTIME) && a3) {
        struct linux_timespec remaining = {.tv_sec=rem.tv_sec, .tv_nsec=rem.tv_nsec};
        if (gmem_guest_copyout_checked(a3, &remaining, sizeof remaining) != 0)
          return -LX_EFAULT;
      }
      return -linux_errno_from_host(saved);
    }
    return 0;
  }

  // ---------- 122 sched_setaffinity / 123 sched_getaffinity ----------
  case 122: {
    long target = (long)(int)a0;
    if (target != 0 && target != guest_tid())
      return guest_tid_known((int)target) ? -95 : -LX_ESRCH;
    // Darwin does not offer Linux's arbitrary CPU bitmask pinning. Accept the
    // unrestricted mask (the host scheduler already uses all online CPUs),
    // but never report success for a restriction we did not apply.
    size_t sz = (size_t)a1;
    unsigned char mask[128];
    if (!a2) return -LX_EFAULT;
    if (sz < 8) return -LX_EINVAL;
    if (sz > sizeof mask) return -95; // bounded cpumask_t emulation
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    if (ncpu > 32) ncpu = 32; // match the guest's /sys and status CPU topology
    if ((size_t)((ncpu + 7) / 8) > sz) return -LX_EINVAL;
    if (gmem_guest_export_copyin(NULL, a2, mask, sz) != 0) return -LX_EFAULT;
    for (long i = 0; i < ncpu && i < 1024; ++i)
      if (!(mask[i / 8] & (1u << (i % 8)))) return -95; // EOPNOTSUPP
    return 0;
  }
  case 123: {
    long target = (long)(int)a0;
    if (target != 0 && target != guest_tid())
      return guest_tid_known((int)target) ? -95 : -LX_ESRCH;
    size_t sz = (size_t)a1;
    // The virtual topology exposes at most 32 online CPUs. Linux's raw
    // syscall copies a kernel-sized mask and returns its byte count, not the
    // size supplied by the caller. AArch64 masks use 64-bit words.
    if (sz < 8 || (sz & 7)) return -LX_EINVAL;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    if (ncpu > 32) ncpu = 32;
    uint64_t mask = (UINT64_C(1) << ncpu) - 1;
    if (gmem_guest_copyout_checked(a2, &mask, sizeof mask) != 0) return -LX_EFAULT;
    return sizeof mask;
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
  // Group/all-process selectors need a guest process-group registry. Sending
  // only to this shim is not equivalent, and kill(-1, sig) on Linux excludes
  // the caller. Refuse these forms rather than delivering a different signal.
  //
  // Positive pids are allowed only for our virtual self PID or a child this
  // shim spawned. In guest init, self is PID 1, not the Darwin getpid() value.
  case 129: {
    int ms = sig_l2m((int)a1);
    if (ms < 0) return -LX_EINVAL;
    // NB: guest TIDs are deliberately NOT accepted here, unlike in tkill/tgkill. kill(2) names a
    // PROCESS; treating a thread id as a process id is the same conflation from the other side,
    // and it would make kill(1, sig) "succeed" purely because guest tid 1 is the main thread.
    long tgt = (long)(int)a0;
    if (tgt <= 0) return -LX_EOPNOTSUPP;
    if (tgt == syscalls_guest_pid())    return herr(kill(getpid(), ms));
    if (guest_child_known((int)tgt))    return herr(kill((pid_t)tgt, ms));
    return -LX_ESRCH;                                         // a pid from a namespace we did not issue
  }

  // ---------- 134 rt_sigaction ----------
  // Recorded only: the shim runs the guest under a Mach exception handler and never delivers
  // host signals into guest code, so installing a real handler would be actively wrong.
  case 134: {
    int sig = (int)a0;
    if (sig < 1 || sig > 64) return -LX_EINVAL;
    if (a3 != 8) return -LX_EINVAL; // native aarch64 sigsetsize
    struct linux_sigaction next;
    if (a1 && gmem_guest_export_copyin(NULL, a1, &next, sizeof next) != 0)
      return -LX_EFAULT;
    if (a1 && sig == g_pdeathsig && next.handler > 1) return -LX_ENOSYS;
    struct linux_sigaction prev = g_sigacts[sig];
    // Copy out before updating the disposition. Bad oldact memory must not
    // leave the new disposition installed; stage act first if they alias.
    if (a2 && gmem_guest_copyout_checked(a2, &prev, sizeof prev) != 0)
      return -LX_EFAULT;
    if (a1) {
      g_sigacts[sig] = next;
      if (next.handler == 1)
        __atomic_fetch_and(&g_sigpending, ~(1ULL << (sig - 1)), __ATOMIC_ACQ_REL);
    }
    return 0;
  }

  // ---------- 135 rt_sigprocmask ----------
  case 135: {
    int howv = (int)a0;                         // SIG_BLOCK 0, SIG_UNBLOCK 1, SIG_SETMASK 2
    if (a3 != 8) return -LX_EINVAL;
    if (a1 && (howv < 0 || howv > 2)) return -LX_EINVAL;
    uint64_t set = 0;
    if (a1 && gmem_guest_export_copyin(NULL, a1, &set, sizeof set) != 0)
      return -LX_EFAULT;
    uint64_t old = __atomic_load_n(&g_sigmask, __ATOMIC_ACQUIRE);
    // Check and write the old mask before mutating state. A bad output pointer
    // must not leave a changed mask behind; stage input first for aliasing.
    if (a2 && gmem_guest_copyout_checked(a2, &old, sizeof old) != 0)
      return -LX_EFAULT;
    if (a1) {
      set &= ~((1ULL << (9 - 1)) | (1ULL << (19 - 1)));
      if      (howv == 0) __atomic_fetch_or(&g_sigmask, set, __ATOMIC_ACQ_REL);
      else if (howv == 1) __atomic_fetch_and(&g_sigmask, ~set, __ATOMIC_ACQ_REL);
      else                __atomic_store_n(&g_sigmask, set, __ATOMIC_RELEASE);
      virtual_signal_check_parent();
      virtual_signal_deliver_unblocked();
    }
    return 0;
  }

  // ---------- 137 rt_sigtimedwait ----------
  // Consume supported virtual pending standard signals, including parent death.
  case 137: {
    if (!a0) return -LX_EFAULT;
    if (a3 != 8) return -LX_EINVAL;
    uint64_t wanted;
    if (gmem_guest_export_copyin(NULL, a0, &wanted, sizeof wanted) != 0)
      return -LX_EFAULT;
    // SIGKILL and SIGSTOP cannot be caught, blocked or synchronously waited for.
    wanted &= ~((1ULL << (9 - 1)) | (1ULL << (19 - 1)));
    struct linux_timespec timeout;
    const struct linux_timespec* to = NULL;
    if (a2) {
      if (gmem_guest_export_copyin(NULL, a2, &timeout, sizeof timeout) != 0)
        return -LX_EFAULT;
      to = &timeout;
    }
    if (to && (to->tv_sec < 0 || to->tv_nsec < 0 || to->tv_nsec >= 1000000000L))
      return -LX_EINVAL;
    struct timespec begin;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (;;) {
      virtual_signal_check_parent();
      int signo = virtual_signal_take(wanted);
      if (signo) {
        if (a1) {
          unsigned char info[128] = {0}; // Linux aarch64 siginfo_t
          memcpy(info, &signo, sizeof signo);
          if (gmem_guest_copyout_checked(a1, info, sizeof info) != 0) {
            // A bad destination must not silently consume the signal. This
            // standard-signal queue coalesces repeated instances by design.
            __atomic_fetch_or(&g_sigpending, 1ULL << (signo - 1), __ATOMIC_ACQ_REL);
            return -LX_EFAULT;
          }
        }
        return signo;
      }
      struct timespec sleep_for = {0, 20000000}; // bounded parent-liveness poll
      if (to) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed = (int64_t)(now.tv_sec - begin.tv_sec) * 1000000000LL +
                          (int64_t)(now.tv_nsec - begin.tv_nsec);
        int64_t total = to->tv_sec > INT64_MAX / 1000000000LL ? INT64_MAX :
                        (int64_t)to->tv_sec * 1000000000LL + to->tv_nsec;
        if (elapsed >= total) return -LX_EAGAIN;
        int64_t left = total - elapsed;
        if (left < sleep_for.tv_nsec) sleep_for.tv_nsec = (long)left;
      }
      if (nanosleep(&sleep_for, NULL) < 0 && errno == EINTR) return -LX_EINTR;
    }
  }

  // ---------- 139 rt_sigreturn ----------
  // No guest signal frame or saved register context exists to restore. A raw
  // guest syscall can reach this case, so success would falsely claim a
  // return from a handler. Keep it unavailable until signal frames exist.
  case 139: return -LX_ENOSYS;

  // ---------- 153 times ----------
  case 153: {
    struct rusage ru, rc;
    if (getrusage(RUSAGE_SELF, &ru) < 0) return herrno();
    if (a0) {
      if (getrusage(RUSAGE_CHILDREN, &rc) < 0) return herrno();
      struct linux_tms t = {
        .tms_utime = ru.ru_utime.tv_sec * LX_USER_HZ + ru.ru_utime.tv_usec / (1000000 / LX_USER_HZ),
        .tms_stime = ru.ru_stime.tv_sec * LX_USER_HZ + ru.ru_stime.tv_usec / (1000000 / LX_USER_HZ),
        .tms_cutime = rc.ru_utime.tv_sec * LX_USER_HZ + rc.ru_utime.tv_usec / (1000000 / LX_USER_HZ),
        .tms_cstime = rc.ru_stime.tv_sec * LX_USER_HZ + rc.ru_stime.tv_usec / (1000000 / LX_USER_HZ),
      };
      if (gmem_guest_copyout_checked(a0, &t, sizeof t) != 0) return -LX_EFAULT;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return herrno();
    return now.tv_sec * LX_USER_HZ + now.tv_nsec / (1000000000L / LX_USER_HZ);
  }

  // ---------- 160 uname ----------
  case 160: {
    if (!a0) return -LX_EFAULT;
    struct linux_utsname u = {0};
    snprintf(u.sysname,    sizeof u.sysname,    "%s", "Linux");
    long e = guest_uts_gethostname(u.nodename);
    if (e < 0) return e;
    snprintf(u.release,    sizeof u.release,    "%s", "6.1.0");
    snprintf(u.version,    sizeof u.version,    "%s", "#1 SMP PREEMPT r-drift shim");
    snprintf(u.machine,    sizeof u.machine,    "%s", "aarch64");
    e = guest_uts_getdomainname(u.domainname);
    if (e < 0) return e;
    return gmem_guest_copyout_checked(a0, &u, sizeof u) == 0 ? 0 : -LX_EFAULT;
  }

  // ---------- 165 getrusage ----------
  case 165: {
    int who = (int)(long)a0;                    // SELF 0, CHILDREN -1, THREAD 1
    // Darwin has no RUSAGE_THREAD getrusage selector. Reporting process-wide
    // usage for that Linux selector would mislead thread-aware applications.
    if (who == 1) return -LX_EOPNOTSUPP;
    if (who != 0 && who != -1) return -LX_EINVAL;
    if (!a1) return -LX_EFAULT;
    int mwho = who == -1 ? RUSAGE_CHILDREN : RUSAGE_SELF;
    struct rusage ru;
    if (getrusage(mwho, &ru) < 0) return herrno();
    struct linux_rusage out;
    rusage_m2l(&ru, &out);
    return gmem_guest_copyout_checked(a1, &out, sizeof out) == 0 ? 0 : -LX_EFAULT;
  }

  // ---------- 172..178 creds ----------
  // PID-1 emulation: the init process (/sbin/init) must see itself as pid 1 (ppid 0) so systemd
  // takes the "system manager" path. Every other guest process keeps the real host pid. getppid()
  // must return the GUEST parent pid (1 for a direct child of init), not the host's: systemd's
  // safe_fork_full() checks getppid()==original_pid right after forking to detect "the parent died
  // before PR_SET_PDEATHSIG took", and a host ppid (the shim's own parent) made that check fire,
  // raise(SIGTERM) in the child and abort the generator sandbox with a bogus -ENOSYS.
  case 172: return g_is_init ? 1 : (long)getpid();
  case 173: return g_guest_ppid;
  // The guest is the host's uid (501 here), not 0. A Linux container answers this with a user
  // namespace and a uid map; macOS has none, but the shim IS the guest's kernel, so it can map.
  // RDUID0=1 reports 0/0. OPT-IN, and deliberately so: the tree is owned by the real uid and every
  // file operation still runs as that uid, so this changes what the guest BELIEVES, not what it may
  // do. Some programs only test `id -u` and then do work the real uid can already perform
  // (dictionaries-common's update-dictcommon-* refuse outright); those are fixed by it. Others
  // branch on uid 0 into chown()/chmod() they would otherwise skip -- dpkg does exactly that, which
  // is why `dpkg --force-not-root` is the better answer there and this is not the default.
  case 174: return g_uid0 ? (long)guest_cred_uid(0) : (long)getuid();
  case 175: return g_uid0 ? (long)guest_cred_uid(1) : (long)geteuid();
  case 176: return g_uid0 ? (long)guest_cred_gid(0) : (long)getgid();
  case 177: return g_uid0 ? (long)guest_cred_gid(1) : (long)getegid();
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
    if (!a0) return -LX_EFAULT;
    // Preserve the existing synthetic fields, but never write them through a
    // guest address without the guest-memory ledger checking the destination.
    struct linux_sysinfo si = {0};
    struct timespec up;
    clock_gettime(CLOCK_MONOTONIC, &up);
    si.uptime = up.tv_sec;
    uint64_t memsize = 0; size_t msz = sizeof memsize;
    if (sysctlbyname("hw.memsize", &memsize, &msz, 0, 0) != 0) memsize = 8ULL << 30;
    double la[3] = {0, 0, 0};
    if (getloadavg(la, 3) == 3)
      for (int i = 0; i < 3; i++) si.loads[i] = (uint64_t)(la[i] * 65536.0);
    si.mem_unit  = 1;
    si.totalram  = memsize;
    si.freeram   = memsize / 4;
    si.sharedram = 0;
    si.bufferram = 0;
    si.totalswap = 0;
    si.freeswap  = 0;
    si.procs     = 1;
    return gmem_guest_copyout_checked(a0, &si, sizeof si) == 0 ? 0 : -LX_EFAULT;
  }

  // ---------- 214 brk ----------
  case 214: return do_brk(a0);

  // ---------- 215 munmap ----------
  case 215: return gmem_munmap(a0, (size_t)a1);

  // ---------- 216 mremap ----------
  case 216: {
    unsigned long old = a0; size_t olds = (size_t)a1, news = (size_t)a2;
    unsigned long flags = a3;
    const size_t PG = 16384;
    // Validate before changing any mapping. The clone form (old_size=0), FIXED,
    // and DONTUNMAP need backing/protection metadata that this path does not keep.
    if (!olds) return -LX_EOPNOTSUPP;
    if (!news || (old & (PG - 1)) || (flags & ~1ul))
      return (flags & ~1ul) ? -LX_EOPNOTSUPP : -LX_EINVAL;
    if (olds > SIZE_MAX - (PG - 1) || news > SIZE_MAX - (PG - 1))
      return -LX_EINVAL;
    size_t oldr = (olds + PG - 1) & ~(PG - 1);
    size_t newr = (news + PG - 1) & ~(PG - 1);
    if (old > ULONG_MAX - oldr || old > ULONG_MAX - newr)
      return -LX_EINVAL;
    if (gmem_guest_range_mapped(old, oldr) != 0) return -LX_EFAULT;
    if (newr <= oldr) {
      if (newr < oldr) {
        long unmap = gmem_munmap(old + newr, oldr - newr);
        if (unmap < 0) return unmap;
      }
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
      fprintf(stderr, "[mremap] grow %#lx %#zx -> %#zx flags=%#lx\n", old, oldr, newr, flags);
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
  case 233: {
    /* Linux and Darwin use the same values for the four nondestructive
     * access-pattern hints. Keep the guest range check in front of the host
     * call: a host mapping is not necessarily part of the guest address space.
     * Do not acknowledge DONTNEED/FREE/fork/THP/etc. without their Linux
     * data or VMA semantics. */
    int advice = (int)a2;
    switch (advice) {
      case 0: case 1: case 2: case 3: break;
      case 4: case 8: case 9: case 10: case 11: case 12: case 13:
      case 14: case 15: case 16: case 17: case 18: case 19:
      case 20: case 21: case 22: case 23: case 24: case 25:
      case 100: case 101: case 102: case 103:
        return -LX_EOPNOTSUPP;
      default: return -LX_EINVAL;
    }
    const uint64_t page = (uint64_t)getpagesize();
    if (a0 & (page - 1)) return -LX_EINVAL;
    if (a1 > UINT64_MAX - (page - 1)) return -LX_EINVAL;
    uint64_t rounded = (a1 + page - 1) & ~(page - 1);
    if (a0 > UINT64_MAX - rounded) return -LX_EINVAL;
    if (!rounded) return 0; // Linux treats a valid zero-length hint as a no-op
    if (rounded > SIZE_MAX ||
        gmem_guest_range_mapped(a0, (size_t)rounded) != 0)
      return -LX_ENOMEM;
    return madvise((void *)(uintptr_t)a0, (size_t)rounded, advice) == 0
               ? 0 : herrno();
  }

  // ---------- 260 wait4 ----------
  case 260: {
    int lopt = (int)a2;
    if (lopt & ~(1 | 2 | 8)) return -LX_EINVAL;
    int mopt = 0;
    if (lopt & 1) mopt |= WNOHANG;
    if (lopt & 2) mopt |= WUNTRACED;
    if (lopt & 8) mopt |= WCONTINUED;
    // Probe the guest outputs before consuming a child's status. An EFAULT
    // after wait4 has reaped the child cannot be retried by userspace.
    int old_status = 0;
    struct linux_rusage old_usage;
    if (a1 && (gmem_guest_export_copyin(NULL, a1, &old_status, sizeof old_status) != 0 ||
               gmem_guest_copyout_checked(a1, &old_status, sizeof old_status) != 0))
      return -LX_EFAULT;
    if (a3 && (gmem_guest_export_copyin(NULL, a3, &old_usage, sizeof old_usage) != 0 ||
               gmem_guest_copyout_checked(a3, &old_usage, sizeof old_usage) != 0))
      return -LX_EFAULT;
    long selector = (long)(int)a0;
    pid_t target;
    if (selector > 0) {
      if (!guest_child_known((int)selector)) return -LX_ECHILD;
      target = (pid_t)selector;
    } else if (selector == -1) {
      // Darwin's P_ALL may include a shim-owned helper. Peek without reaping,
      // then wait for that exact child only if the guest registry issued it.
      siginfo_t si = {0};
      int events = WEXITED | (lopt & 2 ? WSTOPPED : 0) |
                   (lopt & 8 ? WCONTINUED : 0) | WNOWAIT |
                   (lopt & 1 ? WNOHANG : 0);
      if (waitid(P_ALL, 0, &si, events) != 0) return herrno();
      if (!si.si_pid) return 0;
      if (!guest_child_known(si.si_pid)) return -LX_EOPNOTSUPP;
      target = si.si_pid;
    } else {
      // Group selectors need a guest process-group registry. Passing them to
      // Darwin could wait for a different host group or reap the wrong child.
      return -LX_EOPNOTSUPP;
    }
    int st = 0;
    struct rusage ru;
    pid_t p = wait4(target, &st, mopt, a3 ? &ru : 0);
    if (p < 0) return herrno();
    if (p > 0) syscalls_signal_child_reaped(p);
    if (p > 0 && a1 && gmem_guest_copyout_checked(a1, &st, sizeof st) != 0)
      return -LX_EFAULT;
    if (p > 0 && a3) {
      struct linux_rusage out;
      rusage_m2l(&ru, &out);
      if (gmem_guest_copyout_checked(a3, &out, sizeof out) != 0) return -LX_EFAULT;
    }
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
    /* The host CSPRNG is ready before a guest runs.  Its secure output
     * satisfies the default and NONBLOCK urandom modes, but it cannot model
     * the separate RANDOM/INSECURE sources.  Linux permits a short result
     * above 256 bytes; keeping this call to one 256-byte chunk also avoids
     * unbounded host work on a guest-supplied length. */
    unsigned flags = (unsigned)a2;
    if (flags & ~7u) return -LX_EINVAL;
    if ((flags & 6u) == 6u) return -LX_EINVAL;
    if (flags & 6u) return -LX_EOPNOTSUPP; /* RANDOM or INSECURE */
    if (!a1) return 0;
    if (!a0) return -LX_EFAULT;
    size_t n = a1 < 256 ? (size_t)a1 : 256;
    unsigned char bytes[256];
    arc4random_buf(bytes, n);
    if (gmem_guest_copyout_checked(a0, bytes, n) != 0)
      return -LX_EFAULT;
    return (long)n;
  }

  // ---------- 293 rseq ----------
  case 293: return -LX_ENOSYS;                  // no restartable sequences under the shim

  default:
    if (g_trace) { fprintf(stderr, "[sc] -> ext nr=%lu\n", nr); fflush(stderr); }
    return linux_syscall_ext(nr, a0, a1, a2, a3, a4, a5);
  }
}
