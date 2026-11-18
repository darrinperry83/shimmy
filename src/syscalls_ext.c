#include "guest_ownership.h"
// syscalls_ext.c — extended/rare Linux aarch64 syscalls (the FALLBACK tier).
//
// linux_syscall() (syscalls.c) owns the hot core set and tail-calls us for everything else.
// Our job is the long tail: creds, process/session, the rest of the fs surface, memory hygiene,
// and the modern odds and ends (memfd_create, statx, renameat2, close_range, ...), so that a
// real musl/glibc program never trips over a surprise -ENOSYS during init.
//
// ABI (identical to linux_syscall): return the RAW Linux value — >= 0 on success, or a
// NEGATIVE LINUX errno. Never -1/errno. Host errnos are translated with linux_errno_from_host().
//
// Struct marshalling is NOT optional in several places, because macOS and Linux disagree:
//   - `struct timeval`: Linux tv_usec is 64-bit, macOS is 32-bit  -> gettimeofday/itimer/rusage
//     must be filled field-by-field, never memcpy'd.
//   - `stack_t`:        member ORDER differs (Linux sp/flags/size vs macOS sp/size/flags).
//   - `siginfo_t`, `struct statx`, `struct rlimit` (RLIM_INFINITY), AT_*, O_*, MS_*, W*, signal
//     numbers and RLIMIT_* indices all differ in value and are translated below.
// `struct timespec` and `struct iovec` DO match on arm64 and are passed through.
//
// Dangerous-or-meaningless-on-macOS calls (mount, chroot, kexec, acct, reboot, module loading)
// return a plausible -EPERM instead of failing weirdly or touching the host.
void fx_note(const char* fmt, ...);
#include "drmasahi.h"
#include "v4l2cam.h"
#include "snd_alsa.h"
#include "evdev.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <signal.h>
#include <poll.h>
#include <sched.h>
#include <pthread.h>
#include "guestthread.h"
#include "execve.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/event.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/select.h>
#include <sys/syslimits.h>
#include <sys/xattr.h>

#include "syscalls.h"       // linux_errno_from_host()
#include "net.h"
#include "syscalls_ext.h"
#include "signalfd_shim.h"
#include "timerfd_shim.h"
#include "posix_timer_shim.h"
#include "pidfd_shim.h"
#include "gmem.h"
#include "inotify_watch.h"
#include <sys/socket.h>
#include <sys/un.h>

extern void net_fd_closed(int fd);   // net.c — drop fd from every epoll/kqueue
extern void rd_rtnl_dup(int oldfd, int newfd);

// ---------------------------------------------------------------------------
// Linux constants (the guest's values — do NOT use the host headers' versions)
// ---------------------------------------------------------------------------
#define LX_EPERM           1
#define LX_ENOENT          2
#define LX_ESRCH           3
#define LX_EINTR           4
#define LX_EIO             5
#define LX_E2BIG           7
#define LX_EBADF           9
#define LX_ECHILD         10
#define LX_EAGAIN         11
#define LX_ENOMEM         12
#define LX_EACCES         13
#define LX_EFAULT         14
#define LX_EINVAL         22
#define LX_ERANGE         34
#define LX_ENAMETOOLONG   36
#define LX_ENOSYS         38
#define LX_ELOOP          40
#define LX_EOPNOTSUPP     95

#define LX_AT_FDCWD              (-100)
#define LX_AT_SYMLINK_NOFOLLOW   0x100
#define LX_AT_EACCESS            0x200
#define LX_AT_REMOVEDIR          0x200
#define LX_AT_SYMLINK_FOLLOW     0x400
#define LX_AT_NO_AUTOMOUNT       0x800
#define LX_AT_EMPTY_PATH         0x1000

#define LX_O_NONBLOCK   0x800
#define LX_O_CLOEXEC    0x80000

#define LX_O_NONBLOCK_ 0x800
#define LX_O_CLOEXEC_  0x80000

// clone/prctl/memfd
#define LX_MFD_CLOEXEC        0x0001
#define LX_MFD_ALLOW_SEALING  0x0002
#define LX_MFD_HUGETLB        0x0004
#define LX_MFD_NOEXEC_SEAL    0x0008   /* Linux 6.3 */
#define LX_MFD_EXEC           0x0010   /* Linux 6.3 */

#define LX_PR_SET_PDEATHSIG    1
#define LX_PR_GET_PDEATHSIG    2
#define LX_PR_GET_DUMPABLE     3
#define LX_PR_SET_DUMPABLE     4
#define LX_PR_GET_KEEPCAPS     7
#define LX_PR_SET_KEEPCAPS     8
#define LX_PR_SET_NAME         15
#define LX_PR_GET_NAME         16
#define LX_PR_GET_SECCOMP      21
#define LX_PR_SET_SECCOMP      22
#define LX_PR_CAPBSET_READ     23
#define LX_PR_CAPBSET_DROP     24
#define LX_PR_GET_SECUREBITS   27
#define LX_PR_SET_SECUREBITS   28
#define LX_PR_SET_TIMERSLACK   29
#define LX_PR_GET_TIMERSLACK   30
#define LX_PR_SET_NO_NEW_PRIVS 38
#define LX_PR_GET_NO_NEW_PRIVS 39
#define LX_PR_CAP_AMBIENT      47
#define LX_PR_SET_THP_DISABLE  41
#define LX_PR_GET_THP_DISABLE  42
#define LX_PR_SET_MDWE         65
#define LX_PR_GET_MDWE         66
#define LX_PR_SET_MEMORY_MERGE 67
#define LX_PR_GET_MEMORY_MERGE 68
#define LX_PR_SET_VMA          0x53564d41
#define LX_PR_SET_PTRACER      0x59616d61

#define LX_MS_ASYNC       1
#define LX_MS_INVALIDATE  2
#define LX_MS_SYNC        4

#define LX_WNOHANG      0x00000001
#define LX_WUNTRACED    0x00000002
#define LX_WSTOPPED     0x00000002
#define LX_WEXITED      0x00000004
#define LX_WCONTINUED   0x00000008
#define LX_WNOWAIT      0x01000000

#define LX_P_PIDFD      3

#define LX_FALLOC_KEEP_SIZE   0x01
#define LX_FALLOC_PUNCH_HOLE  0x02

#define LX_RENAME_NOREPLACE 1
#define LX_RENAME_EXCHANGE  2

#define LX_UTIME_NOW  0x3fffffff
#define LX_UTIME_OMIT 0x3ffffffe

#define LX_RLIM_INFINITY  (~0ULL)

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static long eret(void) { return -linux_errno_from_host(errno); }
static long ok0(int r)  { return r == 0 ? 0 : eret(); }

// macOS __PAGEZERO spans the whole low 4 GB; nothing the guest can dereference lives there.
#define FOUR_GB 0x100000000UL

// Is every page of [addr, addr+len) actually mapped in this process? Walked with mach_vm_region,
// which reports the first region at or above an address: if that region starts ABOVE the address
// we asked about, the address itself is in a hole. Used to give mincore() Linux's ENOMEM
// semantics — see the comment at syscall 232 for why that matters.
static int host_range_mapped(unsigned long addr, size_t len) {
  unsigned long end = addr + len;
  if (end < addr) return 0;                                   // overflow
  mach_vm_address_t a = (mach_vm_address_t)addr;
  while (a < (mach_vm_address_t)end) {
    mach_vm_address_t ra = a;
    mach_vm_size_t rs = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&info, &cnt, &obj);
    if (kr != KERN_SUCCESS) return 0;                         // nothing mapped at or above
    if (ra > a) return 0;                                     // hole at `a`
    if (rs == 0) return 0;                                    // no progress; treat as unmapped
    a = ra + rs;
  }
  return 1;
}

// Linux `struct timeval` — tv_usec is 64-bit here, unlike macOS.
struct lx_timeval { long long tv_sec; long long tv_usec; };
struct lx_itimerval { struct lx_timeval it_interval; struct lx_timeval it_value; };
struct lx_timespec { long long tv_sec; long long tv_nsec; };   // matches macOS arm64

static void lx_tv_put(struct lx_timeval* d, const struct timeval* s) {
  d->tv_sec = (long long)s->tv_sec; d->tv_usec = (long long)s->tv_usec;
}
static void lx_tv_get(struct timeval* d, const struct lx_timeval* s) {
  d->tv_sec = (time_t)s->tv_sec; d->tv_usec = (suseconds_t)s->tv_usec;
}

// Linux `struct rusage` — same BSD field order as macOS, but with Linux timevals.
struct lx_rusage {
  struct lx_timeval ru_utime, ru_stime;
  long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap;
  long ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw;
};
static void lx_rusage_put(struct lx_rusage* d, const struct rusage* s) {
  memset(d, 0, sizeof *d);
  lx_tv_put(&d->ru_utime, &s->ru_utime);
  lx_tv_put(&d->ru_stime, &s->ru_stime);
  d->ru_maxrss = s->ru_maxrss; d->ru_ixrss = s->ru_ixrss; d->ru_idrss = s->ru_idrss;
  d->ru_isrss = s->ru_isrss;   d->ru_minflt = s->ru_minflt; d->ru_majflt = s->ru_majflt;
  d->ru_nswap = s->ru_nswap;   d->ru_inblock = s->ru_inblock; d->ru_oublock = s->ru_oublock;
  d->ru_msgsnd = s->ru_msgsnd; d->ru_msgrcv = s->ru_msgrcv; d->ru_nsignals = s->ru_nsignals;
  d->ru_nvcsw = s->ru_nvcsw;   d->ru_nivcsw = s->ru_nivcsw;
}

// Linux `struct tms` (clock ticks; Linux USER_HZ is 100).
struct lx_tms { long tms_utime, tms_stime, tms_cutime, tms_cstime; };
#define LX_USER_HZ 100

// Linux aarch64 `struct shmid64_ds` (asm-generic/shmbuf.h) and the `ipc64_perm` it embeds.
// This is what the guest's musl hands to shmctl(IPC_STAT), and it is NOT the macOS shape:
// macOS puts shm_lpid/shm_cpid/shm_nattch BEFORE the timestamps and makes nattch a short, so a
// raw pass-through would report a segment size read out of the middle of a pid pair. That is why
// IPC_STAT used to be refused here. Refusing is not survivable, though -- see the call site.
//
// Offsets are pinned by the static asserts below rather than trusted to the compiler's padding.
struct lx_ipc64_perm {
  int            key;
  unsigned       uid, gid, cuid, cgid;
  unsigned       mode;
  unsigned short seq, __pad2;
  unsigned long long __unused1, __unused2;
};
struct lx_shmid64_ds {
  struct lx_ipc64_perm shm_perm;
  unsigned long long   shm_segsz;
  long long            shm_atime, shm_dtime, shm_ctime;
  int                  shm_cpid, shm_lpid;
  unsigned long long   shm_nattch, __unused4, __unused5;
};
_Static_assert(sizeof(struct lx_ipc64_perm)  == 48,  "linux ipc64_perm is 48 bytes");
_Static_assert(sizeof(struct lx_shmid64_ds)  == 112, "linux shmid64_ds is 112 bytes");
_Static_assert(offsetof(struct lx_shmid64_ds, shm_segsz) == 48, "shm_segsz at 48");
_Static_assert(offsetof(struct lx_shmid64_ds, shm_cpid)  == 80, "shm_cpid at 80");
_Static_assert(offsetof(struct lx_shmid64_ds, shm_nattch) == 88, "shm_nattch at 88");

// Field-by-field by NAME, never by memcpy: the two layouts agree on nothing past the first word.
static void lx_shmid_put(struct lx_shmid64_ds* d, const struct shmid_ds* s) {
  memset(d, 0, sizeof *d);
  d->shm_perm.key  = (int)s->shm_perm._key;
  d->shm_perm.uid  = (unsigned)s->shm_perm.uid;
  d->shm_perm.gid  = (unsigned)s->shm_perm.gid;
  d->shm_perm.cuid = (unsigned)s->shm_perm.cuid;
  d->shm_perm.cgid = (unsigned)s->shm_perm.cgid;
  d->shm_perm.mode = (unsigned)s->shm_perm.mode;
  d->shm_perm.seq  = (unsigned short)s->shm_perm._seq;
  d->shm_segsz     = (unsigned long long)s->shm_segsz;
  d->shm_atime     = (long long)s->shm_atime;
  d->shm_dtime     = (long long)s->shm_dtime;
  d->shm_ctime     = (long long)s->shm_ctime;
  d->shm_cpid      = (int)s->shm_cpid;
  d->shm_lpid      = (int)s->shm_lpid;
  d->shm_nattch    = (unsigned long long)(unsigned short)s->shm_nattch;  // macOS: short
}

// Linux aarch64 `struct msqid64_ds` (asm-generic/msgbuf.h) and the ipc64_perm it embeds.
// Same translation story as shmid64_ds above: Darwin's msqid_ds orders cbytes/qnum/qbytes/
// lspid/lrpid BEFORE the timestamps and pads differently, so msgctl(IPC_STAT) must be filled
// field-by-field. Offsets are pinned by static asserts, not trusted to compiler padding.
struct lx_msqid64_ds {
  struct lx_ipc64_perm msg_perm;
  long long            msg_stime, msg_rtime, msg_ctime;
  unsigned long long   msg_cbytes, msg_qnum, msg_qbytes;
  int                  msg_lspid, msg_lrpid;
  unsigned long long   __unused4, __unused5;
};
_Static_assert(sizeof(struct lx_msqid64_ds)  == 120, "linux msqid64_ds is 120 bytes");
_Static_assert(offsetof(struct lx_msqid64_ds, msg_stime)  == 48, "msg_stime at 48");
_Static_assert(offsetof(struct lx_msqid64_ds, msg_cbytes)  == 72, "msg_cbytes at 72");
_Static_assert(offsetof(struct lx_msqid64_ds, msg_qnum)    == 80, "msg_qnum at 80");
_Static_assert(offsetof(struct lx_msqid64_ds, msg_qbytes)  == 88, "msg_qbytes at 88");
_Static_assert(offsetof(struct lx_msqid64_ds, msg_lspid)   == 96, "msg_lspid at 96");
_Static_assert(offsetof(struct lx_msqid64_ds, msg_lrpid)   == 100, "msg_lrpid at 100");

// Field-by-field by NAME. Darwin's msqid_ds and Linux's msqid64_ds agree on nothing past msg_perm.
static void lx_msqid_put(struct lx_msqid64_ds* d, const struct msqid_ds* s) {
  memset(d, 0, sizeof *d);
  d->msg_perm.key  = (int)s->msg_perm._key;
  d->msg_perm.uid  = (unsigned)s->msg_perm.uid;
  d->msg_perm.gid  = (unsigned)s->msg_perm.gid;
  d->msg_perm.cuid = (unsigned)s->msg_perm.cuid;
  d->msg_perm.cgid = (unsigned)s->msg_perm.cgid;
  d->msg_perm.mode = (unsigned)s->msg_perm.mode;
  d->msg_perm.seq  = (unsigned short)s->msg_perm._seq;
  d->msg_stime     = (long long)s->msg_stime;
  d->msg_rtime     = (long long)s->msg_rtime;
  d->msg_ctime     = (long long)s->msg_ctime;
  d->msg_cbytes    = (unsigned long long)s->msg_cbytes;
  d->msg_qnum      = (unsigned long long)s->msg_qnum;
  d->msg_qbytes    = (unsigned long long)s->msg_qbytes;
  d->msg_lspid     = (int)s->msg_lspid;
  d->msg_lrpid     = (int)s->msg_lrpid;
}

// IPC_SET direction: Linux honors only msg_perm.{uid,gid,mode} and msg_qbytes, which is also the
// subset Darwin honors, so those are the only fields that need to survive the trip back.
static void lx_msqid_get(struct msqid_ds* d, const struct lx_msqid64_ds* s) {
  memset(d, 0, sizeof *d);
  d->msg_perm.uid  = (uid_t)s->msg_perm.uid;
  d->msg_perm.gid  = (gid_t)s->msg_perm.gid;
  d->msg_perm.cuid = (uid_t)s->msg_perm.cuid;
  d->msg_perm.cgid = (gid_t)s->msg_perm.cgid;
  d->msg_perm.mode = (mode_t)(s->msg_perm.mode & 0xffff);
  d->msg_qbytes    = (msglen_t)s->msg_qbytes;
}

// ---------------------------------------------------------------------------
// Signal number translation (Linux -> macOS). They diverge badly above 6.
// ---------------------------------------------------------------------------
static int host_signo(int lx) {
  static const signed char m[32] = {
    /* 0*/  0,
    /* 1*/  SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT,
    /* 7*/  SIGBUS,          // Linux 7 = SIGBUS (macOS 7 is SIGEMT!)
    /* 8*/  SIGFPE, SIGKILL,
    /*10*/  SIGUSR1,         // Linux 10 = USR1 -> macOS 30
    /*11*/  SIGSEGV,
    /*12*/  SIGUSR2,         // Linux 12 = USR2 -> macOS 31
    /*13*/  SIGPIPE, SIGALRM, SIGTERM,
    /*16*/  -1,              // Linux SIGSTKFLT: no macOS equivalent
    /*17*/  SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU,
    /*23*/  SIGURG, SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF, SIGWINCH,
    /*29*/  SIGIO,
    /*30*/  -1,              // Linux SIGPWR: no macOS equivalent
    /*31*/  SIGSYS,
  };
  if (lx == 0) return 0;
  if (lx > 0 && lx < 32) return m[lx];
  return -1;                 // real-time signals 32..64: nothing to map to
}

// ---------------------------------------------------------------------------
// Rootfs path resolution (same convention as syscalls.c: guest-absolute -> <root><path>)
// ---------------------------------------------------------------------------
static char g_root[PATH_MAX];
static size_t g_rootlen;

void syscalls_ext_set_root(const char* root) {
  if (!root) { g_root[0] = 0; g_rootlen = 0; return; }
  snprintf(g_root, sizeof g_root, "%s", root);
  g_rootlen = strlen(g_root);
  while (g_rootlen > 1 && g_root[g_rootlen - 1] == '/') g_root[--g_rootlen] = 0;
}

// Resolve a guest path into `out`. Delegates to syscalls.c's syscalls_resolve() — the ONE
// resolver — so these syscalls get guest-side symlink resolution and rootfs containment for free.
//
// This file used to carry a second, independent copy of the bare `<root><path>` concatenation.
// That is how the absolute-symlink escape survived: the fix had to land in two places and only
// ever landed in one (shim.c, for execve). There is now a single implementation; do not add a
// third. `nf` = leave a symlink in the final component alone.
static long resolve_path_nf(const char* gp, char* out, size_t outsz, int nf) {
  if (!gp) return -LX_EFAULT;
  return syscalls_resolve(LX_AT_FDCWD, gp, nf, out, outsz, 0, 0);
}
static long resolve_path(const char* gp, char* out, size_t outsz) {
  return resolve_path_nf(gp, out, outsz, 0);
}

// Resolve a (dirfd, path) pair. The result is always an ABSOLUTE host path, so *hdirfd is always
// AT_FDCWD: relative guest paths are resolved against the GUEST cwd that syscalls.c tracks, not
// against the shim's host cwd, which is what the old copy here did and which diverged from
// syscalls.c on every relative path.
#define resolve_at(d, p, o, n, h) resolve_at_nf((d), (p), (o), (n), (h), 0)
static long resolve_at_nf(int lx_dirfd, const char* gp, char* out, size_t outsz, int* hdirfd,
                          int nf) {
  if (!gp) return -LX_EFAULT;
  long r = syscalls_resolve(lx_dirfd, gp, nf, out, outsz, 0, 0);
  if (r < 0) return r;
  *hdirfd = AT_FDCWD;
  return 0;
}

// Snapshot a bounded guest string before handing it to a host pathname/xattr API.
// A bad guest address must return EFAULT instead of crashing the shim process.
static long copy_guest_cstr(uint64_t addr, char* out, size_t cap) {
  if (!addr) return -LX_EFAULT;
  for (size_t i = 0; i < cap; ++i) {
    if (gmem_guest_export_copyin(NULL, addr + i, out + i, 1) != 0)
      return -LX_EFAULT;
    if (!out[i]) return 0;
  }
  return -LX_ENAMETOOLONG;
}

static int host_at_flags(int lxf) {
  int f = 0;
  if (lxf & LX_AT_SYMLINK_NOFOLLOW) f |= AT_SYMLINK_NOFOLLOW;
  if (lxf & LX_AT_SYMLINK_FOLLOW)   f |= AT_SYMLINK_FOLLOW;
  return f;
}

// ---------------------------------------------------------------------------
// struct statx (Linux uapi) — 256 bytes, offsets are ABI and asserted by the test.
// ---------------------------------------------------------------------------
struct lx_statx_timestamp { long long tv_sec; unsigned tv_nsec; int __reserved; };
struct lx_statx {
  unsigned stx_mask;                 /*   0 */
  unsigned stx_blksize;              /*   4 */
  unsigned long long stx_attributes; /*   8 */
  unsigned stx_nlink;                /*  16 */
  unsigned stx_uid;                  /*  20 */
  unsigned stx_gid;                  /*  24 */
  unsigned short stx_mode;           /*  28 */
  unsigned short __spare0[1];        /*  30 */
  unsigned long long stx_ino;        /*  32 */
  unsigned long long stx_size;       /*  40 */
  unsigned long long stx_blocks;     /*  48 */
  unsigned long long stx_attributes_mask; /* 56 */
  struct lx_statx_timestamp stx_atime;    /*  64 */
  struct lx_statx_timestamp stx_btime;    /*  80 */
  struct lx_statx_timestamp stx_ctime;    /*  96 */
  struct lx_statx_timestamp stx_mtime;    /* 112 */
  unsigned stx_rdev_major;           /* 128 */
  unsigned stx_rdev_minor;           /* 132 */
  unsigned stx_dev_major;            /* 136 */
  unsigned stx_dev_minor;            /* 140 */
  unsigned long long stx_mnt_id;     /* 144 */
  unsigned stx_dio_mem_align;        /* 152 */
  unsigned stx_dio_offset_align;     /* 156 */
  unsigned long long __spare3[12];   /* 160 .. 256 */
};
_Static_assert(sizeof(struct lx_statx) == 256, "Linux struct statx is 256 bytes");
_Static_assert(offsetof(struct lx_statx, stx_nlink) == 16, "statx ABI");
_Static_assert(offsetof(struct lx_statx, stx_mode)  == 28, "statx ABI");
_Static_assert(offsetof(struct lx_statx, stx_ino)   == 32, "statx ABI");
_Static_assert(offsetof(struct lx_statx, stx_size)  == 40, "statx ABI");
_Static_assert(offsetof(struct lx_statx, stx_atime) == 64, "statx ABI");
_Static_assert(offsetof(struct lx_statx, stx_mtime) == 112, "statx ABI");
_Static_assert(offsetof(struct lx_statx, stx_dev_major) == 136, "statx ABI");

#define LX_STATX_TYPE        0x0001U
#define LX_STATX_MODE        0x0002U
#define LX_STATX_NLINK       0x0004U
#define LX_STATX_UID         0x0008U
#define LX_STATX_GID         0x0010U
#define LX_STATX_ATIME       0x0020U
#define LX_STATX_MTIME       0x0040U
#define LX_STATX_CTIME       0x0080U
#define LX_STATX_INO         0x0100U
#define LX_STATX_SIZE        0x0200U
#define LX_STATX_BLOCKS      0x0400U
#define LX_STATX_BASIC_STATS 0x07ffU
#define LX_STATX_BTIME       0x0800U
#define LX_STATX_MNT_ID      0x1000U
#define LX_STATX_MNT_ID_UNIQUE 0x4000U
#define LX_STATX_ATTR_MOUNT_ROOT 0x2000U

static void statx_ts(struct lx_statx_timestamp* d, const struct timespec* s) {
  d->tv_sec = (long long)s->tv_sec; d->tv_nsec = (unsigned)s->tv_nsec; d->__reserved = 0;
}

static long do_statx(int lx_dirfd, const char* gpath, int lxflags, unsigned mask, void* ubuf) {
  struct lx_statx* sx = (struct lx_statx*)ubuf;
  if (!sx) return -LX_EFAULT;
  struct stat st;
  int r;

  // cgroup v2 dirs/files are synthesized in-memory (syscalls.c); the host fstatat would ENOENT,
  // so answer the statx from the in-memory tree. Absolute paths only (systemd always uses them
  // for /sys/fs/cgroup); a relative/dirfd cgroup path falls through to the host (ENOENT).
  if (gpath && gpath[0] == '/') {
    int kind = cgroup_path_kind(gpath);
    if (kind) {
      memset(sx, 0, sizeof *sx);
      sx->stx_mask       = LX_STATX_BASIC_STATS | LX_STATX_BTIME | LX_STATX_MNT_ID | LX_STATX_MNT_ID_UNIQUE;
      sx->stx_blksize    = 4096;
      sx->stx_attributes = 0;
      sx->stx_nlink      = (kind == 1) ? 2 : 1;
      sx->stx_uid        = 0;
      sx->stx_gid        = 0;
      sx->stx_mode       = (kind == 1) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
      sx->stx_ino        = (kind == 1) ? 2 : 1;
      sx->stx_blocks     = 0;
      if (kind == 2) {
        char cbuf[2048];
        const char* c = cgroup_file_content(gpath, cbuf, sizeof cbuf);
        sx->stx_size = c ? (unsigned long long)strlen(c) : 0;
      }
      sx->stx_attributes_mask = LX_STATX_ATTR_MOUNT_ROOT;
      sx->stx_rdev_major = 0; sx->stx_rdev_minor = 0;
      sx->stx_dev_major  = 0; sx->stx_dev_minor  = 0;
      return 0;
    }
  }

  if ((lxflags & LX_AT_EMPTY_PATH) && (!gpath || !gpath[0])) {
    int fd = (lx_dirfd == LX_AT_FDCWD) ? AT_FDCWD : lx_dirfd;
    if (fd == AT_FDCWD) r = stat(".", &st);
    else r = net_opath_socket_stat(fd, &st) ? 0 : fstat(fd, &st);
  } else {
    char host[PATH_MAX]; int hdirfd;
    long e = resolve_at_nf(lx_dirfd, gpath, host, sizeof host, &hdirfd,
                           (lxflags & LX_AT_SYMLINK_NOFOLLOW) != 0);
    if (e < 0) return e;
    r = fstatat(hdirfd, host, &st, host_at_flags(lxflags));
  }
  if (r != 0) return eret();

  memset(sx, 0, sizeof *sx);
  // We always fill the full basic set plus btime, regardless of what was requested; stx_mask
  // reports what is actually valid (that is what the kernel does too).
  (void)mask;
  sx->stx_mask       = LX_STATX_BASIC_STATS | LX_STATX_BTIME | LX_STATX_MNT_ID | LX_STATX_MNT_ID_UNIQUE;
  sx->stx_mnt_id     = (unsigned long long)st.st_dev;   // one shim fs == one "mount"
  sx->stx_blksize    = (unsigned)st.st_blksize;
  sx->stx_attributes = 0;
  sx->stx_nlink      = (unsigned)st.st_nlink;
  sx->stx_uid        = (unsigned)(g_uid0 ? 0 : st.st_uid);
  sx->stx_gid        = (unsigned)(g_uid0 ? 0 : st.st_gid);
  if (g_uid0 && guest_ownership_get(&st, &sx->stx_uid, &sx->stx_gid)<0) return eret();
  sx->stx_mode       = (unsigned short)st.st_mode;   // S_IF* values match Linux exactly
  sx->stx_ino        = (unsigned long long)st.st_ino;
  sx->stx_size       = (unsigned long long)st.st_size;
  sx->stx_blocks     = (unsigned long long)st.st_blocks;
  // Report that we UNDERSTAND the mount-root attribute, and that this file is NOT a mount root
  // (the shim has no real mounts). systemd's fd_is_mount_point() reads this to decide "/proc is not
  // a mount point" and proceeds to mount it; without the bit it falls through to a mount-id
  // comparison that has no parent to compare against and fails (EUNATCH) before any mount happens.
  sx->stx_attributes_mask = LX_STATX_ATTR_MOUNT_ROOT;
  statx_ts(&sx->stx_atime, &st.st_atimespec);
  statx_ts(&sx->stx_btime, &st.st_birthtimespec);
  statx_ts(&sx->stx_ctime, &st.st_ctimespec);
  statx_ts(&sx->stx_mtime, &st.st_mtimespec);
  sx->stx_rdev_major = (unsigned)major(st.st_rdev);
  sx->stx_rdev_minor = (unsigned)minor(st.st_rdev);
  sx->stx_dev_major  = (unsigned)major(st.st_dev);
  sx->stx_dev_minor  = (unsigned)minor(st.st_dev);
  // The DRM render node must look like a character device here TOO, not just in newfstatat:
  // modern glibc implements stat() with statx, so libdrm's scan of /dev/dri goes through this
  // path and nothing else. Faking only newfstatat left `stat` reporting "regular empty file"
  // and libdrm skipping the node without ever opening it.
  if (gpath && gpath[0] == '/' && cam_is_node(gpath)) {
    unsigned mode = sx->stx_mode; unsigned long rdev = 0;
    cam_fake_stat(&mode, &rdev);
    sx->stx_mode = (unsigned short)mode;
    sx->stx_rdev_major = 81; sx->stx_rdev_minor = 0;
  }
  if ((gpath && gpath[0] == '/' && drm_is_node(gpath)) ||
      ((lxflags & LX_AT_EMPTY_PATH) && (!gpath || !gpath[0]) && drm_owns_fd(lx_dirfd))) {
    unsigned mode = sx->stx_mode; unsigned long rdev = 0;
    const char* node = (gpath && gpath[0] == '/') ? gpath :
                       drm_minor_of_fd(lx_dirfd) == 0 ? "/dev/dri/card0" : "/dev/dri/renderD128";
    drm_fake_stat(&mode, &rdev, node);
    sx->stx_mode       = (unsigned short)mode;
    sx->stx_rdev_major = (unsigned)((rdev >> 8) & 0xfff);
    sx->stx_rdev_minor = (unsigned)(rdev & 0xff);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// memfd_create — a real anonymous file: mkstemp + immediate unlink, so the fd supports
// read/write/ftruncate/mmap exactly like Linux's memfd (shm_open on macOS does NOT).
// ---------------------------------------------------------------------------
static const char* memfd_dir(void) {
  const char* d = getenv("RDX_MEMFD_DIR");
  if (d && *d) return d;
  static char rootdir[PATH_MAX];
  if (g_rootlen) {
    snprintf(rootdir, sizeof rootdir, "%s/tmp", g_root);
    struct stat st;
    if (stat(rootdir, &st) == 0 && S_ISDIR(st.st_mode)) return rootdir;
  }
  d = getenv("TMPDIR");
  if (d && *d) return d;
  return "/private/var/tmp";
}

static long do_memfd_create(const char* name, unsigned flags) {
  if (!name) return -LX_EFAULT;
  // MFD_NOEXEC_SEAL / MFD_EXEC (Linux 6.3) are ACCEPTED AND IGNORED, not rejected.
  // They only constrain whether the memfd may ever be mapped executable -- and this
  // memfd is a macOS mkstemp file, which does not enforce that either way, so there
  // is nothing here for them to select between. Ignoring them is the honest answer;
  // rejecting them reports "this flag is invalid", which is false.
  //
  // MEASURED: Firefox 140 calls memfd_create(name, MFD_CLOEXEC|MFD_ALLOW_SEALING|
  // MFD_NOEXEC_SEAL) = 0xb and got EINVAL. It happens to retry at 0x3 and survive,
  // so this is currently latent -- but a caller that does NOT retry sees a hard
  // failure from a flag combination that is valid on any kernel since 6.3.
  if (flags & ~(unsigned)(LX_MFD_CLOEXEC | LX_MFD_ALLOW_SEALING | LX_MFD_HUGETLB |
                          LX_MFD_NOEXEC_SEAL | LX_MFD_EXEC))
    return -LX_EINVAL;
  size_t n = strnlen(name, 250);
  if (n >= 250) return -LX_EINVAL;

  static unsigned long seq;
  char path[PATH_MAX];
  snprintf(path, sizeof path, "%s/.rdx-memfd-%d-%lu-XXXXXX",
           memfd_dir(), (int)getpid(), __atomic_fetch_add(&seq, 1, __ATOMIC_RELAXED));
  int fd = mkstemp(path);
  if (fd < 0) return eret();
  unlink(path);                                  // anonymous from here on
  if (flags & LX_MFD_CLOEXEC) {
    int fl = fcntl(fd, F_GETFD);
    if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
  }
  return fd;
}

// ---------------------------------------------------------------------------
// prctl state
// ---------------------------------------------------------------------------
static char g_procname[16] = "guest";
static int  g_dumpable = 1;
static int  g_securebits;
static int  g_thp_disable;
static unsigned g_mdwe;
static int  g_memory_merge;
static unsigned long g_timerslack = 50000;

static long do_prctl(int opt, unsigned long a1, unsigned long a2,
                     unsigned long a3, unsigned long a4) {
  (void)a2; (void)a3; (void)a4;
  switch (opt) {
    case LX_PR_SET_NAME: {
      if (!a1) return -LX_EFAULT;
      memset(g_procname, 0, sizeof g_procname);
      memcpy(g_procname, (const char*)a1, strnlen((const char*)a1, sizeof g_procname - 1));
      pthread_setname_np(g_procname);            // macOS can only name the calling thread
      return 0;
    }
    case LX_PR_GET_NAME:
      if (!a1) return -LX_EFAULT;
      memset((void*)a1, 0, 16);
      memcpy((void*)a1, g_procname, strnlen(g_procname, 15));
      return 0;
    case LX_PR_SET_PDEATHSIG:
      return syscalls_signal_set_pdeathsig((int)a1);
    case LX_PR_GET_PDEATHSIG:
      if (!a1) return -LX_EFAULT;
      *(int*)a1 = syscalls_signal_get_pdeathsig(); return 0;
    case LX_PR_SET_DUMPABLE:   g_dumpable = (int)a1; return 0;
    case LX_PR_GET_DUMPABLE:   return g_dumpable;
    case LX_PR_SET_KEEPCAPS:   return 0;
    case LX_PR_GET_KEEPCAPS:   return 0;
    case LX_PR_SET_SECUREBITS: g_securebits = (int)a1; return 0;
    case LX_PR_GET_SECUREBITS: return g_securebits;
    case LX_PR_SET_NO_NEW_PRIVS: return 0;
    case LX_PR_GET_NO_NEW_PRIVS: return 0;
    case LX_PR_CAPBSET_READ:   return 0;
    case LX_PR_CAPBSET_DROP:   return 0; // CapBnd is already empty; dropping remains idempotent.
    case LX_PR_SET_TIMERSLACK: g_timerslack = a1 ? a1 : g_timerslack; return 0;
    case LX_PR_GET_TIMERSLACK: return (long)g_timerslack;
    case LX_PR_SET_VMA:        return 0;         // anon-vma naming: purely cosmetic
    case LX_PR_SET_PTRACER:    return 0;
    case LX_PR_GET_SECCOMP:    return 0;
    case LX_PR_SET_SECCOMP:    return -LX_EINVAL;  // no filtering here; Linux also EINVALs
    // PR_CAP_AMBIENT (47): raise/lower/clear ambient capabilities. The shim does not enforce
    // capabilities (uid is already virtualized), so accept RAISE/LOWER/CLEAR_ALL. systemd's
    // exec_context_apply_ambient() calls PR_CAP_AMBIENT_RAISE for every unit and aborted the
    // service ("Failed to apply the starting ambient set: Invalid argument") when this EINVAL'd.
    case LX_PR_CAP_AMBIENT:    return 0;
    // Securebits / THP / MDWE / memory-merge are security/hint toggles the shim does not enforce
    // (uid is already virtualized, no MDWE hardening, THP/merge are host policy). Accept the SET
    // and round-trip the GET so systemd's exec_context_apply() does not abort every unit.
    // MEASURED: systemd-executor exits EXIT_SECUREBITS (213) when PR_SET_SECUREBITS EINVALs, which
    // is exactly why PID 1 spawned 11 executors and then stalled with no service ever started.
    case LX_PR_SET_THP_DISABLE:  g_thp_disable = (int)a1; return 0;
    case LX_PR_GET_THP_DISABLE:  return g_thp_disable;
    case LX_PR_SET_MDWE:         g_mdwe = (unsigned)a1; return 0;
    case LX_PR_GET_MDWE:         return (long)g_mdwe;
    case LX_PR_SET_MEMORY_MERGE: g_memory_merge = (int)a1; return 0;
    case LX_PR_GET_MEMORY_MERGE: return g_memory_merge;
    default:                   return -LX_EINVAL;
  }
}

// ---------------------------------------------------------------------------
// sigaltstack — SHADOW ONLY. We never deliver guest signals through host handlers, and the
// member order differs from macOS, so installing it on the host would be both wrong and
// pointless. Recording it keeps libc's init happy and makes the query path round-trip.
// ---------------------------------------------------------------------------
struct lx_stack { void* ss_sp; int ss_flags; /*pad*/ unsigned long ss_size; };
#define LX_SS_ONSTACK 1
#define LX_SS_DISABLE 2
#define LX_MINSIGSTKSZ 5120

static struct lx_stack g_altstack = { 0, LX_SS_DISABLE, 0 };

static long do_sigaltstack(const struct lx_stack* ss, struct lx_stack* oss) {
  struct lx_stack prev = g_altstack;
  if (ss) {
    if (ss->ss_flags & ~(LX_SS_DISABLE | LX_SS_ONSTACK)) return -LX_EINVAL;
    if (ss->ss_flags & LX_SS_DISABLE) {
      g_altstack.ss_sp = 0; g_altstack.ss_size = 0; g_altstack.ss_flags = LX_SS_DISABLE;
    } else {
      if (ss->ss_size < LX_MINSIGSTKSZ) return -LX_ENOMEM;
      g_altstack.ss_sp = ss->ss_sp; g_altstack.ss_size = ss->ss_size; g_altstack.ss_flags = 0;
    }
  }
  if (oss) *oss = prev;
  return 0;
}

// ---------------------------------------------------------------------------
// rlimit index + value translation
// ---------------------------------------------------------------------------
static int host_rlimit(int lx) {
  switch (lx) {
    case 0: return RLIMIT_CPU;
    case 1: return RLIMIT_FSIZE;
    case 2: return RLIMIT_DATA;
    case 3: return RLIMIT_STACK;
    case 4: return RLIMIT_CORE;
    case 5: return RLIMIT_RSS;      // macOS RLIMIT_RSS == RLIMIT_AS
    case 6: return RLIMIT_NPROC;
    case 7: return RLIMIT_NOFILE;
    case 8: return RLIMIT_MEMLOCK;
    case 9: return RLIMIT_AS;
    default: return -1;             // LOCKS/SIGPENDING/MSGQUEUE/NICE/RTPRIO/RTTIME
  }
}
struct lx_rlimit { unsigned long long rlim_cur, rlim_max; };
static unsigned long long lx_rlim(rlim_t v) {
  return (v == RLIM_INFINITY) ? LX_RLIM_INFINITY : (unsigned long long)v;
}
static rlim_t host_rlim(unsigned long long v) {
  return (v == LX_RLIM_INFINITY || v > (unsigned long long)RLIM_INFINITY)
         ? RLIM_INFINITY : (rlim_t)v;
}

// ---------------------------------------------------------------------------
// misc small workers
// ---------------------------------------------------------------------------
static long do_pipe2(int* ufds, int flags) {
  if (!ufds) return -LX_EFAULT;
  if (flags & ~(LX_O_NONBLOCK | LX_O_CLOEXEC)) return -LX_EINVAL;
  int fds[2];
  if (pipe(fds) != 0) return eret();
  for (int i = 0; i < 2; i++) {
    if (flags & LX_O_CLOEXEC)  fcntl(fds[i], F_SETFD, fcntl(fds[i], F_GETFD) | FD_CLOEXEC);
    if (flags & LX_O_NONBLOCK) fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
  }
  ufds[0] = fds[0]; ufds[1] = fds[1];
  return 0;
}

static long do_dup3(int oldfd, int newfd, int flags) {
  if (getenv("IDLE_PIPE_TRACE") && (oldfd == 11 || newfd == 11))
    fprintf(stderr, "[idle-socket] pid=%d op=dup3 old=%d new=%d flags=%d\n",
            getpid(), oldfd, newfd, flags);
  if (drm_owns_fd(oldfd) && newfd >= 0) drm_register_fd_copy(newfd, oldfd);
  if (snd_owns_fd(oldfd) && newfd >= 0) snd_dup_fd(oldfd, newfd);
  if (evdev_owns_fd(oldfd) && newfd >= 0) evdev_register_fd(newfd, evdev_device_of_fd(oldfd));
  if (oldfd == newfd) return -LX_EINVAL;         // this is the whole point of dup3 vs dup2
  if (flags & ~LX_O_CLOEXEC) return -LX_EINVAL;
  if (fcntl(oldfd, F_GETFD) < 0) return eret();  // validate before clobbering newfd
  if (dup2(oldfd, newfd) < 0) return eret();
  rd_rtnl_dup(oldfd, newfd);
  if (pidfd_shim_pid(oldfd) > 0) pidfd_shim_register(newfd, pidfd_shim_pid(oldfd));
  if (flags & LX_O_CLOEXEC) fcntl(newfd, F_SETFD, fcntl(newfd, F_GETFD) | FD_CLOEXEC);
  return newfd;
}

static long do_fallocate(int fd, int mode, long long off, long long len) {
  if (len <= 0 || off < 0) return -LX_EINVAL;
  if (mode & LX_FALLOC_PUNCH_HOLE) {
#ifdef F_PUNCHHOLE
    fpunchhole_t ph = {0};
    ph.fp_offset = off; ph.fp_length = len;
    if (fcntl(fd, F_PUNCHHOLE, &ph) == 0) return 0;
    return eret();
#else
    return -LX_EOPNOTSUPP;
#endif
  }
  if (mode & ~LX_FALLOC_KEEP_SIZE) return -LX_EOPNOTSUPP;
  struct stat st;
  if (fstat(fd, &st) != 0) return eret();
  long long target = off + len;
  if (target > st.st_size) {
    fstore_t fst = {0};
    fst.fst_flags   = F_ALLOCATECONTIG;
    fst.fst_posmode = F_PEOFPOSMODE;
    fst.fst_offset  = 0;
    fst.fst_length  = target - st.st_size;
    if (fcntl(fd, F_PREALLOCATE, &fst) < 0) {
      fst.fst_flags = F_ALLOCATEALL;
      if (fcntl(fd, F_PREALLOCATE, &fst) < 0) return eret();
    }
    if (!(mode & LX_FALLOC_KEEP_SIZE) && ftruncate(fd, target) != 0) return eret();
  }
  return 0;
}

// A bounce-buffer copy used by sendfile / splice / copy_file_range. `nonblock` mirrors Linux
// SPLICE_F_NONBLOCK: the source read must return -EAGAIN rather than block (journald splices its
// stream sockets this way, and a blocking read parked the journald main loop forever).
static long bounce_copy(int in_fd, long long* in_off, int out_fd, long long* out_off,
                        size_t count, int nonblock) {
  char buf[65536];
  size_t done = 0;
  int saved_fl = -1;
  if (nonblock) {
    saved_fl = fcntl(in_fd, F_GETFL);
    if (saved_fl >= 0 && !(saved_fl & O_NONBLOCK)) fcntl(in_fd, F_SETFL, saved_fl | O_NONBLOCK);
  }
  while (done < count) {
    size_t want = count - done; if (want > sizeof buf) want = sizeof buf;
    ssize_t got = in_off ? pread(in_fd, buf, want, (off_t)(*in_off + (long long)done))
                         : read(in_fd, buf, want);
    if (got < 0) {
      long e = eret();
      if (nonblock && (errno == EAGAIN || errno == EWOULDBLOCK) && done == 0) {
        if (saved_fl >= 0) fcntl(in_fd, F_SETFL, saved_fl);
        return -LX_EAGAIN;
      }
      if (saved_fl >= 0) fcntl(in_fd, F_SETFL, saved_fl);
      return done ? (long)done : e;
    }
    if (got == 0) break;
    ssize_t put = 0;
    while (put < got) {
      ssize_t w = out_off ? pwrite(out_fd, buf + put, (size_t)(got - put),
                                   (off_t)(*out_off + (long long)done + put))
                          : write(out_fd, buf + put, (size_t)(got - put));
      if (w <= 0) { if (saved_fl >= 0) fcntl(in_fd, F_SETFL, saved_fl);
                    return (done + (size_t)put) ? (long)(done + (size_t)put) : eret(); }
      put += w;
    }
    done += (size_t)got;
    if (got < (ssize_t)want) break;
  }
  if (saved_fl >= 0) fcntl(in_fd, F_SETFL, saved_fl);
  if (in_off)  *in_off  += (long long)done;
  if (out_off) *out_off += (long long)done;
  return (long)done;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
long linux_syscall_ext(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
                       unsigned long a3, unsigned long a4, unsigned long a5) {
  (void)a5;
  char host[PATH_MAX], host2[PATH_MAX];
  int hdirfd, hdirfd2;
  long e;

  switch (nr) {

  // ===== 0..16: aio + extended attributes ==================================
  case 0: case 1: case 2: case 3: case 4: return -LX_ENOSYS;   // io_setup..io_getevents (aio)
  case 5: case 6: case 7: {                                    // setxattr / lsetxattr / fsetxattr
    int hopt = 0;                                              // Linux XATTR_CREATE=1 REPLACE=2
    if ((int)a4 & 1) hopt |= XATTR_CREATE;
    if ((int)a4 & 2) hopt |= XATTR_REPLACE;
    if (nr == 6) hopt |= XATTR_NOFOLLOW;
    if (nr == 7) return ok0(fsetxattr((int)a0, (const char*)a1, (const void*)a2,
                                      (size_t)a3, 0, hopt));
    e = resolve_path_nf((const char*)a0, host, sizeof host, nr == 6);
    if (e < 0) return e;
    return ok0(setxattr(host, (const char*)a1, (const void*)a2, (size_t)a3, 0, hopt));
  }
  case 8: case 9: case 10: {                                   // getxattr / lgetxattr / fgetxattr
    ssize_t r;
    if (nr == 10) r = fgetxattr((int)a0, (const char*)a1, (void*)a2, (size_t)a3, 0, 0);
    else {
      e = resolve_path_nf((const char*)a0, host, sizeof host, nr == 9);
      if (e < 0) return e;
      r = getxattr(host, (const char*)a1, (void*)a2, (size_t)a3, 0,
                   nr == 9 ? XATTR_NOFOLLOW : 0);
    }
    return r < 0 ? eret() : r;
  }
  case 11: case 12: case 13: {                                 // listxattr / l / f
    ssize_t r;
    if (nr == 13) r = flistxattr((int)a0, (char*)a1, (size_t)a2, 0);
    else {
      e = resolve_path_nf((const char*)a0, host, sizeof host, nr == 12);
      if (e < 0) return e;
      r = listxattr(host, (char*)a1, (size_t)a2, nr == 12 ? XATTR_NOFOLLOW : 0);
    }
    return r < 0 ? eret() : r;
  }
  case 14: case 15: case 16: {                                 // removexattr / l / f
    if (nr == 16) return ok0(fremovexattr((int)a0, (const char*)a1, 0));
    e = resolve_path_nf((const char*)a0, host, sizeof host, nr == 15);
    if (e < 0) return e;
    return ok0(removexattr(host, (const char*)a1, nr == 15 ? XATTR_NOFOLLOW : 0));
  }

  // ===== 18..34: fds, inotify, nodes ========================================
  case 18: return -LX_ENOSYS;                                  // lookup_dcookie
  case 23: {                                                   // dup
    int r = dup((int)a0);
    // A dup of the DRM render node is still the render node. gbm dups the fd, and without this
    // the copy reported /dev/null's device numbers and Mesa went looking for /sys/dev/char/0:12290.
    if (r >= 0 && pidfd_shim_pid((int)a0) > 0) pidfd_shim_register(r, pidfd_shim_pid((int)a0));
    if (r >= 0) rd_rtnl_dup((int)a0, r);
    if (r >= 0 && drm_owns_fd((int)a0)) drm_register_fd_copy(r, (int)a0);
    if (r >= 0 && snd_owns_fd((int)a0)) snd_dup_fd((int)a0, r);
    if (r >= 0 && evdev_owns_fd((int)a0)) evdev_register_fd(r, evdev_device_of_fd((int)a0));
    return r < 0 ? eret() : r;
  }
  case 24: return do_dup3((int)a0, (int)a1, (int)a2);          // dup3
  // inotify: an INERT file-change backend. glib's GIO registers its file-monitor backend by
  // calling inotify_init1() and giving up if it fails — and when no monitor backend exists it
  // reports "Unable to find default local file monitor type" and then NULL-derefs. So the fd has
  // to be real and pollable (a pipe read end: normally quiet, except IN_IGNORED on removal).
  // No filesystem-change watch ever fires. Apps that merely want notification degrade to not noticing
  // changes; apps that merely want a monitor object work.
  case 26: {                                                    // inotify_init1(flags)
    if (a0 & ~(unsigned long)(LX_O_NONBLOCK_ | LX_O_CLOEXEC_)) return -LX_EINVAL;
    int fds[2];
    if (pipe(fds) != 0) return eret();
    // The write end is deliberately KEPT OPEN. Closing it would make the
    // read end instantly readable-at-EOF, and glib treats that as fatal: "inotify unexpectedly
    // hit eof". Holding it open makes the fd simply quiet forever, which is what we want. It must
    // ALSO be reserved: udevd's worker spawn (posix_spawn) does close_range(3,~0U) in the vfork'd
    // child, and an unreserved write end gets closed there, making udevd's inotify read end EOF ->
    // a spurious IN_* wake -> udevd reloads its rules dirs in a 100% CPU loop.
    fds[0] = shim_fd_reserve(fds[0]);   // read end: the vfork child's close_range() must skip it too
    fds[1] = shim_fd_reserve(fds[1]);
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
    if (inotify_watch_register(fds[0], fds[1]) != 0) {
      shim_fd_unreserve(fds[0]); shim_fd_unreserve(fds[1]);
      close(fds[0]); close(fds[1]);
      return -LX_ENOMEM;
    }
    if (a0 & LX_O_NONBLOCK_) fcntl(fds[0], F_SETFL, O_NONBLOCK);
    if (a0 & LX_O_CLOEXEC_)  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    return fds[0];
  }
  case 27: {                                                    // inotify_add_watch(fd, path, mask)
    e = resolve_path_nf((const char*)a1, host, sizeof host,
                        ((unsigned)a2 & 0x02000000u) != 0);     // IN_DONT_FOLLOW
    if (e < 0) return e;
    return inotify_watch_add((int)a0, host, (unsigned)a2);
  }
  case 28: return inotify_watch_remove((int)a0, (int)a1);       // inotify_rm_watch
  case 30: {                                                    // ioprio_set: reset-self subset
    // Linux stores per-task I/O priorities and its block schedulers apply
    // them. Darwin has no equivalent guest-visible scheduler here. Only the
    // already-unset self priority can be reset truthfully; do not acknowledge
    // a request for RT, BE, or IDLE while ignoring its scheduling effect.
    int which = (int)a0, who = (int)a1, prio = (int)a2;
    int cls = (prio >> 13) & 7;
    if (cls > 3 || (cls == 0 && (prio & 7))) return -LX_EINVAL;
    if (which < 1 || which > 3) return -LX_EINVAL;
    if (which != 1) return -LX_EOPNOTSUPP; // group/user fanout needs task state
    if (who < 0 || (who && who != syscalls_guest_pid())) return -LX_ESRCH;
    if (prio != 0) return -LX_EOPNOTSUPP;
    return 0;
  }
  case 31: {                                                    // ioprio_get: raw self priority
    // Linux returns the raw priority for IOPRIO_WHO_PROCESS. The only
    // priority accepted by our ioprio_set subset is the unset default (0),
    // so that value is truthful for the current guest task. Group/user
    // queries need a task registry and effective-priority aggregation.
    int which = (int)a0, who = (int)a1;
    if (which < 1 || which > 3) return -LX_EINVAL;
    if (which != 1) return -LX_EOPNOTSUPP;
    if (who < 0 || (who && who != syscalls_guest_pid())) return -LX_ESRCH;
    return 0;
  }
  case 33: {                                                   // mknodat
    mode_t mode = (mode_t)a2;
    // Linux's may_mknod() rejects invalid file types before pathname lookup.
    // A directory has its own EPERM result; symlinks and other unsupported
    // types return EINVAL.  Keep these failures distinct from valid device
    // types, which this shim cannot create on the host.
    switch (mode & S_IFMT) {
      case 0: case S_IFREG: case S_IFIFO: case S_IFCHR:
      case S_IFBLK: case S_IFSOCK: break;
      case S_IFDIR: return -LX_EPERM;
      default: return -LX_EINVAL;
    }
    e = resolve_at_nf((int)a0, (const char*)a1, host, sizeof host, &hdirfd, 1);
    if (e < 0) return e;
    if (S_ISFIFO(mode)) {
      // mkfifoat exists on macOS; fall back to a cwd-relative mkfifo if the dirfd is AT_FDCWD.
      if (mkfifoat(hdirfd, host, mode & 07777) != 0) return eret();
      return 0;
    }
    if ((mode & S_IFMT) == 0 || S_ISREG(mode)) {
      int fd = openat(hdirfd, host, O_CREAT | O_EXCL | O_WRONLY, mode & 07777);
      if (fd < 0) return eret();
      close(fd); return 0;
    }
    return -LX_EPERM;                                          // device nodes: never on macOS
  }

  // ===== 36..59: filesystem =================================================
  case 36: {                                                   // symlinkat(target, dfd, linkpath)
    // The target is stored verbatim (it is guest-namespace text, not a host path).
    if (!a0) return -LX_EFAULT;
    e = resolve_at_nf((int)a1, (const char*)a2, host, sizeof host, &hdirfd, 1);
    if (e < 0) return e;
    return ok0(symlinkat((const char*)a0, hdirfd, host));
  }
  case 39: return 0;                                            // umount2: nothing is ever really mounted
  // mount(): early API filesystems are synthesized in-process, so systemd's
  // mount calls for those existing trees can succeed without a host mount.
  // The per-user tmpfs is different: claiming success for /run/user/<uid>
  // makes systemd assume its uid=/gid= mount options took effect, skipping
  // the container fallback that chmods and chowns the directory. Return
  // EPERM only for that exact target. Earlier API tmpfs mounts still need
  // their existing synthetic success to let PID1 boot.
  case 40: {
    // mount(source, target, fstype, mountflags, data)
    (void)a0; (void)a3; (void)a4;
    const char *target = (const char*)a1;
    const char *fstype = (const char*)a2;
    if (!target) return -LX_EFAULT;
    if (fstype && strcmp(fstype, "tmpfs") == 0) {
      // systemd's mount_nofollow() passes /proc/self/fd/N, not the pathname.
      // Match that fd against the actual guest runtime directory inode; PID1
      // also mounts tmpfs through fd paths, so matching the fd syntax alone
      // would break early boot.
      const char *fdtext = NULL;
      if (strncmp(target, "/proc/self/fd/", 14) == 0) fdtext = target + 14;
      if (fdtext && *fdtext && strspn(fdtext, "0123456789") == strlen(fdtext)) {
        char *end = NULL;
        long n = strtol(fdtext, &end, 10);
        if (n >= 0 && n <= INT_MAX && end && !*end) {
          char fdpath[PATH_MAX];
          if (fcntl((int)n, F_GETPATH, fdpath) == 0) {
            const char *runtime = strstr(fdpath, "/run/user/");
            if (runtime) {
              const char *uid = runtime + 10;
              if (*uid && strspn(uid, "0123456789") == strlen(uid)) {
                char expected[PATH_MAX];
                struct stat got, want;
                if (resolve_path(runtime, expected, sizeof expected) >= 0 &&
                    fstat((int)n, &got) == 0 && stat(expected, &want) == 0 &&
                    got.st_dev == want.st_dev && got.st_ino == want.st_ino)
                  return -LX_EPERM;
              }
            }
          }
        }
      }
      if (strncmp(target, "/run/user/", 10) == 0) {
        const char *uid = target + 10;
        if (*uid && strspn(uid, "0123456789") == strlen(uid)) return -LX_EPERM;
      }
    }
    return 0;                                   // no-op: the synthetic tree already serves the mount
  }
  case 41: return -LX_EPERM;                                   // pivot_root
  case 42: return -LX_ENOSYS;                                  // nfsservctl
  case 45:                                                     // truncate
    e = resolve_path((const char*)a0, host, sizeof host);
    if (e < 0) return e;
    return ok0(truncate(host, (off_t)a1));
  case 47: return do_fallocate((int)a0, (int)a1, (long long)a2, (long long)a3);
  case 50: return ok0(fchdir((int)a0));                        // fchdir
  case 51: return -LX_EPERM;                                   // chroot
  case 52: return ok0(fchmod((int)a0, (mode_t)a1));            // fchmod
  case 53:                                                     // fchmodat(dfd, path, mode)
    // The original Linux syscall has three arguments. x3 is unspecified here;
    // treating it as flags makes chmod fail at random (ldconfig hits EINVAL).
    e = resolve_at((int)a0, (const char*)a1, host, sizeof host, &hdirfd);
    if (e < 0) return e;
    return ok0(fchmodat(hdirfd, host, (mode_t)a2, 0));
  case 54:                                                     // fchownat
    if (g_uid0) {
      struct stat st;
      if ((int)a4 & ~(LX_AT_EMPTY_PATH | LX_AT_SYMLINK_NOFOLLOW)) return -LX_EINVAL;
      if ((a4 & LX_AT_EMPTY_PATH) && (!a1 || !((const char*)a1)[0])) {
        if ((int)a0 == LX_AT_FDCWD) { if (stat(".", &st)<0) return eret(); }
        else if (fstat((int)a0, &st)<0) return eret();
      } else {
        e=resolve_at_nf((int)a0,(const char*)a1,host,sizeof host,&hdirfd,
                       ((int)a4 & LX_AT_SYMLINK_NOFOLLOW)!=0);
        if(e<0)return e;
        if(fstatat(hdirfd,host,&st,host_at_flags((int)a4))<0)return eret();
      }
      return ok0(guest_ownership_set(&st,(uint32_t)a2,(uint32_t)a3));
    }
    if ((a4 & LX_AT_EMPTY_PATH) && (!a1 || !((const char*)a1)[0]))
      return ok0(fchown((int)a0, (uid_t)a2, (gid_t)a3));       // "chown the dirfd itself"
    e = resolve_at_nf((int)a0, (const char*)a1, host, sizeof host, &hdirfd,
                      ((int)a4 & LX_AT_SYMLINK_NOFOLLOW) != 0);
    if (e < 0) return e;
    return ok0(fchownat(hdirfd, host, (uid_t)a2, (gid_t)a3, host_at_flags((int)a4)));
  case 55: // fchown
    if (g_uid0) {
      struct stat st; if(fstat((int)a0,&st)<0)return eret();
      return ok0(guest_ownership_set(&st,(uint32_t)a1,(uint32_t)a2));
    }
    return ok0(fchown((int)a0,(uid_t)a1,(gid_t)a2));
  case 58: return -LX_EPERM;                                   // vhangup
  case 59: return do_pipe2((int*)a0, (int)a1);                 // pipe2 (net.c normally owns this)
  case 60: return -LX_EPERM;                                   // quotactl

  // ===== 69..77: vectored / zero-copy IO ====================================
  case 69: {                                                   // preadv
    ssize_t r = preadv((int)a0, (const struct iovec*)a1, (int)a2, (off_t)a3);
    return r < 0 ? eret() : r;
  }
  case 70: {                                                   // pwritev
    ssize_t r = pwritev((int)a0, (const struct iovec*)a1, (int)a2, (off_t)a3);
    return r < 0 ? eret() : r;
  }
  case 71: {                                                   // sendfile(out, in, off*, count)
    long long off, *offp = 0;
    if (a2) { off = *(long long*)a2; offp = &off; }
    long r = bounce_copy((int)a1, offp, (int)a0, 0, (size_t)a3, 0);
    if (r >= 0 && a2) *(long long*)a2 = off;
    return r;
  }
  case 74: return signalfd4_shim((int)a0, (const uint64_t*)a1, (size_t)a2, (int)a3);
  case 75: {                                                   // vmsplice(pipe, iov, nr_segs, flags)
    // vmsplice's observable operation is to append iovec bytes to a pipe.
    // Darwin writev copies the bytes rather than gifting pages to the pipe;
    // SPLICE_F_MOVE, MORE and GIFT are hints and do not change their contents.
    // A blocking pipe plus SPLICE_F_NONBLOCK cannot be implemented without
    // changing the shared open-file description's flags, so refuse that case
    // rather than unexpectedly blocking another guest thread.
    const unsigned flags = (unsigned)a3;
    if (flags & ~0xfu) return -LX_EINVAL;
    if (a2 > 1024) return -LX_EINVAL;                         // Linux UIO_MAXIOV
    if (a2 && !a1) return -LX_EFAULT;
    struct stat st;
    if (fstat((int)a0, &st) != 0) return eret();
    if (!S_ISFIFO(st.st_mode)) return -LX_EINVAL;
    if (flags & 0x2u) {                                       // SPLICE_F_NONBLOCK
      int fl = fcntl((int)a0, F_GETFL);
      if (fl < 0) return eret();
      if (!(fl & O_NONBLOCK)) return -LX_EOPNOTSUPP;
    }
    if (!a2) return 0;
    ssize_t r = writev((int)a0, (const struct iovec*)a1, (int)a2);
    return r < 0 ? eret() : r;
  }
  case 76: {                                                   // splice
    long long ioff, ooff, *ip = 0, *op = 0;
    if (a1) { ioff = *(long long*)a1; ip = &ioff; }
    if (a3) { ooff = *(long long*)a3; op = &ooff; }
    long r = bounce_copy((int)a0, ip, (int)a2, op, (size_t)a4, (int)(a5 & 0x2));
    if (r >= 0 && a1) *(long long*)a1 = ioff;
    if (r >= 0 && a3) *(long long*)a3 = ooff;
    return r;
  }
  case 77: return -LX_ENOSYS;                                  // tee

  // ===== 81..92: sync, timers, utimes, caps =================================
  case 81: sync(); return 0;                                   // sync
  case 82: return ok0(fsync((int)a0));                         // fsync
  case 83:                                                     // fdatasync
    // macOS fsync does not push the drive cache; F_FULLFSYNC does. fdatasync only owes us
    // metadata-lite durability, so plain fsync is the right (and much cheaper) mapping.
    return ok0(fsync((int)a0));
  case 84: return ok0(fsync((int)a0));                         // sync_file_range -> fsync
  case 85: return timerfd_create_shim((int)a0, (int)a1);
  case 86: return timerfd_settime_shim((int)a0, (int)a1, (const linux_itimerspec*)a2, (linux_itimerspec*)a3);
  case 87: return timerfd_gettime_shim((int)a0, (linux_itimerspec*)a1);
  case 88: {                                                   // utimensat(dfd, path, times, flags)
    const struct timespec* ts = (const struct timespec*)a2;    // layout matches macOS on arm64
    if (!a1) return ok0(futimens((int)a0, ts));                // NULL path == "the dirfd itself"
    e = resolve_at_nf((int)a0, (const char*)a1, host, sizeof host, &hdirfd,
                      ((int)a3 & LX_AT_SYMLINK_NOFOLLOW) != 0);
    if (e < 0) return e;
    return ok0(utimensat(hdirfd, host, ts, host_at_flags((int)a3)));
  }
  case 89: return -LX_EPERM;                                   // acct
  case 90: {                                                   // capget: report "no capabilities"
    // Linux uses a NULL data pointer to negotiate the capability ABI version.
    // libcap-ng initializes with version 0 and expects EINVAL plus the current
    // version written back; a bare success leaves its state uninitialized.
    if (!a0) return -LX_EFAULT;
    uint32_t* header = (uint32_t*)a0;
    const uint32_t current = 0x20080522U; // _LINUX_CAPABILITY_VERSION_3
    uint32_t nwords = (header[0] == 0x19980330U) ? 1 : 2;
    if (header[0] != current && header[0] != 0x20071026U &&
        header[0] != 0x19980330U) {
      header[0] = current;
      return -LX_EINVAL;
    }
    if (!a1) return 0;
    memset((void*)a1, 0, nwords * 12); // __user_cap_data_struct entries
    return 0;
  }
  case 91: return g_uid0 ? 0 : -LX_EPERM;                       // capset: no-op under RDUID0
  case 92: return 0;                                           // personality: accept, ignore

  // ===== 95..106: process =================================================
  case 95: {                                                   // waitid
    int lopt = (int)a3, hopt = 0;
    if (lopt & LX_WNOHANG)    hopt |= WNOHANG;
    if (lopt & LX_WSTOPPED)   hopt |= WSTOPPED;
    if (lopt & LX_WEXITED)    hopt |= WEXITED;
    if (lopt & LX_WCONTINUED) hopt |= WCONTINUED;
    if (lopt & LX_WNOWAIT)    hopt |= WNOWAIT;
    siginfo_t si; memset(&si, 0, sizeof si);
    // waitid(P_PIDFD, pidfd, ...): macOS has no pidfd idtype; resolve the pid from the registry.
    if ((int)a0 == LX_P_PIDFD) {
      if (pidfd_shim_waitid((int)a1, (id_t)a1, &si, hopt) != 0) return eret();
    } else if (waitid((idtype_t)a0, (id_t)a1, &si, hopt) != 0) return eret();
    if (si.si_pid > 0 && !(lopt & LX_WNOWAIT)) syscalls_signal_child_reaped(si.si_pid);
    if (a2) {                                                  // Linux siginfo layout, by hand
      unsigned char* u = (unsigned char*)a2;
      memset(u, 0, 128);
      *(int*)(u + 0)  = 17;                                    // si_signo = Linux SIGCHLD
      *(int*)(u + 4)  = 0;                                     // si_errno
      *(int*)(u + 8)  = si.si_code;                            // CLD_* match
      *(int*)(u + 16) = (int)si.si_pid;
      *(unsigned*)(u + 20) = (unsigned)si.si_uid;
      *(int*)(u + 24) = si.si_status;
    }
    return 0;
  }
  case 97: return -LX_EPERM;                                   // unshare
  case 100: return guest_get_robust_list((long)a0, a1, a2);    // get_robust_list
  case 101:                                                    // nanosleep (timespec matches)
    return ok0(nanosleep((const struct timespec*)a0, (struct timespec*)a1));
  case 102: {                                                  // getitimer
    struct itimerval hv;
    if (getitimer((int)a0, &hv) != 0) return eret();
    if (a1) {
      struct lx_itimerval* o = (struct lx_itimerval*)a1;
      lx_tv_put(&o->it_interval, &hv.it_interval);
      lx_tv_put(&o->it_value, &hv.it_value);
    }
    return 0;
  }
  case 103: {                                                  // setitimer
    struct itimerval hv, old;
    memset(&hv, 0, sizeof hv);
    if (a1) {
      const struct lx_itimerval* n = (const struct lx_itimerval*)a1;
      lx_tv_get(&hv.it_interval, &n->it_interval);
      lx_tv_get(&hv.it_value, &n->it_value);
    }
    if (setitimer((int)a0, a1 ? &hv : 0, &old) != 0) return eret();
    if (a2) {
      struct lx_itimerval* o = (struct lx_itimerval*)a2;
      lx_tv_put(&o->it_interval, &old.it_interval);
      lx_tv_put(&o->it_value, &old.it_value);
    }
    return 0;
  }
  case 104: case 105: case 106: return -LX_EPERM;              // kexec_load, init/delete_module
  case 107: return posix_timer_shim_create(a0, a1, a2);         // staged SIGEV_NONE subset
  case 108: return posix_timer_shim_gettime(a0, a1);            // staged SIGEV_NONE subset
  case 109: return posix_timer_shim_getoverrun(a0);             // staged SIGEV_NONE subset
  case 110: return posix_timer_shim_settime(a0, a1, a2, a3);    // staged SIGEV_NONE subset
  case 111: return posix_timer_shim_delete(a0);                 // staged timer_delete

  // ===== 116..128: logging + scheduling ====================================
  case 112: return -LX_EPERM;                                  // clock_settime
  case 116: return 0;                                          // syslog: stub success
  case 117: return -LX_EPERM;                                  // ptrace
  case 118: case 119: return 0;                                // sched_setparam/setscheduler
  case 120: return 0;                                          // sched_getscheduler -> SCHED_OTHER
  case 121:                                                    // sched_getparam
    if (a1) *(int*)a1 = 0;
    return 0;
  case 124: sched_yield(); return 0;                           // sched_yield
  case 125: case 126: {                                        // sched_get_priority_max/min
    int hp;                                                    // Linux policy -> macOS policy
    switch ((int)a0) {
      case 0: hp = SCHED_OTHER; break;                         // SCHED_NORMAL
      case 1: hp = SCHED_FIFO;  break;
      case 2: hp = SCHED_RR;    break;
      default: return -LX_EINVAL;
    }
    int r = (nr == 125) ? sched_get_priority_max(hp) : sched_get_priority_min(hp);
    return r < 0 ? eret() : r;
  }
  case 127:                                                    // sched_rr_get_interval
    if (a1) { struct lx_timespec* t = (struct lx_timespec*)a1; t->tv_sec = 0; t->tv_nsec = 1000000; }
    return 0;
  case 128: return -LX_EINTR;                                  // restart_syscall

  // ===== 130..140: signals =================================================
  case 130: case 131: {                                        // tkill(tid,sig) / tgkill(tgid,tid,sig)
    fx_note("SIGNAL tkill nr=%d a0=%ld a1=%ld a2=%ld (guest sig=%d)\n",
            (int)nr, (long)a0, (long)a1, (long)a2, (nr == 130) ? (int)a1 : (int)a2);
    int lsig = (nr == 130) ? (int)a1 : (int)a2;
    long tgt  = (nr == 130) ? (long)(int)a0 : (long)(int)a0;
    if (lsig < 0 || lsig > 64) return -LX_EINVAL;
    if (lsig == 0) return 0;                                   // existence probe
    int hs = host_signo(lsig);
    if (hs < 0) return 0;                                      // RT/STKFLT/PWR: no host analogue
    // GUEST TIDS ARE NOT HOST PIDS. They are different namespaces that overlap at small integers:
    // guest tids start at 1, and host pid 1 is launchd. The old code passed the guest's tid
    // straight to kill(), so a guest calling abort() -- which musl implements as
    // tkill(gettid(), SIGABRT) -- sent SIGABRT to LAUNCHD. The host denied it, so nothing visibly
    // exploded, but the guest's abort() then did nothing at all: the process carried on and
    // exited 0, which is precisely the "exits cleanly, no window, no diagnostic" signature that
    // took a four-agent swarm to unpick. (Found by K2.)
    //
    // Everything we can signal is inside THIS process, so a known guest tid targets us. An
    // unknown one is ESRCH -- never a host pid.
    if (tgt <= 0 || tgt == (long)getpid() || guest_tid_known((int)tgt))
      return ok0(kill(getpid(), hs));
    return -LX_ESRCH;
  }
  case 132: return do_sigaltstack((const struct lx_stack*)a0, (struct lx_stack*)a1);
  case 133: {                                                  // rt_sigsuspend
    return syscalls_signal_sigsuspend((const unsigned long long*)a0, (size_t)a1);
  }
  case 136:                                                    // rt_sigpending
    if (!a0) return -LX_EFAULT;
    if (a1 != 8) return -LX_EINVAL;
    *(uint64_t*)a0 = (uint64_t)syscalls_signal_pending();
    return 0;
  case 137: return -LX_EAGAIN;                                 // rt_sigtimedwait: timed out
  case 138: case 240: {                                        // rt_sigqueueinfo/rt_tgsigqueueinfo
    int lsig = (nr == 138) ? (int)a1 : (int)a2;
    if (lsig == 0) return 0;
    int hs = host_signo(lsig);
    if (hs < 0) return 0;
    // Same namespace trap as tkill above: never hand a guest pid/tid to the host kill().
    pid_t p = (pid_t)(int)a0;
    if (p <= 0 || p == getpid() || guest_tid_known((int)p)) return ok0(kill(getpid(), hs));
    return -LX_ESRCH;
  }
  case 140: {                                                  // setpriority
    return ok0(setpriority((int)a0, (id_t)a1, (int)a2));
  }
  case 141: {                                                  // getpriority
    errno = 0;
    int r = getpriority((int)a0, (id_t)a1);
    if (r == -1 && errno) return eret();
    return 20 - r;                                             // raw Linux ABI returns 20-nice
  }
  case 142: return -LX_EPERM;                                  // reboot

  // ===== 143..162: credentials, names ======================================
  case 143: return g_uid0 ? guest_cred_set_gid((int)a0, (int)a1, -1) : ok0(setregid((gid_t)a0, (gid_t)a1));
  case 144: return g_uid0 ? guest_cred_set_gid((int)a0, (int)a0, (int)a0) : ok0(setgid((gid_t)a0));
  case 145: return g_uid0 ? guest_cred_set_uid((int)a0, (int)a1, -1) : ok0(setreuid((uid_t)a0, (uid_t)a1));
  case 146: return g_uid0 ? guest_cred_set_uid((int)a0, (int)a0, (int)a0) : ok0(setuid((uid_t)a0));
  case 147: {                                                  // setresuid (no macOS equivalent)
    if (g_uid0) return guest_cred_set_uid((int)a0, (int)a1, (int)a2);
    uid_t r = (uid_t)a0, ef = (uid_t)a1;
    if ((int)a0 == -1 && (int)a1 == -1) return 0;
    if (setreuid((int)a0 == -1 ? (uid_t)-1 : r, (int)a1 == -1 ? (uid_t)-1 : ef) != 0) return eret();
    return 0;
  }
  case 148:                                                    // getresuid
    // RDUID0 has to be honoured HERE TOO, or a program that asks getresuid() instead of getuid()
    // sees the real uid and the two answers disagree -- glibc's `id` reads both. See syscalls.c:174.
    if (a0) *(unsigned*)a0 = g_uid0 ? guest_cred_uid(0) : getuid();
    if (a1) *(unsigned*)a1 = g_uid0 ? guest_cred_uid(1) : geteuid();
    if (a2) *(unsigned*)a2 = g_uid0 ? guest_cred_uid(2) : geteuid();
    return 0;
  case 149: {                                                  // setresgid
    if (g_uid0) return guest_cred_set_gid((int)a0, (int)a1, (int)a2);
    if ((int)a0 == -1 && (int)a1 == -1) return 0;
    if (setregid((int)a0 == -1 ? (gid_t)-1 : (gid_t)a0,
                 (int)a1 == -1 ? (gid_t)-1 : (gid_t)a1) != 0) return eret();
    return 0;
  }
  case 150:                                                    // getresgid
    if (a0) *(unsigned*)a0 = g_uid0 ? guest_cred_gid(0) : getgid();
    if (a1) *(unsigned*)a1 = g_uid0 ? guest_cred_gid(1) : getegid();
    if (a2) *(unsigned*)a2 = g_uid0 ? guest_cred_gid(2) : getegid();
    return 0;
  case 151: return g_uid0 ? (long)guest_cred_uid(1) : (long)geteuid(); // setfsuid -> previous fsuid
  case 152: return g_uid0 ? (long)guest_cred_gid(1) : (long)getegid(); // setfsgid -> previous fsgid
  case 153: {                                                  // times (core normally owns this)
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return eret();
    if (a0) {
      struct lx_tms* t = (struct lx_tms*)a0;
      t->tms_utime = ru.ru_utime.tv_sec * LX_USER_HZ + ru.ru_utime.tv_usec / (1000000/LX_USER_HZ);
      t->tms_stime = ru.ru_stime.tv_sec * LX_USER_HZ + ru.ru_stime.tv_usec / (1000000/LX_USER_HZ);
      struct rusage rc;
      if (getrusage(RUSAGE_CHILDREN, &rc) == 0) {
        t->tms_cutime = rc.ru_utime.tv_sec * LX_USER_HZ + rc.ru_utime.tv_usec / (1000000/LX_USER_HZ);
        t->tms_cstime = rc.ru_stime.tv_sec * LX_USER_HZ + rc.ru_stime.tv_usec / (1000000/LX_USER_HZ);
      } else { t->tms_cutime = 0; t->tms_cstime = 0; }
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec * LX_USER_HZ + now.tv_nsec / (1000000000/LX_USER_HZ));
  }
  case 154: return ok0(setpgid((pid_t)a0, (pid_t)a1));         // setpgid
  case 155: {                                                  // getpgid
    pid_t r = getpgid((pid_t)a0); return r < 0 ? eret() : r;
  }
  case 156: {                                                  // getsid
    pid_t r = getsid((pid_t)a0); return r < 0 ? eret() : r;
  }
  case 157: {                                                  // setsid
    pid_t r = setsid(); return r < 0 ? eret() : r;
  }
  case 158: {                                                  // getgroups (gid_t is u32 on both)
    // RDUID0=1: the host process is uid 501 and its group list is the HOST's; the guest is uid 0 and
    // expects the root group. Report a synthetic single supplementary group (gid 0) so systemd's
    // executor can "determine supplementary groups" for SupplementaryGroups=/Group= units.
    if (g_uid0) {
      int sz = (int)a0;
      if (sz < 0) return -LX_EINVAL;
      if (sz == 0) return 1;                 // query count
      if (a1) ((gid_t*)a1)[0] = (gid_t)guest_cred_gid(1);
      return 1;
    }
    int n = getgroups((int)a0, (gid_t*)a1);
    return n < 0 ? eret() : n;
  }
  case 159: return g_uid0 ? 0 : ok0(setgroups((int)a0, (const gid_t*)a1));  // setgroups
  case 161: case 162: return -LX_EPERM;                        // sethostname/setdomainname
  case 163: {                                                  // getrlimit
    int hr = host_rlimit((int)a0);
    struct lx_rlimit* o = (struct lx_rlimit*)a1;
    if (!o) return -LX_EFAULT;
    if (hr < 0) { o->rlim_cur = o->rlim_max = LX_RLIM_INFINITY; return 0; }
    struct rlimit rl;
    if (getrlimit(hr, &rl) != 0) return eret();
    o->rlim_cur = lx_rlim(rl.rlim_cur); o->rlim_max = lx_rlim(rl.rlim_max);
    return 0;
  }
  case 164: {                                                  // setrlimit
    int hr = host_rlimit((int)a0);
    const struct lx_rlimit* n = (const struct lx_rlimit*)a1;
    if (!n) return -LX_EFAULT;
    if (hr < 0) return 0;                                      // limits macOS has no notion of
    struct rlimit rl;
    rl.rlim_cur = host_rlim(n->rlim_cur); rl.rlim_max = host_rlim(n->rlim_max);
    return ok0(setrlimit(hr, &rl));
  }
  case 166: return (long)umask((mode_t)a0);                    // umask
  case 167: return do_prctl((int)a0, a1, a2, a3, a4);          // prctl
  case 168:                                                    // getcpu
    if (a0) *(unsigned*)a0 = 0;
    if (a1) *(unsigned*)a1 = 0;
    return 0;
  case 169: {                                                  // gettimeofday (Linux timeval!)
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) return eret();
    if (a0) lx_tv_put((struct lx_timeval*)a0, &tv);
    if (a1) { int* tz = (int*)a1; tz[0] = 0; tz[1] = 0; }
    return 0;
  }
  case 170: return -LX_EPERM;                                  // settimeofday
  case 171:                                                    // adjtimex: report TIME_OK
    if (a0) memset((void*)a0, 0, 208);
    return 0;

  // ===== 180..197: SysV IPC / POSIX mqueues ================================
  // POSIX mqueues (mq_open/...) have NO macOS equivalent (Darwin implements SysV IPC and POSIX
  // shm, but not POSIX mq). They stay a clean -ENOSYS on purpose: the honest milestone here is
  // the SysV message-queue forwarding below plus /dev/shm for POSIX shm_open. A program that
  // hard-requires mq gets an unambiguous ENOSYS to react to, not a half-correct emulation.
  case 180: case 181: case 182: case 183: case 184: case 185:  // mq_*
    return -LX_ENOSYS;

  // SysV message queues are FORWARDED to the host, exactly like shm/sem below. macOS implements
  // msgget/msgsnd/msgrcv/msgctl natively, the four signatures match Linux 1:1, struct msgbuf's
  // leading `long mtype` is identical on both, and guest pointers ARE host pointers. IPC_NOWAIT
  // and MSG_NOERROR share their values, and msgctl's IPC_RMID/IPC_SET/IPC_STAT are 0/1/2 on both.
  case 186: { int r = msgget((key_t)a0, (int)a1); return r < 0 ? eret() : r; }   // msgget
  case 189: { int r = msgsnd((int)a0, (const void*)a1, (size_t)a2, (int)a3);     // msgsnd
              return r < 0 ? eret() : 0; }
  case 188: { ssize_t r = msgrcv((int)a0, (void*)a1, (size_t)a2, (long)a3, (int)a4);  // msgrcv
              return r < 0 ? eret() : (long)r; }
  case 187: {                                                  // msgctl(msqid, cmd, buf)
    // IPC_STAT/IPC_SET must translate the struct, not pass it through: Linux msqid64_ds and
    // Darwin msqid_ds disagree on field order (Darwin puts cbytes/qnum/qbytes/lspid/lrpid BEFORE
    // the timestamps and pads differently), so a raw forward reports qbytes/qnum out of the
    // middle of the reserved words. See lx_msqid_put / lx_msqid_get.
    if ((int)a1 == IPC_RMID) { int r = msgctl((int)a0, IPC_RMID, 0); return r < 0 ? eret() : 0; }
    if ((int)a1 == IPC_SET) {
      if (!a2) return -LX_EFAULT;
      struct msqid_ds hs;
      lx_msqid_get(&hs, (const struct lx_msqid64_ds*)a2);
      int r = msgctl((int)a0, IPC_SET, &hs);
      return r < 0 ? eret() : 0;
    }
    if ((int)a1 == IPC_STAT) {
      if (!a2) return -LX_EFAULT;
      struct msqid_ds hs;
      if (msgctl((int)a0, IPC_STAT, &hs) < 0) return eret();
      lx_msqid_put((struct lx_msqid64_ds*)a2, &hs);
      return 0;
    }
    return -LX_ENOSYS;                          // IPC_INFO/MSG_INFO/MSG_STAT: nothing asks
  }

  // SysV shared memory and semaphores are FORWARDED to the host. macOS implements both natively
  // (verified: semget -> 131083, shmget -> 131073, shmat -> 0x1008b4000), so stubbing these to
  // -ENOSYS was leaving a whole class of app dead for no reason. Qt's QSharedMemory and
  // QSystemSemaphore sit on them, which means SingleApplication does too -- kate issued exactly
  // semget(190) then shmget(194) and quit rc=0 before opening a window, with no diagnostic
  // (found by R5).
  //
  // Forwarding is safe here in a way it would not be under a VM: guest pointers ARE host
  // pointers, so an address returned by the host shmat() is directly usable by the guest.
  case 194:                                                    // shmget(key, size, flag)
    { int r = shmget((key_t)a0, (size_t)a1, (int)a2); return r < 0 ? eret() : r; }
  case 196: {                                                  // shmat(shmid, addr, flag)
    void* r = shmat((int)a0, (const void*)a1, (int)a2);
    return (r == (void*)-1) ? eret() : (long)(uintptr_t)r;
  }
  case 197:                                                    // shmdt(addr)
    { int r = shmdt((const void*)a0); return r < 0 ? eret() : 0; }
  case 195: {                                                  // shmctl(shmid, cmd, buf)
    // IPC_RMID/IPC_SET/IPC_STAT share values 0/1/2 on both systems.
    //
    // IPC_STAT used to be refused here, on the reasoning that struct shmid_ds differs between
    // Linux and macOS so garbage would be worse than an error. The premise was right and the
    // conclusion was wrong: it is the ONE command Qt cannot do without, so refusing it left
    // every QSharedMemory user dead anyway -- just with a better message.
    //
    //   QSharedMemoryPrivate::attach()  shmget -> shmat -> shmctl(IPC_STAT) to learn shm_segsz;
    //                                   a failure here IS the failure, the mapping is discarded.
    //   QSharedMemoryPrivate::detach()  shmctl(IPC_STAT) to read shm_nattch, and IPC_RMID only
    //                                   when it hits 0. Report nattch wrong and every run leaks
    //                                   a segment until the host's shmmni is exhausted.
    //
    // kate's SingleApplication guard is exactly this path: "SingleApplication: Unable to create
    // block. QSharedMemory::UnknownError QSharedMemory::attach (shmctl): Function not
    // implemented", after which kate exits without a window. 16990e1 forwarded shmget/shmat/
    // shmdt and stopped one command short of making them useful (found by K3).
    //
    // So: translate. Field by field by name into the Linux shape -- see lx_shmid_put.
    if ((int)a1 == 0 /*IPC_RMID*/) { int r = shmctl((int)a0, IPC_RMID, 0); return r < 0 ? eret() : 0; }
    if ((int)a1 == 2 /*IPC_STAT*/) {
      if (!a2) return -LX_EFAULT;
      struct shmid_ds hs;
      if (shmctl((int)a0, IPC_STAT, &hs) < 0) return eret();
      lx_shmid_put((struct lx_shmid64_ds*)a2, &hs);
      return 0;
    }
    return -LX_ENOSYS;                          // IPC_SET and the SHM_* extensions: nothing asks
  }

  case 190:                                                    // semget(key, nsems, flag)
    { int r = semget((key_t)a0, (int)a1, (int)a2); return r < 0 ? eret() : r; }
  case 193: {                                                  // semop(semid, sops, nsops)
    // struct sembuf is {unsigned short sem_num; short sem_op; short sem_flg;} on BOTH, so the
    // guest's array is passed through unchanged. Checked rather than assumed.
    { int r = semop((int)a0, (struct sembuf*)a1, (size_t)a2); return r < 0 ? eret() : 0; }
  }
  case 191: {                                                  // semctl(semid, num, cmd, arg)
    // THE COMMAND NUMBERS DIFFER, which is the one thing that would silently corrupt state if
    // passed through: Linux GETVAL=12/SETVAL=16/GETALL=13/SETALL=17/GETPID=11/GETNCNT=14/
    // GETZCNT=15, macOS GETVAL=5/SETVAL=8/GETALL=6/SETALL=9/GETPID=4/GETNCNT=3/GETZCNT=7.
    // A raw forward would turn SETVAL into something else entirely.
    int lcmd = (int)a2, mcmd;
    switch (lcmd) {
      case 0:  mcmd = IPC_RMID; break;
      case 11: mcmd = GETPID;   break;
      case 12: mcmd = GETVAL;   break;
      case 13: mcmd = GETALL;   break;
      case 14: mcmd = GETNCNT;  break;
      case 15: mcmd = GETZCNT;  break;
      case 16: mcmd = SETVAL;   break;
      case 17: mcmd = SETALL;   break;
      default: return -LX_ENOSYS;              // IPC_STAT/IPC_SET: struct layout differs, refuse
    }
    if (mcmd == SETVAL) {
      union semun { int val; struct semid_ds* buf; unsigned short* array; } su;
      su.val = (int)a3;
      { int r = semctl((int)a0, (int)a1, mcmd, su); return r < 0 ? eret() : r; }
    }
    if (mcmd == GETALL || mcmd == SETALL) {
      union semun { int val; struct semid_ds* buf; unsigned short* array; } su;
      su.array = (unsigned short*)a3;
      { int r = semctl((int)a0, (int)a1, mcmd, su); return r < 0 ? eret() : r; }
    }
    { int r = semctl((int)a0, (int)a1, mcmd); return r < 0 ? eret() : r; }
  }
  case 192: {                                                  // semtimedop -> semop
    // macOS has no semtimedop. The timeout is only a real-time upper bound; mapping to a plain
    // blocking semop is exact for a NULL or effectively-infinite timeout (the common case) and
    // the honest approximation otherwise — strictly better than the -ENOSYS it replaced.
    // struct sembuf is identical on both systems, so the guest's sops array passes through.
    int r = semop((int)a0, (struct sembuf*)a1, (size_t)a2);
    return r < 0 ? eret() : 0;
  }

  // ===== 213..241: memory ==================================================
  case 213: return 0;                                          // readahead: advisory, ignore
  case 217: case 218: case 219: return -LX_ENOSYS;             // add_key/request_key/keyctl
  case 220: return -LX_ENOSYS;   // clone — handled in the trap handler (needs the register file)
  case 221: {                                                 // execve(path, argv, envp)
    // Replacing the guest image in place would mean unwinding every mapping, the veneer arenas
    // and the trap tables, and re-entering the loader — all while running on the guest's own
    // stack. Re-execing the SHIM is the same thing done by the kernel instead: the host process
    // is replaced by a fresh shim that loads the new guest from scratch. File descriptors
    // survive execve unless they are CLOEXEC, which is exactly what a spawned helper needs.
    const char* path = (const char*)a0;
    char* const* gargv = (char* const*)a1;
    char* const* genvp = (char* const*)a2;
    if (!path) return -LX_EFAULT;
    return guest_execve(path, gargv, genvp);
  }
  case 223: return 0;                                          // fadvise64: advisory, ignore
  case 224: case 225: return -LX_EPERM;                        // swapon/swapoff
  case 227: {                                                  // msync (MS_SYNC value differs!)
    int lf = (int)a2, hf = 0;
    if (lf & LX_MS_ASYNC)      hf |= MS_ASYNC;
    if (lf & LX_MS_INVALIDATE) hf |= MS_INVALIDATE;
    if (lf & LX_MS_SYNC)       hf |= MS_SYNC;
    if (!hf) hf = MS_SYNC;
    return ok0(msync((void*)a0, (size_t)a1, hf));
  }
  case 228: return ok0(mlock((void*)a0, (size_t)a1));           // mlock
  case 229: return ok0(munlock((void*)a0, (size_t)a1));         // munlock
  case 230: return ok0(mlockall((int)a0));                      // mlockall (MCL_* match)
  case 231:
    // macOS munlockall() is itself ENOSYS. "Unlock everything" is vacuously true when we
    // never honored mlockall, so report success rather than leaking a host -ENOSYS.
    munlockall();
    return 0;
  case 232: {
    // mincore() is a POINTER-VALIDITY ORACLE, not just a residency query, and the difference
    // between Linux and macOS here is load-bearing.
    //
    // Linux fails with ENOMEM if ANY page of the range is unmapped. Mesa's EGL leans on exactly
    // that: _eglPointerIsDereferencable() ignores the returned residency bits entirely and
    // reports "this is a real pointer" whenever the call merely SUCCEEDS. It uses that to tell
    // an old wl_egl_window (whose first field is a wl_surface*) from a modern one (whose first
    // field is the integer version, 3).
    //
    // macOS mincore() succeeds for unmapped addresses, and the whole low 4 GB is the process's
    // __PAGEZERO mapping besides. A straight passthrough therefore told Mesa that the integer 3
    // was dereferenceable, and it duly called wl_proxy_create_wrapper((void*)3) and died on
    // `ldr x0, [x20, #0x18]`. Reproducing Linux's ENOMEM is what makes Wayland EGL work at all.
    if (a1 == 0) return 0;
    if (a0 < FOUR_GB) return -LX_ENOMEM;            // __PAGEZERO: never guest-dereferenceable
    if (!host_range_mapped(a0, (size_t)a1)) return -LX_ENOMEM;
    return ok0(mincore((void*)a0, (size_t)a1, (char*)a2));
  }
  case 234: return -LX_ENOSYS;                                  // remap_file_pages (dead on Linux too)
  case 235: return 0;                                           // mbind: single NUMA node, no-op
  case 236:                                                     // get_mempolicy
    if (a0) *(int*)a0 = 0;                                      // MPOL_DEFAULT
    return 0;
  case 237: return 0;                                           // set_mempolicy: no-op
  case 238: return -LX_ENOSYS;                                  // migrate_pages
  case 239: {                                                   // move_pages: query current process
    /* Shimmy presents one NUMA node. Darwin has no equivalent to migrating
     * Linux pages, but a query can report an actual resident guest page on
     * that node. Do not report a fabricated location for holes or pages the
     * host has not made resident. */
    long pid = (long)(int)a0;
    size_t count = (size_t)a1;
    void *const *pages = (void *const *)a2;
    const int *nodes = (const int *)a3;
    int *status = (int *)a4;
    unsigned flags = (unsigned)a5;
    if (flags & ~3u) return -LX_EINVAL;  // MPOL_MF_MOVE / MOVE_ALL
    if (pid && pid != syscalls_guest_pid()) return -LX_EOPNOTSUPP;
    if (nodes || flags) return -LX_EOPNOTSUPP; // no page migration
    if (!count) return 0;
    if (!pages || !status) return -LX_EFAULT;
    if (count > SIZE_MAX / sizeof(*pages) ||
        count > SIZE_MAX / sizeof(*status)) return -LX_EINVAL;
    if (!host_range_mapped((unsigned long)pages, count * sizeof(*pages)) ||
        !host_range_mapped((unsigned long)status, count * sizeof(*status)))
      return -LX_EFAULT;
    const size_t page = (size_t)getpagesize();
    for (size_t i = 0; i < count; ++i) {
      unsigned long addr = (unsigned long)pages[i];
      unsigned char resident = 0;
      if (addr < FOUR_GB || (addr & (page - 1)) ||
          !host_range_mapped(addr, page)) {
        status[i] = -LX_EFAULT;
      } else if (mincore((void *)addr, page, (char *)&resident) < 0) {
        status[i] = -LX_EFAULT;
      } else {
        status[i] = resident & 1 ? 0 : -LX_ENOENT;
      }
    }
    return 0;
  }
  case 241: return -LX_EPERM;                                   // perf_event_open
  // 243 recvmmsg is now HANDLED in net.c, next to sendmmsg. net_owns() claims both
  // before this table is reached; leaving either at -ENOSYS is a hard failure for any
  // glibc caller, not a graceful degradation.

  // ===== 260..294: modern =================================================
  case 260: {                                                   // wait4 (core normally owns this)
    int hopt = 0, lopt = (int)a2;
    if (lopt & LX_WNOHANG)    hopt |= WNOHANG;
    if (lopt & LX_WUNTRACED)  hopt |= WUNTRACED;
    if (lopt & LX_WCONTINUED) hopt |= WCONTINUED;
    struct rusage ru; memset(&ru, 0, sizeof ru);
    int status = 0;
    pid_t r = wait4((pid_t)a0, a1 ? &status : 0, hopt, &ru);
    if (r < 0) return eret();
    if (r > 0) syscalls_signal_child_reaped(r);
    if (a1) *(int*)a1 = status;                                 // wait status encoding matches
    if (a3) lx_rusage_put((struct lx_rusage*)a3, &ru);
    return r;
  }
  case 262: case 263: return -LX_EPERM;                         // fanotify_init/mark
  case 264: {                                                  // name_to_handle_at(dfd, path, handle, mount_id, flags)
    // Returns an opaque filesystem handle + a mount id for a path. systemd's is_idmapping_supported()
    // probes this to decide whether a directory sits on an idmapped mount; EOPNOTSUPP made the probe
    // fail and took down the RUNTIME_DIRECTORY step ("Failed to set up special execution directory in
    // /run: No such file or directory"). The shim has no real mount topology, so return an empty handle
    // and the host device number as the mount id; existence is checked so ENOENT still propagates.
    char host[PATH_MAX]; int hdirfd = -100;
    long e = resolve_at((int)a0, (const char*)a1, host, sizeof host, &hdirfd);
    if (e < 0) return e;
    struct stat st;
    if (fstatat(hdirfd, host, &st, 0) < 0) return eret();
    if (a2) {
      struct { unsigned int handle_bytes; int handle_type; }* fh = (void*)a2;
      fh->handle_bytes = 0;                                   // empty handle: nothing to store
      fh->handle_type = 0;
    }
    if (a3) *(int*)a3 = (int)st.st_dev;
    return 0;
  }
  case 265: return -LX_EOPNOTSUPP;                            // open_by_handle_at (no stored handles)
  case 266: return -LX_EPERM;                                   // clock_adjtime
  case 267: sync(); return 0;                                   // syncfs
  case 268: return -LX_EPERM;                                   // setns
  // 269 sendmmsg is now HANDLED in net.c -- it is not optional, glibc's
  // getaddrinfo(AF_UNSPEC) sends its A and AAAA queries in one sendmmsg and treats a
  // failure as fatal. Leaving it -ENOSYS here made a Debian rootfs unable to resolve
  // any name at all. net_owns() claims it before this table is reached.
  case 270: case 271: {                                         // process_vm_readv / writev
    // The guest shares OUR address space, so cross-"process" vm IO on ourselves is a memcpy.
    const struct iovec* liov = (const struct iovec*)a1;
    const struct iovec* riov = (const struct iovec*)a3;
    unsigned long ln = a2, rn = a4;
    if (!liov || !riov) return -LX_EFAULT;
    unsigned long li = 0, ri = 0; size_t lo = 0, ro = 0; long total = 0;
    while (li < ln && ri < rn) {
      size_t lrem = liov[li].iov_len - lo, rrem = riov[ri].iov_len - ro;
      size_t n = lrem < rrem ? lrem : rrem;
      if (n) {
        char* lp = (char*)liov[li].iov_base + lo;
        char* rp = (char*)riov[ri].iov_base + ro;
        if (nr == 270) memcpy(lp, rp, n); else memcpy(rp, lp, n);
        total += (long)n; lo += n; ro += n;
      }
      if (lo == liov[li].iov_len) { li++; lo = 0; }
      if (ro == riov[ri].iov_len) { ri++; ro = 0; }
    }
    return total;
  }
  case 272: {                                                   // kcmp
    // Only comparisons within this guest process have a reliable identity.
    // Linux compares kernel object pointers; fstat(dev, ino) is not a substitute
    // for an open-file-description identity (two independent opens can match).
    // Do not compare a guest PID with a Darwin PID or guess about a second fd.
    long self = syscalls_guest_pid();
    if ((long)(int)a0 != self || (long)(int)a1 != self)
      return -LX_EOPNOTSUPP;
    if (a2 > 7) return -LX_EINVAL; // KCMP_FILE..KCMP_EPOLL_TFD
    if (a2 == 0) { // KCMP_FILE: an fd always shares its own open description.
      if (a3 > INT_MAX || a4 > INT_MAX ||
          shim_fd_reserved((int)a3) || shim_fd_reserved((int)a4) ||
          fcntl((int)a3, F_GETFD) < 0 || fcntl((int)a4, F_GETFD) < 0)
        return -LX_EBADF;
      return a3 == a4 ? 0 : -LX_EOPNOTSUPP;
    }
    if (a2 == 7) return -LX_EOPNOTSUPP; // KCMP_EPOLL_TFD needs epoll target identity.
    // For one process, VM, FILES, FS, SIGHAND, IO, and SYSVSEM necessarily
    // refer to the same object; idx1/idx2 are ignored by Linux for these types.
    return 0;
  }
  case 273: return -LX_EPERM;                                   // finit_module
  case 274: {                                                   // sched_setattr
    /* Darwin priority is process-wide.  Only a lone guest thread may change
     * nice; otherwise a successful call would silently change its siblings.
     * RT/deadline/uclamp scheduling is not implemented. */
    struct lx_sched_attr_v0 {
      uint32_t size, policy;
      uint64_t flags;
      int32_t nice;
      uint32_t priority;
      uint64_t runtime, deadline, period;
    } attr;
    _Static_assert(sizeof(attr) == 48, "Linux sched_attr v0 ABI");
    long target = (long)(int)a0;
    if (!a1) return -LX_EFAULT;
    if (a2) return -LX_EINVAL;
    if (target < 0) return -LX_EINVAL;
    if (target && target != guest_tid() &&
        !(target == (long)getpid() && guest_tid() == 1))
      return guest_tid_known((int)target) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    uint32_t size = *(const uint32_t *)a1;
    if (size && size < sizeof(attr)) return -LX_EINVAL;
    if (size > 4096) return -LX_E2BIG;
    memcpy(&attr, (const void *)a1, sizeof(attr));
    if (size > sizeof(attr)) {
      const uint8_t *extra = (const uint8_t *)a1 + sizeof(attr);
      for (uint32_t i = sizeof(attr); i < size; i++)
        if (extra[i - sizeof(attr)]) return -LX_E2BIG;
    }
    if (attr.policy != 0 || attr.flags || attr.priority || attr.runtime ||
        attr.deadline || attr.period) return -LX_EOPNOTSUPP;
    if (attr.nice < -20 || attr.nice > 19) return -LX_EINVAL;
    errno = 0;
    int old_nice = getpriority(PRIO_PROCESS, 0);
    if (old_nice == -1 && errno) return eret();
    if (attr.nice == old_nice) return 0;
    if (guest_thread_count() != 1) return -LX_EOPNOTSUPP;
    return ok0(setpriority(PRIO_PROCESS, 0, attr.nice));
  }
  case 275: {                                                   // sched_getattr
    /* Shimmy exposes SCHED_OTHER for guest threads.  Report only the v0
     * sched_attr fields, with the nice value backed by the host process's
     * actual priority.  Do not invent RT, deadline or uclamp state. */
    struct lx_sched_attr_v0 {
      uint32_t size, policy;
      uint64_t flags;
      int32_t nice;
      uint32_t priority;
      uint64_t runtime, deadline, period;
    } attr = {0};
    _Static_assert(sizeof(attr) == 48, "Linux sched_attr v0 ABI");
    long target = (long)(int)a0;
    size_t size = (size_t)(unsigned int)a2;
    if (!a1 || target < 0 || size < sizeof(attr) || size > 4096)
      return -LX_EINVAL;
    if (target && target != guest_tid() &&
        !(target == (long)getpid() && guest_tid() == 1))
      return guest_tid_known((int)target) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    if (a3) return -LX_EINVAL;  /* No SCHED_DEADLINE dynamic fields. */
    errno = 0;
    int nice = getpriority(PRIO_PROCESS, 0);
    if (nice == -1 && errno) return eret();
    attr.size = sizeof(attr);
    attr.nice = nice;
    memcpy((void *)a1, &attr, sizeof(attr));
    if (size > sizeof(attr)) memset((char *)a1 + sizeof(attr), 0, size - sizeof(attr));
    return 0;
  }
  case 276: {                                                   // renameat2
    unsigned flags = (unsigned)a4;
    e  = resolve_at_nf((int)a0, (const char*)a1, host,  sizeof host,  &hdirfd,  1);
    if (e < 0) return e;
    e  = resolve_at_nf((int)a2, (const char*)a3, host2, sizeof host2, &hdirfd2, 1);
    if (e < 0) return e;
    if (flags == 0) return ok0(renameat(hdirfd, host, hdirfd2, host2));
#ifdef RENAME_EXCL
    unsigned hf = 0;
    if (flags & LX_RENAME_NOREPLACE) hf |= RENAME_EXCL;
    if (flags & LX_RENAME_EXCHANGE)  hf |= RENAME_SWAP;
    if (flags & ~(unsigned)(LX_RENAME_NOREPLACE | LX_RENAME_EXCHANGE)) return -LX_EINVAL;
    return ok0(renameatx_np(hdirfd, host, hdirfd2, host2, hf));
#else
    return -LX_EINVAL;
#endif
  }
  case 277: return -LX_ENOSYS;                                  // seccomp
  case 279: return do_memfd_create((const char*)a0, (unsigned)a1);
  case 280: return -LX_EPERM;                                   // bpf
  case 281: {                                                   // execveat(dirfd, path, argv, envp, flags)
    const char *path = (const char *)a1;
    unsigned flags = (unsigned)a4;
    if (!path) return -LX_EFAULT;
    if (flags & ~(LX_AT_EMPTY_PATH | LX_AT_SYMLINK_NOFOLLOW)) return -LX_EINVAL;
    if (!path[0]) {
      if (!(flags & LX_AT_EMPTY_PATH)) return -LX_ENOENT;
      // The existing exec bridge resolves /proc/self/fd/N through F_GETPATH.
      // Reject descriptors with no recoverable backing path rather than
      // claiming to support anonymous/deleted executables and failing after
      // the shim replaces its process image.
      int fd = (int)a0;
      struct stat st;
      if (fstat(fd, &st) < 0) return eret();
      if ((flags & LX_AT_SYMLINK_NOFOLLOW) && S_ISLNK(st.st_mode))
        return -LX_ELOOP;
      if (!S_ISREG(st.st_mode)) return -LX_EACCES;
      char host[PATH_MAX];
      if (fcntl(fd, F_GETPATH, host) < 0) return -LX_ENOSYS;
      struct stat named;
      if (stat(host, &named) < 0 || named.st_dev != st.st_dev ||
          named.st_ino != st.st_ino) return -LX_ENOSYS;
      char fdpath[64];
      snprintf(fdpath, sizeof fdpath, "/proc/self/fd/%d", fd);
      return guest_execve(fdpath, (char *const *)a2, (char *const *)a3);
    }
    char host[PATH_MAX], guest[PATH_MAX];
    long r = syscalls_resolve((int)a0, path,
                              (flags & LX_AT_SYMLINK_NOFOLLOW) != 0,
                              host, sizeof host, guest, sizeof guest);
    if (r < 0) return r;
    if (flags & LX_AT_SYMLINK_NOFOLLOW) {
      struct stat st;
      if (lstat(host, &st) < 0) return eret();
      if (S_ISLNK(st.st_mode)) return -LX_ELOOP;
    }
    return guest_execve(guest, (char *const *)a2, (char *const *)a3);
  }
  case 282: return -LX_ENOSYS;                                  // userfaultfd
  case 283: {                                                   // membarrier
    __sync_synchronize();
    if ((int)a0 == 0) return 0x1F;                              // CMD_QUERY: GLOBAL..REG_PRIVATE_EXP
    return 0;
  }
  case 284: return ok0(mlock((void*)a0, (size_t)a1));           // mlock2 (MLOCK_ONFAULT ignored)
  case 285: {                                                   // copy_file_range
    long long ioff, ooff, *ip = 0, *op = 0;
    if (a1) { ioff = *(long long*)a1; ip = &ioff; }
    if (a3) { ooff = *(long long*)a3; op = &ooff; }
    long r = bounce_copy((int)a0, ip, (int)a2, op, (size_t)a4, 0);
    if (r >= 0 && a1) *(long long*)a1 = ioff;
    if (r >= 0 && a3) *(long long*)a3 = ooff;
    return r;
  }
  case 286: {                                                   // preadv2
    // Linux's six-argument ABI puts rwf_t flags in x5; x4 is pos_h and is
    // irrelevant on a 64-bit guest.  A nonzero flag must not silently become
    // a blocking, ordinary read (notably RWF_NOWAIT and RWF_HIPRI).
    if ((unsigned int)a5) return -LX_EOPNOTSUPP;
    off_t pos = (off_t)a3;
    if (pos < -1) return -LX_EINVAL;
    // offset -1 uses and advances the open file description's current offset.
    ssize_t r = pos == -1 ? readv((int)a0, (const struct iovec*)a1, (int)a2)
                          : preadv((int)a0, (const struct iovec*)a1, (int)a2, pos);
    return r < 0 ? eret() : r;
  }
  case 287: {                                                   // pwritev2: unflagged subset
    // On native arm64 Linux, pos_from_hilo() receives the full 64-bit offset
    // in x3; shifting the x4 high argument twice by 32 discards it.  Offset -1
    // means writev at the fd's current position (including pipes), whereas
    // pwritev(-1) fails.  Never silently ignore a per-write durability,
    // append, atomic, or nonblocking flag: the host pwritev cannot honor it.
    if (a5) return -LX_EOPNOTSUPP;
    uint64_t pos = (uint64_t)a3;
    ssize_t r = (pos == UINT64_MAX)
      ? writev((int)a0, (const struct iovec*)a1, (int)a2)
      : pwritev((int)a0, (const struct iovec*)a1, (int)a2, (off_t)pos);
    return r < 0 ? eret() : r;
  }
  case 288: {                                                   // pkey_mprotect
    // ARM64 has no protection-key allocation in this guest. Linux's no-pkeys
    // implementation permits the default key 0 and the legacy key -1, both
    // of which use ordinary mprotect permissions. Never claim another key
    // was installed: that would silently omit its access restrictions.
    if (a0 & (16384UL - 1)) return -LX_EINVAL; // guest page size
    if (!a1) return 0; // Linux checks zero length before key allocation
    if ((int)a3 != 0 && (int)a3 != -1) return -LX_EINVAL;
    if (a2 & ~(unsigned long)(PROT_READ | PROT_WRITE | PROT_EXEC))
      return -LX_EINVAL;
    return gmem_mprotect(a0, (size_t)a1, (int)a2);
  }
  case 289: case 290: return -LX_ENOSYS;                        // pkey_alloc/free
  case 258:                                                     // statx alias (see report)
  case 291: return do_statx((int)a0, (const char*)a1, (int)a2, (unsigned)a3, (void*)a4);
  case 292: return -LX_ENOSYS;                                  // io_pgetevents
  case 294: return -LX_EPERM;                                   // kexec_file_load

  // ===== 403..456: the *_time64 tail and post-5.x additions ================
  // aarch64 is 64-bit-time from birth, so these normally never appear; map the cheap ones
  // anyway so a mis-built guest degrades gracefully instead of dying.
  case 403:                                                     // clock_gettime64
    return ok0(clock_gettime((clockid_t)((a0 == 1 || a0 == 4 || a0 == 6) ? CLOCK_UPTIME_RAW :
                                         a0 == 7 ? CLOCK_MONOTONIC_RAW : a0), (struct timespec*)a1));
  case 406:                                                     // clock_getres_time64
    return ok0(clock_getres((clockid_t)a0, (struct timespec*)a1));
  case 407: {                                                   // clock_nanosleep_time64
    // macOS has no clock_nanosleep; emulate with nanosleep (absolute -> relative by hand).
    const struct timespec* req = (const struct timespec*)a2;
    if (!req) return -LX_EFAULT;
    struct timespec rel = *req;
    if ((int)a1 & 1) {                                          // TIMER_ABSTIME
      struct timespec now;
      if (clock_gettime((clockid_t)((a0 == 1 || a0 == 4 || a0 == 6) ? CLOCK_UPTIME_RAW :
                                   a0 == 7 ? CLOCK_MONOTONIC_RAW : a0), &now) != 0) return eret();
      rel.tv_sec  = req->tv_sec - now.tv_sec;
      rel.tv_nsec = req->tv_nsec - now.tv_nsec;
      if (rel.tv_nsec < 0) { rel.tv_nsec += 1000000000; rel.tv_sec--; }
      if (rel.tv_sec < 0) return 0;                             // deadline already passed
    }
    if (nanosleep(&rel, (struct timespec*)a3) != 0) return eret();
    return 0;
  }
  case 412: {                                                   // utimensat_time64
    const struct timespec* ts = (const struct timespec*)a2;
    if (!a1) return ok0(futimens((int)a0, ts));
    e = resolve_at_nf((int)a0, (const char*)a1, host, sizeof host, &hdirfd,
                      ((int)a3 & LX_AT_SYMLINK_NOFOLLOW) != 0);
    if (e < 0) return e;
    return ok0(utimensat(hdirfd, host, ts, host_at_flags((int)a3)));
  }
  case 413: {                                                   // pselect6_time64
    int r = pselect((int)a0, (fd_set*)a1, (fd_set*)a2, (fd_set*)a3,
                    (const struct timespec*)a4, 0);
    return r < 0 ? eret() : r;
  }
  case 414: {                                                   // ppoll_time64
    const struct timespec* to = (const struct timespec*)a2;
    int ms = to ? (int)(to->tv_sec * 1000 + to->tv_nsec / 1000000) : -1;
    int r = poll((struct pollfd*)a0, (nfds_t)a1, ms);           // Linux POLL* values match
    return r < 0 ? eret() : r;
  }
  case 424: {                                                   // pidfd_send_signal(pidfd, sig, info, flags)
    (void)a2; (void)a3;
    if ((int)a1 == 0 && getenv("RDPIDFD_SOCKET"))
      return ok0(pidfd_shim_send_signal((int)a0, 0));
    int hsig = host_signo((int)a1);
    if (hsig <= 0) return hsig == 0 ? 0 : -LX_EINVAL;
    return ok0(pidfd_shim_send_signal((int)a0, hsig));
  }
  case 425: case 426: case 427: return -LX_ENOSYS;              // io_uring_*
  case 428: case 432: {                                       // open_tree / fsmount -> a real dir fd
    // fsmount's result is fd_reopen(mfd, O_DIRECTORY)'d by systemd's credential setup, so it must
    // be a real directory. Return the host /tmp dir fd.
    int fd = open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    return fd >= 0 ? fd : eret();
  }
  case 430: case 433: {                                       // fsopen / fspick -> unconnected DGRAM
    // libmount's mnt_context_read_mesgs() does `while (read(fd,...) != -1)` and loops forever on
    // EOF (0); an unconnected socket makes read() return -1 immediately. MEASURED: on macOS a
    // BLOCKING unconnected DGRAM read() does NOT return -1 — it blocks forever, so /usr/bin/mount
    // (tmp.mount's fsopen path) hung in read() and the boot stalled. Make it O_NONBLOCK so read()
    // returns EAGAIN at once and libmount's loop breaks.
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd >= 0) { fcntl(fd, F_SETFD, FD_CLOEXEC); fcntl(fd, F_SETFL, O_NONBLOCK); }
    return fd >= 0 ? fd : eret();
  }
  case 429: return 0;                                           // move_mount: no-op success
  case 431: return 0;                                           // fsconfig: no-op success
  case 434: return pidfd_open_shim((pid_t)(long)a0, (unsigned int)a1);   // pidfd_open
  case 435: {                                                   // clone3
    if (getenv("RDCLONEDBG")) {
      FILE* cf = fopen("/tmp/rd_clone_dbg.txt", "a");
      if (cf) {
        unsigned long flags = a0 ? *(unsigned long*)(uintptr_t)a0 : 0;
        unsigned long pidfd = a0 ? *(unsigned long*)(uintptr_t)(a0 + 8) : 0;
        unsigned long chtid = a0 ? *(unsigned long*)(uintptr_t)(a0 + 16) : 0;
        unsigned long ptid  = a0 ? *(unsigned long*)(uintptr_t)(a0 + 24) : 0;
        unsigned long stack = a0 ? *(unsigned long*)(uintptr_t)(a0 + 40) : 0;
        fprintf(cf, "clone3 a0=%#lx size=%#lx flags=%#lx pidfd_field=%#lx child_tid=%#lx parent_tid=%#lx stack=%#lx\n",
                a0, a1, flags, pidfd, chtid, ptid, stack); fclose(cf);
      }
    }
    return -LX_ENOSYS;                                          // clone3 (glibc may not fall back)
  }
  case 436: {                                                   // close_range(first, last, flags)
    // This walks the HOST fd table, because the guest has no other one — a guest fd IS a host fd.
    // For close(2) that is harmless: the guest names a single descriptor it believes it owns. For
    // close_range it is not, because the guest names a RANGE it has never looked at. The idiom is
    // completely routine — glibc's closefrom()/posix_spawn, python's subprocess, glib's g_spawn,
    // libdbus all issue close_range(3, ~0U, 0) before exec — and it used to take the shim's own
    // private stderr with it, after which every shim diagnostic went to a closed descriptor and
    // said nothing. MEASURED with RDPATHS=1: the [path] line for the openat BEFORE the call
    // appeared, the one AFTER did not. tests/wl/closerange.c is written so that is visible.
    //
    // Descriptors the shim registered as its own are therefore skipped. A guest can still close
    // one by number via close(2) — it can only know the number if it genuinely owns it — so this
    // costs the guest nothing it is entitled to.
    unsigned first = (unsigned)a0, last = (unsigned)a1;
    unsigned flags = (unsigned)a2;
    // CLOSE_RANGE_UNSHARE (2) asks for a private copy of the fd table before closing. There is no
    // table to unshare here, so the request is vacuously satisfied rather than refused — glibc
    // passes it on some spawn paths, and an EINVAL there is a failure the caller does not expect.
    // CLOSE_RANGE_CLOEXEC (4) marks instead of closing, and must NOT be treated as "close".
    if (flags & ~(unsigned)(2 | 4)) return -LX_EINVAL;
    if (first > last) return -LX_EINVAL;
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd <= 0 || maxfd > 65536) maxfd = 65536;
    if (last > (unsigned)maxfd) last = (unsigned)maxfd;
    if (getenv("IDLE_PIPE_TRACE") && first <= 11 && last >= 11)
      fprintf(stderr, "[idle-socket] pid=%d op=close_range first=%u last=%u flags=%u\n",
              getpid(), first, last, flags);
    for (unsigned fd = first; fd <= last; fd++) {
      if (shim_fd_reserved((int)fd)) continue;
      if (pidfd_shim_pid((int)fd) > 0) continue;                  // leak pidfds, like close(2)
      if (flags & 4) {                                          // CLOSE_RANGE_CLOEXEC
        int fl = fcntl((int)fd, F_GETFD);
        if (fl >= 0) fcntl((int)fd, F_SETFD, fl | FD_CLOEXEC);
      } else {
        drm_forget_fd((int)fd);                                 // see the close(2) case for why
        evdev_forget_fd((int)fd);
        net_fd_closed((int)fd);
        close((int)fd);
      }
    }
    return 0;
  }
  case 438: {                                                   // pidfd_getfd: self regular-file subset
    // Linux always creates a CLOEXEC copy of the target's open file description.
    // A guest fd is a host fd, but only a regular file beneath the guest root
    // is safe to duplicate here: synthetic device/socket fds need their own
    // per-fd registry handoff, and another process has a different fd table.
    if ((unsigned int)a2) return -LX_EINVAL;
    int pfd = (int)a0, target = (int)a1;
    if (pfd < 0 || fcntl(pfd, F_GETFD) < 0) return -LX_EBADF;
    pid_t pid = pidfd_shim_pid(pfd);
    if (pid <= 0) return -LX_EBADF;
    if (pid != getpid()) return -LX_EOPNOTSUPP;
    // The ordinary Shimmy pidfd is a kqueue. A socket-backed pidfd can be
    // authenticated, but its descriptor transfer needs separate coverage.
    if (kevent(pfd, NULL, 0, NULL, 0, NULL) < 0) return -LX_EOPNOTSUPP;
    if (target < 0 || shim_fd_reserved(target)) return -LX_EBADF;
    struct stat st;
    if (fstat(target, &st) < 0) return -LX_EBADF;
    if (!S_ISREG(st.st_mode)) return -LX_EOPNOTSUPP;
    char path[PATH_MAX];
    const char* root = syscalls_get_root();
    size_t rootlen = root ? strlen(root) : 0;
    if (!rootlen || fcntl(target, F_GETPATH, path) < 0 ||
        strncmp(path, root, rootlen) != 0 ||
        (path[rootlen] != '/' && path[rootlen] != 0))
      return -LX_EOPNOTSUPP;
    int copy = fcntl(target, F_DUPFD_CLOEXEC, 0);
    return copy < 0 ? eret() : copy;
  }
  case 439: {                                                   // faccessat2(dfd, path, mode, flags)
    // AT_EMPTY_PATH + "" means "check the file the fd refers to". macOS faccessat() has no
    // AT_EMPTY_PATH, so resolve the fd's own path (F_GETPATH) and check that. systemd's
    // pin_callout_binary() does faccessat2(fd, "", X_OK, AT_EMPTY_PATH) on the freshly opened
    // systemd-executor binary; returning ENOENT here made manager_new() fail with -ENOENT and
    // froze PID 1 before it could start getty/dbus.
    if ((int)a3 & LX_AT_EMPTY_PATH) {
      const char* p = (const char*)a1;
      if (!p || !p[0]) {
        int fd = (int)a0;
        if (fd == LX_AT_FDCWD) return ok0(faccessat(AT_FDCWD, ".", (int)a2, AT_EACCESS));
        char hp[PATH_MAX];
        if (fcntl(fd, F_GETPATH, hp) == 0)
          return ok0(faccessat(AT_FDCWD, hp, (int)a2, 0));
        // No path (socket/pipe/memfd): the fd is a live open handle, so F_OK/X_OK/W_OK/R_OK pass.
        return 0;
      }
    }
    e = resolve_at_nf((int)a0, (const char*)a1, host, sizeof host, &hdirfd,
                      ((int)a3 & LX_AT_SYMLINK_NOFOLLOW) != 0);
    if (e < 0) return e;
    int hf = host_at_flags((int)a3);
    if ((int)a3 & LX_AT_EACCESS) hf |= AT_EACCESS;
    return ok0(faccessat(hdirfd, host, (int)a2, hf));
  }
  case 440: return 0;                                           // process_madvise: advisory
  case 441: return -LX_ENOSYS;                                  // epoll_pwait2 (falls back)
  case 442: return 0;                                           // mount_setattr: no-op success
  case 443: return -LX_EPERM;                                   // quotactl_fd
  case 452: {                                                   // fchmodat2(dfd, path, mode, flags)
    // Linux fchmodat2 accepts only these two AT_* bits. Passing other Linux bits through
    // host_at_flags silently ignores them, turning an invalid request into a chmod.
    if (a3 & ~(unsigned long)(LX_AT_EMPTY_PATH | LX_AT_SYMLINK_NOFOLLOW))
      return -LX_EINVAL;
    if (!a1) return -LX_EFAULT;
    // AT_EMPTY_PATH + empty path == "chmod the dirfd itself". systemd's executor uses this while
    // setting up a service's RuntimeDirectory; the generic resolve below fails ENOENT on an empty
    // path and the executor then exits 233 (EXIT_RUNTIME_DIRECTORY), failing the unit.
    if ((a3 & LX_AT_EMPTY_PATH) && !((const char*)a1)[0]) {
      // AT_FDCWD means the guest's current directory, which need not equal the host cwd.
      if ((int)a0 == LX_AT_FDCWD) {
        e = resolve_at_nf(LX_AT_FDCWD, ".", host, sizeof host, &hdirfd, 0);
        if (e < 0) return e;
        return ok0(chmod(host, (mode_t)a2));
      }
      return ok0(fchmod((int)a0, (mode_t)a2));
    }
    e = resolve_at_nf((int)a0, (const char*)a1, host, sizeof host, &hdirfd,
                      ((int)a3 & LX_AT_SYMLINK_NOFOLLOW) != 0);
    if (e < 0) return e;
    return ok0(fchmodat(hdirfd, host, (mode_t)a2, host_at_flags((int)a3)));
  }
  case 463: {                                                   // setxattrat: pathname subset
    // Linux uapi xattr_args: { u64 value; u32 size; u32 flags }.
    // Keep the mutation inside the resolved guest root; fd/AT_EMPTY_PATH
    // needs separate guest-fd provenance and is deliberately unsupported.
    struct { uint64_t value; uint32_t size, flags; } args;
    if (a5 < sizeof args) return -LX_EINVAL;
    if (a5 > 16384) return -LX_E2BIG;
    if (!a4 || gmem_guest_export_copyin(NULL, a4, &args, sizeof args) != 0)
      return -LX_EFAULT;
    for (size_t off = sizeof args; off < a5;) {
      unsigned char extra[256];
      size_t n = a5 - off < sizeof extra ? a5 - off : sizeof extra;
      if (gmem_guest_export_copyin(NULL, a4 + off, extra, n) != 0)
        return -LX_EFAULT;
      for (size_t i = 0; i < n; ++i) if (extra[i]) return -LX_E2BIG;
      off += n;
    }
    if (args.flags & ~3u) return -LX_EINVAL;  // Linux CREATE=1, REPLACE=2
    if (args.flags == 3) return -LX_EINVAL;
    if (args.size > 65536u) return -LX_E2BIG;
    if (a2 & ~(unsigned long)(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH))
      return -LX_EINVAL;
    if (!a1) return (a2 & LX_AT_EMPTY_PATH) ? -LX_EOPNOTSUPP : -LX_EFAULT;
    char guest_path[PATH_MAX], guest_name[256];
    e = copy_guest_cstr(a1, guest_path, sizeof guest_path);
    if (e < 0) return e;
    if (!guest_path[0])
      return (a2 & LX_AT_EMPTY_PATH) ? -LX_EOPNOTSUPP : -LX_ENOENT;
    e = copy_guest_cstr(a3, guest_name, sizeof guest_name);
    if (e < 0) return e;
    if (!guest_name[0]) return -LX_ERANGE;
    e = resolve_at_nf((int)a0, guest_path, host, sizeof host, &hdirfd,
                      (a2 & LX_AT_SYMLINK_NOFOLLOW) != 0);
    if (e < 0) return e;
    void* value = args.size ? malloc(args.size) : NULL;
    if (args.size && !value) return -LX_ENOMEM;
    if (args.size && gmem_guest_export_copyin(NULL, args.value, value, args.size) != 0) {
      free(value);
      return -LX_EFAULT;
    }
    int hopt = (a2 & LX_AT_SYMLINK_NOFOLLOW) ? XATTR_NOFOLLOW : 0;
    if (args.flags & 1) hopt |= XATTR_CREATE;
    if (args.flags & 2) hopt |= XATTR_REPLACE;
    int result = setxattr(host, guest_name, value, args.size, 0, hopt);
    long ret = result < 0 ? eret() : 0;
    free(value);
    return ret;
  }
  case 464: {                                                   // getxattrat: pathname subset
    // Linux uapi struct xattr_args is { u64 value; u32 size; u32 flags }.
    // Only the pathname route is implemented.  In particular, AT_EMPTY_PATH
    // must not turn a shim-private host descriptor into an xattr capability.
    struct { uint64_t value; uint32_t size, flags; } args;
    if (a5 < sizeof args) return -LX_EINVAL;
    if (a5 > 16384) return -LX_E2BIG;  // copy_struct_from_user page-size ceiling
    if (!a4 || gmem_guest_export_copyin(NULL, a4, &args, sizeof args) != 0)
      return -LX_EFAULT;
    // A larger zero-extended argument structure is permitted by Linux.
    for (size_t off = sizeof args; off < a5;) {
      unsigned char extra[256];
      size_t n = a5 - off < sizeof extra ? a5 - off : sizeof extra;
      if (gmem_guest_export_copyin(NULL, a4 + off, extra, n) != 0)
        return -LX_EFAULT;
      for (size_t i = 0; i < n; ++i) if (extra[i]) return -LX_E2BIG;
      off += n;
    }
    if (args.flags) return -LX_EINVAL;
    if (a2 & ~(unsigned long)(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH))
      return -LX_EINVAL;
    if (!a1) return (a2 & LX_AT_EMPTY_PATH) ? -LX_EOPNOTSUPP : -LX_EFAULT;
    char guest_path[PATH_MAX], guest_name[256];
    e = copy_guest_cstr(a1, guest_path, sizeof guest_path);
    if (e < 0) return e;
    if (!guest_path[0])
      return (a2 & LX_AT_EMPTY_PATH) ? -LX_EOPNOTSUPP : -LX_ENOENT;
    // fd/AT_EMPTY_PATH needs guest-fd ownership proof; do not use a host fd.
    e = copy_guest_cstr(a3, guest_name, sizeof guest_name);
    if (e < 0) return e;
    e = resolve_at_nf((int)a0, guest_path, host, sizeof host, &hdirfd,
                      (a2 & LX_AT_SYMLINK_NOFOLLOW) != 0);
    if (e < 0) return e;
    size_t cap = args.size > 65536u ? 65536u : args.size;
    void* value = cap ? malloc(cap) : NULL;
    if (cap && !value) return -LX_ENOMEM;
    ssize_t got = getxattr(host, guest_name, value, cap, 0,
                           (a2 & LX_AT_SYMLINK_NOFOLLOW) ? XATTR_NOFOLLOW : 0);
    if (got < 0) {
      long error = (errno == ERANGE && cap == 65536u) ? -LX_E2BIG : eret();
      free(value);
      return error;
    }
    if (cap && (size_t)got > cap) { free(value); return -LX_EIO; }
    if (got && cap && gmem_guest_copyout_checked(args.value, value, (size_t)got) != 0) {
      free(value); return -LX_EFAULT;
    }
    free(value);
    return got;
  }
  case 465: {                                                   // listxattrat(dfd, path, flags, list, size)
    // Linux fs/xattr.c accepts exactly these flags. Unlike getxattrat, this
    // syscall has five scalar arguments and no xattr_args structure.
    if (a2 & ~(unsigned long)(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH))
      return -LX_EINVAL;
    const char* path = (const char*)a1;
    size_t size = (size_t)a4;
    if (size > 65536) size = 65536;                              // XATTR_LIST_MAX
    ssize_t r;
    if ((a2 & LX_AT_EMPTY_PATH) && (!path || !path[0])) {
      int fd = (int)a0;
      if (fd < 0 || shim_fd_reserved(fd) || fcntl(fd, F_GETFD) < 0)
        return -LX_EBADF;
      // An unrecognized host fd must not expose host-private xattrs. For now,
      // support only ordinary files/directories opened beneath the guest root.
      struct stat st;
      char fdpath[PATH_MAX];
      const char* root = syscalls_get_root();
      size_t rootlen = root ? strlen(root) : 0;
      if (fstat(fd, &st) < 0) return -LX_EBADF;
      if ((!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) || !rootlen ||
          fcntl(fd, F_GETPATH, fdpath) < 0 ||
          strncmp(fdpath, root, rootlen) != 0 ||
          (fdpath[rootlen] != '/' && fdpath[rootlen] != 0))
        return -LX_EOPNOTSUPP;
      r = flistxattr(fd, (char*)a3, size, 0);
    } else {
      if (!path) return -LX_EFAULT;
      if (!path[0]) return -LX_ENOENT;
      e = resolve_at_nf((int)a0, path, host, sizeof host, &hdirfd,
                        (a2 & LX_AT_SYMLINK_NOFOLLOW) != 0);
      if (e < 0) return e;
      r = listxattr(host, (char*)a3, size,
                    (a2 & LX_AT_SYMLINK_NOFOLLOW) ? XATTR_NOFOLLOW : 0);
    }
    if (r < 0 && errno == ERANGE && size >= 65536) return -LX_E2BIG;
    return r < 0 ? eret() : r;
  }
  case 466: {                                                   // removexattrat(dfd, path, flags, name)
    if (a2 & ~(unsigned long)(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH))
      return -LX_EINVAL;
    // The fd/empty-path form needs proof that the descriptor belongs to the
    // guest. Never hand a shim-private host descriptor to fremovexattr.
    if (!a1) return (a2 & LX_AT_EMPTY_PATH) ? -LX_EOPNOTSUPP : -LX_EFAULT;
    char guest_path[PATH_MAX], guest_name[256];
    e = copy_guest_cstr(a1, guest_path, sizeof guest_path);
    if (e < 0) return e;
    if (!guest_path[0])
      return (a2 & LX_AT_EMPTY_PATH) ? -LX_EOPNOTSUPP : -LX_ENOENT;
    e = copy_guest_cstr(a3, guest_name, sizeof guest_name);
    if (e < 0) return e;
    e = resolve_at_nf((int)a0, guest_path, host, sizeof host, &hdirfd,
                      (a2 & LX_AT_SYMLINK_NOFOLLOW) != 0);
    if (e < 0) return e;
    return ok0(removexattr(host, guest_name,
                           (a2 & LX_AT_SYMLINK_NOFOLLOW) ? XATTR_NOFOLLOW : 0));
  }

  default:
    return -LX_ENOSYS;                                          // genuinely not implemented
  }
}
