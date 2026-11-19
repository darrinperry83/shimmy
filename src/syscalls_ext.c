#include "guest_ownership.h"
#include "guest_cred_registry.h"
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
#include <stdint.h>
#include <stddef.h>
#include <signal.h>
#include <poll.h>
#include <sched.h>
#include <pthread.h>
#include "guestthread.h"
#include "execve.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <sys/timex.h>
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
#include "landlock.h"
#include "seccomp_runtime.h"
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
#define LX_EISDIR         21
#define LX_EINVAL         22
#define LX_EOVERFLOW      75
#define LX_ESPIPE         29
#define LX_ERANGE         34
#define LX_ENAMETOOLONG   36
#define LX_ENOSYS         38
#define LX_ELOOP          40
#define LX_EOPNOTSUPP     95

// Prototype native-AIO context store. The handle is the mapped guest ring
// address, as on Linux. Do not merge this alone: io_destroy/io_submit and
// completion retrieval must use the same registry before io_setup is exposed.
struct lx_aio_ring_header {
  uint32_t id, nr, head, tail;
  uint32_t magic, compat_features, incompat_features, header_length;
};
struct lx_aio_event {
  uint64_t data, obj;
  int64_t res, res2;
};
struct lx_aio_iocb {
  uint64_t data;
  uint32_t key, rw_flags;
  uint16_t opcode;
  int16_t reqprio;
  uint32_t fd;
  uint64_t buf, nbytes;
  int64_t offset;
  uint64_t reserved2;
  uint32_t flags, resfd;
};
_Static_assert(sizeof(struct lx_aio_iocb) == 64, "Linux ARM64 iocb size");
struct lx_aio_context {
  uint64_t handle;
  size_t mapping_length;
  unsigned requested_events;
  pid_t host_pid;
  struct lx_aio_event* completions;
  unsigned completion_head, completion_tail, completion_count;
  unsigned ring_head, ring_tail;
};
static pthread_mutex_t g_lx_aio_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_lx_aio_cv = PTHREAD_COND_INITIALIZER;
static struct lx_aio_context g_lx_aio_contexts[32];

// A guest may consume the mmap ring without calling io_getevents. Reconcile
// that head before using the host queue, so a later syscall cannot redeliver
// those events. This also returns capacity to io_submit direct-ring readers.
// A missing/unmapped or malformed ring head leaves the host queue authoritative.
static void lx_aio_sync_ring_head_locked(struct lx_aio_context* ctx) {
  uint32_t head;
  unsigned ring_nr = ctx->requested_events + 1;
  if (gmem_guest_export_copyin(NULL,
          ctx->handle + offsetof(struct lx_aio_ring_header, head),
          &head, sizeof head) != 0 || head >= ring_nr)
    return;
  unsigned delta = (head + ring_nr - ctx->ring_head) % ring_nr;
  if (delta > ctx->completion_count) return;
  ctx->completion_head = (ctx->completion_head + delta) % ctx->requested_events;
  ctx->completion_count -= delta;
  ctx->ring_head = head;
}

static long lx_io_setup(unsigned nr_events, uint64_t ctxp) {
  uint64_t existing;
  if (!ctxp || gmem_guest_export_copyin(NULL, ctxp, &existing, sizeof existing) != 0)
    return -LX_EFAULT;
  if (existing || !nr_events) return -LX_EINVAL;
  // Linux's default aio-max-nr is 65536. This prototype deliberately has a
  // smaller per-process bound until completion storage and quota are tested.
  if (nr_events > 1024) return -LX_EAGAIN;

  pthread_mutex_lock(&g_lx_aio_mu);
  unsigned slot = 32, total = 0;
  for (unsigned i = 0; i < 32; ++i) {
    if (!g_lx_aio_contexts[i].handle && slot == 32) slot = i;
    if (g_lx_aio_contexts[i].host_pid == getpid())
      total += g_lx_aio_contexts[i].requested_events;
  }
  if (slot == 32 || total + nr_events > 1024) {
    pthread_mutex_unlock(&g_lx_aio_mu);
    return -LX_EAGAIN;
  }
  struct lx_aio_event* completions = calloc(nr_events, sizeof *completions);
  if (!completions) {
    pthread_mutex_unlock(&g_lx_aio_mu);
    return -LX_ENOMEM;
  }
  // One spare ring entry distinguishes full from empty. The Linux ring
  // header is 32 bytes followed by 32-byte io_event records.
  size_t length = sizeof(struct lx_aio_ring_header) + ((size_t)nr_events + 1) * 32;
  long mapping = gmem_mmap(0, length, PROT_READ | PROT_WRITE, 0x01 | 0x20, -1, 0);
  if (mapping < 0) {
    free(completions);
    pthread_mutex_unlock(&g_lx_aio_mu);
    return mapping;
  }
  struct lx_aio_ring_header ring = {
    .id = slot + 1, .nr = nr_events + 1,
    .magic = 0xa10a10a1u, .compat_features = 1,
    .header_length = sizeof(struct lx_aio_ring_header),
  };
  if (gmem_guest_copyout_checked((uint64_t)mapping, &ring, sizeof ring) != 0 ||
      gmem_guest_copyout_checked(ctxp, &mapping, sizeof mapping) != 0) {
    gmem_munmap((unsigned long)mapping, length);
    free(completions);
    pthread_mutex_unlock(&g_lx_aio_mu);
    return -LX_EFAULT;
  }
  g_lx_aio_contexts[slot] = (struct lx_aio_context){
    .handle = (uint64_t)mapping, .mapping_length = length,
    .requested_events = nr_events, .host_pid = getpid(),
    .completions = completions,
  };
  pthread_mutex_unlock(&g_lx_aio_mu);
  return 0;
}

// io_destroy(2) removes the context from this process before unmapping its
// guest ring. The registry lock also prevents a second destroy from claiming
// the same handle. A later io_submit backend must keep in-flight requests
// accounted for and drain them before this returns; none can exist yet.
static long lx_io_destroy(uint64_t handle) {
  if (!handle) return -LX_EINVAL;
  pthread_mutex_lock(&g_lx_aio_mu);
  unsigned slot = 32;
  for (unsigned i = 0; i < 32; ++i) {
    if (g_lx_aio_contexts[i].handle == handle &&
        g_lx_aio_contexts[i].host_pid == getpid()) {
      slot = i;
      break;
    }
  }
  if (slot == 32) {
    pthread_mutex_unlock(&g_lx_aio_mu);
    return -LX_EINVAL;
  }
  size_t length = g_lx_aio_contexts[slot].mapping_length;
  free(g_lx_aio_contexts[slot].completions);
  g_lx_aio_contexts[slot] = (struct lx_aio_context){0};
  // Linux tears down the context even when userspace has unmapped the ring.
  // The mapping operation's result is not the syscall's completion status.
  (void)gmem_munmap((unsigned long)handle, length);
  pthread_cond_broadcast(&g_lx_aio_cv);
  pthread_mutex_unlock(&g_lx_aio_mu);
  return 0;
}

// A bounded, synchronously completed AIO submission. Linux permits a regular
// file request to finish inline; we still publish a real io_event for the
// subsequent io_getevents family call. Holding the registry lock pins the
// context through completion and makes io_destroy wait for this operation.
// This stage is not exposed on development until io_getevents is implemented.
static long lx_io_submit(uint64_t handle, long nr, uint64_t iocbpp) {
  if (nr < 0) return -LX_EINVAL;
  pthread_mutex_lock(&g_lx_aio_mu);
  struct lx_aio_context* ctx = NULL;
  for (unsigned i = 0; i < 32; ++i)
    if (g_lx_aio_contexts[i].handle == handle &&
        g_lx_aio_contexts[i].host_pid == getpid()) {
      ctx = &g_lx_aio_contexts[i];
      break;
    }
  if (!ctx) { pthread_mutex_unlock(&g_lx_aio_mu); return -LX_EINVAL; }
  if (!nr) { pthread_mutex_unlock(&g_lx_aio_mu); return 0; }
  if (nr > ctx->requested_events) nr = ctx->requested_events;
  long submitted = 0, error = 0;
  for (; submitted < nr; ++submitted) {
    lx_aio_sync_ring_head_locked(ctx);
    if (ctx->completion_count >= ctx->requested_events) { error = -LX_EAGAIN; break; }
    uint64_t iocb_addr = 0;
    if (!iocbpp ||
        gmem_guest_export_copyin(NULL, iocbpp + (uint64_t)submitted * 8,
                                 &iocb_addr, sizeof iocb_addr) != 0 || !iocb_addr) {
      error = -LX_EFAULT; break;
    }
    struct lx_aio_iocb cb;
    if (gmem_guest_export_copyin(NULL, iocb_addr, &cb, sizeof cb) != 0) {
      error = -LX_EFAULT; break;
    }
    if (cb.reserved2 || cb.rw_flags || cb.flags || cb.reqprio ||
        (cb.opcode != 0 && cb.opcode != 1) || cb.offset < 0 ||
        cb.nbytes > 1048576 || cb.nbytes > SSIZE_MAX) {
      error = -LX_EOPNOTSUPP; break;
    }
    int fd = (int)cb.fd;
    if (fd < 0 || shim_fd_reserved(fd)) { error = -LX_EBADF; break; }
    // Duplicate before inspection so a concurrent guest close cannot turn
    // this request into an operation on a later reuse of the descriptor.
    int pinned = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (pinned < 0) { error = -linux_errno_from_host(errno); break; }
    struct stat st;
    char path[PATH_MAX];
    const char* root = syscalls_get_root();
    size_t rootlen = root ? strlen(root) : 0;
    if (fstat(pinned, &st) < 0 || !S_ISREG(st.st_mode) || !rootlen ||
        fcntl(pinned, F_GETPATH, path) < 0 ||
        strncmp(path, root, rootlen) != 0 ||
        (path[rootlen] != '/' && path[rootlen] != 0)) {
      close(pinned); error = -LX_EOPNOTSUPP; break;
    }
    void* data = cb.nbytes ? malloc((size_t)cb.nbytes) : NULL;
    if (cb.nbytes && !data) { close(pinned); error = -LX_ENOMEM; break; }
    if (cb.opcode == 1 && cb.nbytes &&
        gmem_guest_export_copyin(NULL, cb.buf, data, (size_t)cb.nbytes) != 0) {
      free(data); close(pinned); error = -LX_EFAULT; break;
    }
    // The kernel writes aio_key=0 into the caller's iocb on accepted work.
    uint32_t zero = 0;
    if (gmem_guest_copyout_checked(iocb_addr + 8, &zero, sizeof zero) != 0) {
      free(data); close(pinned); error = -LX_EFAULT; break;
    }
    ssize_t done = cb.opcode == 0
        ? pread(pinned, data, (size_t)cb.nbytes, (off_t)cb.offset)
        : pwrite(pinned, data, (size_t)cb.nbytes, (off_t)cb.offset);
    int saved_errno = errno;
    close(pinned);
    int64_t result = done < 0 ? -linux_errno_from_host(saved_errno) : done;
    if (cb.opcode == 0 && done > 0 &&
        gmem_guest_copyout_checked(cb.buf, data, (size_t)done) != 0)
      result = -LX_EFAULT;
    free(data);
    struct lx_aio_event event = {
      .data = cb.data, .obj = iocb_addr, .res = result, .res2 = 0,
    };
    ctx->completions[ctx->completion_tail] = event;
    ctx->completion_tail = (ctx->completion_tail + 1) % ctx->requested_events;
    ++ctx->completion_count;
    // Mirror into the Linux ring for direct-ring readers. The host queue is
    // authoritative so io_getevents still works if userspace unmapped it.
    unsigned pos = ctx->ring_tail;
    uint64_t event_addr = handle + sizeof(struct lx_aio_ring_header) +
                          (uint64_t)pos * sizeof event;
    if (gmem_guest_copyout_checked(event_addr, &event, sizeof event) == 0) {
      uint32_t tail = (pos + 1) % (ctx->requested_events + 1);
      if (gmem_guest_copyout_checked(handle + offsetof(struct lx_aio_ring_header, tail),
                                     &tail, sizeof tail) == 0)
        ctx->ring_tail = tail;
    }
  }
  if (submitted) pthread_cond_broadcast(&g_lx_aio_cv);
  pthread_mutex_unlock(&g_lx_aio_mu);
  return submitted ? submitted : error;
}

// Every request in the staged regular-file subset finishes synchronously in
// io_submit. Linux io_cancel reads aio_key before looking up the context, then
// returns EINVAL if the request is no longer on its active list. Preserve the
// queued completion: a failed cancellation does not consume it. The result
// pointer is unused by current Linux kernels for this failure path.
static long lx_io_cancel(uint64_t handle, uint64_t iocb_addr,
                          uint64_t result_addr) {
  (void)handle;
  (void)result_addr;
  uint32_t key;
  if (!iocb_addr ||
      gmem_guest_export_copyin(NULL, iocb_addr +
                               offsetof(struct lx_aio_iocb, key),
                               &key, sizeof key) != 0)
    return -LX_EFAULT;
  if (key) return -LX_EINVAL;
  // A valid context has no cancellable in-flight request in this subset.
  // Invalid contexts carry the same Linux EINVAL result.
  return -LX_EINVAL;
}

// io_getevents(2), bounded to this process's synchronous regular-file AIO
// completions. The timeout is relative, as in Linux. We wait only for an
// already accepted request (or another guest thread's submission); no host
// operation runs in the background in this stage.
static long lx_io_getevents(uint64_t handle, long min_nr, long nr,
                            uint64_t events_addr, uint64_t timeout_addr) {
  struct { int64_t tv_sec, tv_nsec; } timeout;
  int have_timeout = timeout_addr != 0;
  if (have_timeout) {
    if (gmem_guest_export_copyin(NULL, timeout_addr, &timeout, sizeof timeout) != 0)
      return -LX_EFAULT;
    if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000)
      return -LX_EINVAL;
  }
  if (min_nr < 0 || nr < 0 || min_nr > nr) return -LX_EINVAL;

  pthread_mutex_lock(&g_lx_aio_mu);
  struct lx_aio_context* ctx = NULL;
  for (unsigned i = 0; i < 32; ++i)
    if (g_lx_aio_contexts[i].handle == handle &&
        g_lx_aio_contexts[i].host_pid == getpid()) {
      ctx = &g_lx_aio_contexts[i];
      break;
    }
  if (!ctx) { pthread_mutex_unlock(&g_lx_aio_mu); return -LX_EINVAL; }
  if (!nr) { pthread_mutex_unlock(&g_lx_aio_mu); return 0; }
  if (min_nr > ctx->requested_events) {
    pthread_mutex_unlock(&g_lx_aio_mu);
    return -LX_EOPNOTSUPP; // needs incremental draining across submissions
  }
  if (nr > ctx->requested_events) nr = ctx->requested_events;
  lx_aio_sync_ring_head_locked(ctx);
  struct timespec deadline = {0};
  if (have_timeout) {
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
      pthread_mutex_unlock(&g_lx_aio_mu);
      return -LX_EIO;
    }
    if (timeout.tv_sec > INT64_MAX - deadline.tv_sec) {
      deadline.tv_sec = INT64_MAX;
      deadline.tv_nsec = 999999999;
    } else {
      deadline.tv_sec += (time_t)timeout.tv_sec;
      deadline.tv_nsec += (long)timeout.tv_nsec;
      if (deadline.tv_nsec >= 1000000000) {
        if (deadline.tv_sec < INT64_MAX) ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000;
      }
    }
  }
  while (ctx->completion_count < (unsigned)min_nr) {
    int wait_err = have_timeout
        ? pthread_cond_timedwait(&g_lx_aio_cv, &g_lx_aio_mu, &deadline)
        : pthread_cond_wait(&g_lx_aio_cv, &g_lx_aio_mu);
    if (ctx->handle != handle || ctx->host_pid != getpid()) {
      pthread_mutex_unlock(&g_lx_aio_mu);
      return -LX_EINVAL;
    }
    lx_aio_sync_ring_head_locked(ctx);
    if (wait_err == ETIMEDOUT) break;
    if (wait_err) {
      pthread_mutex_unlock(&g_lx_aio_mu);
      return -LX_EINTR;
    }
  }
  unsigned count = ctx->completion_count < (unsigned)nr
      ? ctx->completion_count : (unsigned)nr;
  if (!count) { pthread_mutex_unlock(&g_lx_aio_mu); return 0; }
  struct lx_aio_event* out = malloc((size_t)count * sizeof *out);
  if (!out) { pthread_mutex_unlock(&g_lx_aio_mu); return -LX_ENOMEM; }
  for (unsigned i = 0; i < count; ++i)
    out[i] = ctx->completions[(ctx->completion_head + i) % ctx->requested_events];
  if (!events_addr || gmem_guest_copyout_checked(events_addr, out,
                                                (size_t)count * sizeof *out) != 0) {
    free(out);
    pthread_mutex_unlock(&g_lx_aio_mu);
    return -LX_EFAULT;
  }
  free(out);
  ctx->completion_head = (ctx->completion_head + count) % ctx->requested_events;
  ctx->completion_count -= count;
  ctx->ring_head = (ctx->ring_head + count) % (ctx->requested_events + 1);
  uint32_t head = ctx->ring_head;
  (void)gmem_guest_copyout_checked(
      handle + offsetof(struct lx_aio_ring_header, head), &head, sizeof head);
  pthread_mutex_unlock(&g_lx_aio_mu);
  return count;
}

// Linux ARM64 io_pgetevents adds an optional __aio_sigset argument to
// io_getevents. The guest currently has no per-thread temporary signal-mask
// handoff for this blocking path. The no-mask form is exactly io_getevents;
// reject an actual mask rather than silently ignoring it.
static long lx_io_pgetevents(uint64_t handle, long min_nr, long nr,
                             uint64_t events_addr, uint64_t timeout_addr,
                             uint64_t usig_addr) {
  struct { uint64_t sigmask, sigsetsize; } usig;
  if (usig_addr) {
    if (gmem_guest_export_copyin(NULL, usig_addr, &usig, sizeof usig) != 0)
      return -LX_EFAULT;
    if (usig.sigmask) {
      // ARM64 Linux _NSIG is 64: kernel sigset_t is one 64-bit word.
      if (usig.sigsetsize != sizeof(uint64_t)) return -LX_EINVAL;
      uint64_t ignored_mask;
      if (gmem_guest_export_copyin(NULL, usig.sigmask, &ignored_mask,
                                   sizeof ignored_mask) != 0)
        return -LX_EFAULT;
      return -LX_EOPNOTSUPP;
    }
  }
  return lx_io_getevents(handle, min_nr, nr, events_addr, timeout_addr);
}

#define LX_AT_FDCWD              (-100)
#define LX_AT_SYMLINK_NOFOLLOW   0x100
#define LX_AT_EACCESS            0x200
#define LX_AT_REMOVEDIR          0x200
#define LX_AT_SYMLINK_FOLLOW     0x400
#define LX_AT_HANDLE_FID         0x200
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

// Linux seccomp UAPI (aarch64 values). Only SET_MODE_FILTER is installed;
// SET_MODE_STRICT and later modes are rejected with EINVAL.
#define LX_SECCOMP_SET_MODE_STRICT 0
#define LX_SECCOMP_SET_MODE_FILTER 1
#define LX_SECCOMP_FILTER_FLAG_TSYNC (1u << 0)
#define LX_SECCOMP_MODE_FILTER       2

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
#define LX_FALLOC_ZERO_RANGE  0x10

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

// Is any page of [addr, addr+len) executable? Apple Silicon serves every
// executable mapping as MAP_JIT, and Darwin mlock() refuses MAP_JIT with EPERM,
// so mlockall(MCL_CURRENT) must skip executable guest spans (text/JIT) rather
// than fail the whole call. Data, stack and heap are plain and remain lockable.
static int host_range_executable(unsigned long addr, size_t len) {
  unsigned long end = addr + len;
  if (end < addr) return 0;
  mach_vm_address_t a = (mach_vm_address_t)addr;
  while (a < (mach_vm_address_t)end) {
    mach_vm_address_t ra = a;
    mach_vm_size_t rs = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&info, &cnt, &obj);
    if (kr != KERN_SUCCESS || ra > a || rs == 0) return 0;
    if (info.protection & VM_PROT_EXECUTE) return 1;
    a = ra + rs;
  }
  return 0;
}

// Lock and unlock requests must name whole guest-owned pages. Linux rounds
// unaligned ranges; the bounded host operation must never touch Shimmy pages.
static long guest_lock_range(unsigned long addr, unsigned long len,
                             unsigned long *base_out, unsigned long *rounded_out) {
  const unsigned long page = 16384;                           // guest AT_PAGESZ
  if (!len) return 0;
  unsigned long base = addr & ~(page - 1);
  unsigned long offset = addr - base;
  if (len > ULONG_MAX - offset) return -LX_EINVAL;
  unsigned long span = len + offset;
  if (span > ULONG_MAX - (page - 1)) return -LX_EINVAL;
  unsigned long rounded = (span + page - 1) & ~(page - 1);
  if (base > ULONG_MAX - rounded) return -LX_EINVAL;
  if (rounded > 1024ul * 1024) return -LX_EOPNOTSUPP;
  if (base < FOUR_GB || gmem_guest_range_mapped(base, (size_t)rounded) != 0 ||
      !host_range_mapped(base, (size_t)rounded)) return -LX_ENOMEM;
  *base_out = base;
  *rounded_out = rounded;
  return 0;
}

// Eager page locking shared by mlock and mlock2(flags=0).
static long guest_mlock_eager(unsigned long addr, unsigned long len) {
  if (!len) return 0;
  unsigned long base, rounded;
  long status = guest_lock_range(addr, len, &base, &rounded);
  if (status < 0) return status;
  return ok0(mlock((void*)base, (size_t)rounded));
}

// Linux `struct timeval` — tv_usec is 64-bit here, unlike macOS.
struct lx_timeval { long long tv_sec; long long tv_usec; };
// Native AArch64 Linux layout from linux/timex.h. Darwin's struct timex has
// neither the embedded time nor all of the trailing fields, so it cannot be
// copied to guest memory directly.
struct lx_timex_clock {
  uint32_t modes, pad0;
  int64_t offset, freq, maxerror, esterror;
  int32_t status, pad1;
  int64_t constant, precision, tolerance;
  int64_t time_sec, time_usec, tick;
  int64_t ppsfreq, jitter;
  int32_t shift, pad2;
  int64_t stabil, jitcnt, calcnt, errcnt, stbcnt;
  int32_t tai;
  uint32_t reserved[11];
};
_Static_assert(sizeof(struct lx_timex_clock) == 208, "Linux AArch64 timex ABI");
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

// Same as resolve_at_nf, but also returns the resolved GUEST-absolute path so a
// Landlock enforcement check can run against the exact path the syscall touches.
static long resolve_at_nf_g(int lx_dirfd, const char* gp, char* out, size_t outsz,
                            int* hdirfd, int nf, char* gabs, size_t gcap) {
  if (!gp) return -LX_EFAULT;
  long r = syscalls_resolve(lx_dirfd, gp, nf, out, outsz, gabs, gcap);
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

static long do_seccomp_set_mode_filter(unsigned long prog_addr);

static long do_prctl(int opt, unsigned long a1, unsigned long a2,
                     unsigned long a3, unsigned long a4) {
  switch (opt) {
    case LX_PR_SET_NAME: {
      if (!a1) return -LX_EFAULT;
      char next[sizeof g_procname] = {0};
      // Linux truncates at 15 bytes without requiring a NUL in guest memory.
      // Finish checked copy-in before changing the visible process name.
      for (size_t i = 0; i < sizeof next - 1; ++i) {
        if (gmem_guest_export_copyin(NULL, a1 + i, next + i, 1) != 0)
          return -LX_EFAULT;
        if (!next[i]) break;
      }
      memcpy(g_procname, next, sizeof g_procname);
      pthread_setname_np(g_procname);            // macOS can only name the calling thread
      return 0;
    }
    case LX_PR_GET_NAME: {
      if (!a1) return -LX_EFAULT;
      char out[sizeof g_procname] = {0};
      memcpy(out, g_procname, strnlen(g_procname, sizeof out - 1));
      return gmem_guest_copyout_checked(a1, out, sizeof out) == 0 ? 0 : -LX_EFAULT;
    }
    case LX_PR_SET_PDEATHSIG:
      return syscalls_signal_set_pdeathsig((int)a1);
    case LX_PR_GET_PDEATHSIG:
      if (!a1) return -LX_EFAULT;
      {
        int sig = syscalls_signal_get_pdeathsig();
        return gmem_guest_copyout_checked(a1, &sig, sizeof sig) == 0 ? 0 : -LX_EFAULT;
      }
    case LX_PR_SET_DUMPABLE:   g_dumpable = (int)a1; return 0;
    case LX_PR_GET_DUMPABLE:   return g_dumpable;
    case LX_PR_SET_KEEPCAPS:   return 0;
    case LX_PR_GET_KEEPCAPS:   return 0;
    case LX_PR_SET_SECUREBITS: g_securebits = (int)a1; return 0;
    case LX_PR_GET_SECUREBITS: return g_securebits;
    case LX_PR_SET_NO_NEW_PRIVS:
      if (a1 != 1 || a2 || a3 || a4) return -LX_EINVAL;
      return guest_no_new_privs_set();
    case LX_PR_GET_NO_NEW_PRIVS:
      if (a1 || a2 || a3 || a4) return -LX_EINVAL;
      return guest_no_new_privs_get();
    case LX_PR_CAPBSET_READ:   return 0;
    case LX_PR_CAPBSET_DROP:   return 0; // CapBnd is already empty; dropping remains idempotent.
    case LX_PR_SET_TIMERSLACK: g_timerslack = a1 ? a1 : g_timerslack; return 0;
    case LX_PR_GET_TIMERSLACK: return (long)g_timerslack;
    case LX_PR_SET_VMA:        return 0;         // anon-vma naming: purely cosmetic
    case LX_PR_SET_PTRACER:    return 0;
    case LX_PR_GET_SECCOMP:    return 0;
    case LX_PR_SET_SECCOMP:
      // prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog, 0, 0). Only the
      // filter mode is installed; STRICT and anything else are EINVAL.
      if (a1 != LX_SECCOMP_MODE_FILTER) return -LX_EINVAL;
      if (a3 || a4) return -LX_EINVAL;
      return do_seccomp_set_mode_filter(a2);
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
// seccomp(2) install — SET_MODE_FILTER. The guest passes a `struct sock_fprog`
// (len + pointer to len `struct sock_filter` entries). We copy both in with the
// standard checked guest-memory helper, bound the length, then hand the snapshot
// to guest_seccomp_install() (which verifies and attaches it).
// ---------------------------------------------------------------------------
struct lx_sock_fprog {
  unsigned short len;
  unsigned short _pad0;
  unsigned int   _pad1;
  uint64_t       filter;   /* guest pointer to len sock_filter entries */
};
_Static_assert(sizeof(struct lx_sock_fprog) == 16, "Linux aarch64 sock_fprog size");

static long do_seccomp_set_mode_filter(unsigned long prog_addr) {
  if (!prog_addr) return -LX_EFAULT;
  struct lx_sock_fprog fp;
  if (gmem_guest_export_copyin(NULL, prog_addr, &fp, sizeof fp) != 0)
    return -LX_EFAULT;
  unsigned len = fp.len;
  if (len == 0 || len > SHIM_SECCOMP_MAX_INSNS) return -LX_EINVAL;
  struct shim_sock_filter insns[SHIM_SECCOMP_MAX_INSNS];
  if (gmem_guest_export_copyin(NULL, fp.filter, insns,
                               (size_t)len * sizeof insns[0]) != 0)
    return -LX_EFAULT;
  return guest_seccomp_install(insns, len);
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

static _Thread_local struct lx_stack g_altstack = { 0, LX_SS_DISABLE, 0 };

static long do_sigaltstack(unsigned long ss_addr, unsigned long oss_addr) {
  struct lx_stack next;
  struct lx_stack prev = g_altstack;
  if (ss_addr) {
    if (gmem_guest_export_copyin(NULL, ss_addr, &next, sizeof next) != 0)
      return -LX_EFAULT;
    if (next.ss_flags & ~(LX_SS_DISABLE | LX_SS_ONSTACK)) return -LX_EINVAL;
    if (next.ss_flags & LX_SS_DISABLE) {
      next.ss_sp = 0; next.ss_size = 0; next.ss_flags = LX_SS_DISABLE;
    } else {
      if (next.ss_size < LX_MINSIGSTKSZ) return -LX_ENOMEM;
      next.ss_flags = 0;
    }
  }
  if (oss_addr && gmem_guest_copyout_checked(oss_addr, &prev, sizeof prev) != 0)
    return -LX_EFAULT;
  if (ss_addr) g_altstack = next;
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
  if (oldfd == newfd) return -LX_EINVAL;         // this is the whole point of dup3 vs dup2
  if (flags & ~LX_O_CLOEXEC) return -LX_EINVAL;
  if (fcntl(oldfd, F_GETFD) < 0) return eret();  // validate before clobbering newfd
  int allowed = inotify_watch_can_dup(oldfd, newfd);
  if (allowed < 0) return allowed;
  if (dup2(oldfd, newfd) < 0) return eret();
  // Device identity belongs to the duplicated descriptor, not to a failed
  // attempt. Register only after dup2 has actually replaced the target fd.
  if (drm_owns_fd(oldfd)) drm_register_fd_copy(newfd, oldfd);
  if (snd_owns_fd(oldfd)) snd_dup_fd(oldfd, newfd);
  if (evdev_owns_fd(oldfd)) evdev_register_fd(newfd, evdev_device_of_fd(oldfd));
  inotify_watch_dup(oldfd, newfd);
  rd_rtnl_dup(oldfd, newfd);
  if (pidfd_shim_pid(oldfd) > 0) pidfd_shim_register(newfd, pidfd_shim_pid(oldfd));
  if (flags & LX_O_CLOEXEC) fcntl(newfd, F_SETFD, fcntl(newfd, F_GETFD) | FD_CLOEXEC);
  return newfd;
}

static long do_fallocate(int fd, int mode, long long off, long long len) {
  if (len <= 0 || off < 0) return -LX_EINVAL;
  if ((uint64_t)off + (uint64_t)len > INT64_MAX) return -LX_EOVERFLOW;
  // Linux permits hole punching only with KEEP_SIZE, and no other mode bits.
  // Validate before Darwin F_PUNCHHOLE can mutate the host file.
  if ((mode & LX_FALLOC_PUNCH_HOLE) &&
      mode != (LX_FALLOC_PUNCH_HOLE | LX_FALLOC_KEEP_SIZE))
    return -LX_EOPNOTSUPP;
  if (mode & LX_FALLOC_ZERO_RANGE) {
    // Darwin has no unwritten-extent equivalent. For a small range wholly
    // inside a regular file, positioned zero writes reproduce the observable
    // bytes without changing the file size or current seek position.
    if (mode & ~(LX_FALLOC_ZERO_RANGE | LX_FALLOC_KEEP_SIZE)) return -LX_EOPNOTSUPP;
    if (fd < 0 || shim_fd_reserved(fd)) return -LX_EBADF;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) return eret();
    if ((fl & O_ACCMODE) == O_RDONLY) return -LX_EBADF;
    if (fl & O_APPEND) return -LX_EOPNOTSUPP;
    struct stat st;
    if (fstat(fd, &st) < 0) return eret();
    if (!S_ISREG(st.st_mode) || len > 1024 * 1024 || off + len > st.st_size)
      return -LX_EOPNOTSUPP;
    static const char zeros[4096] = {0};
    long long done = 0;
    while (done < len) {
      size_t count = (size_t)((len - done) < (long long)sizeof zeros ?
                              (len - done) : (long long)sizeof zeros);
      ssize_t n = pwrite(fd, zeros, count, (off_t)(off + done));
      if (n < 0) { if (errno == EINTR) continue; return eret(); }
      if (!n) return -LX_EIO;
      done += n;
    }
    return 0;
  }
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

// The guest and shim share a host address space. A raw memcpy here would let
// the guest write arbitrary shim memory. This self-process subset stages and
// validates every local vector before making checked guest-memory writes.
static long lx_process_vm_writev_self(unsigned long pid_arg, unsigned long local_arg,
                                     unsigned long local_count, unsigned long remote_arg,
                                     unsigned long remote_count, unsigned long flags_arg) {
  if (flags_arg) return -LX_EINVAL;
  long pid = (long)(int)pid_arg;
  if (pid <= 0) return -LX_ESRCH;
  if (pid != syscalls_guest_pid())
    return guest_child_known((int)pid) ? -LX_EOPNOTSUPP : -LX_ESRCH;
  if (local_count > 1024 || remote_count > 1024) return -LX_EINVAL;
  if (local_count > 16 || remote_count > 16) return -LX_EOPNOTSUPP;
  if (!local_count || !remote_count) return 0;
  if (!local_arg || !remote_arg) return -LX_EFAULT;
  struct iovec local[16], remote[16];
  if (gmem_guest_export_copyin(NULL, local_arg, local,
                               local_count * sizeof local[0]) != 0 ||
      gmem_guest_export_copyin(NULL, remote_arg, remote,
                               remote_count * sizeof remote[0]) != 0) return -LX_EFAULT;
  size_t local_bytes = 0, remote_bytes = 0;
  for (unsigned long i = 0; i < local_count; ++i) {
    size_t len = local[i].iov_len;
    if (len > 1024ul * 1024 - local_bytes) return -LX_EOPNOTSUPP;
    if (len && (uintptr_t)local[i].iov_base > UINTPTR_MAX - len) return -LX_EFAULT;
    local_bytes += len;
  }
  for (unsigned long i = 0; i < remote_count; ++i) {
    size_t len = remote[i].iov_len;
    if (len > 1024ul * 1024 - remote_bytes) return -LX_EOPNOTSUPP;
    if (len && (uintptr_t)remote[i].iov_base > UINTPTR_MAX - len) return -LX_EFAULT;
    remote_bytes += len;
  }
  // Snapshotting all local bytes differs from Linux's sequential operation
  // if source and destination overlap. Refuse that case rather than claim
  // an unverified copy ordering.
  for (unsigned long i = 0; i < local_count; ++i)
    for (unsigned long j = 0; j < remote_count; ++j) {
      uintptr_t l = (uintptr_t)local[i].iov_base, r = (uintptr_t)remote[j].iov_base;
      if (local[i].iov_len && remote[j].iov_len &&
          l < r + remote[j].iov_len && r < l + local[i].iov_len)
        return -LX_EOPNOTSUPP;
    }
  char *staged = malloc(local_bytes ? local_bytes : 1);
  if (!staged) return -LX_ENOMEM;
  size_t at = 0;
  for (unsigned long i = 0; i < local_count; ++i) {
    size_t len = local[i].iov_len;
    if (len && gmem_guest_export_copyin(NULL, (uintptr_t)local[i].iov_base,
                                        staged + at, len) != 0) {
      free(staged);
      return -LX_EFAULT;
    }
    at += len;
  }
  size_t copied = 0, limit = local_bytes < remote_bytes ? local_bytes : remote_bytes;
  for (unsigned long i = 0; i < remote_count && copied < limit; ++i) {
    uintptr_t addr = (uintptr_t)remote[i].iov_base;
    size_t remaining = remote[i].iov_len;
    if (remaining > limit - copied) remaining = limit - copied;
    while (remaining) {
      size_t step = 16384 - (addr & 16383); // remote fault granularity
      if (step > remaining) step = remaining;
      if (gmem_guest_copyout_checked(addr, staged + copied, step) != 0) {
        free(staged);
        return copied ? (long)copied : -LX_EFAULT;
      }
      addr += step;
      copied += step;
      remaining -= step;
    }
  }
  free(staged);
  return (long)copied;
}

// The remote source is still a guest-owned mapping, not an arbitrary pointer
// in the shared host address space. Validate all local destinations before
// transfer, then read remote pages only through the checked guest ledger.
static long lx_process_vm_readv_self(unsigned long pid_arg, unsigned long local_arg,
                                    unsigned long local_count, unsigned long remote_arg,
                                    unsigned long remote_count, unsigned long flags_arg) {
  if (flags_arg) return -LX_EINVAL;
  long pid = (long)(int)pid_arg;
  if (pid <= 0) return -LX_ESRCH;
  if (pid != syscalls_guest_pid())
    return guest_child_known((int)pid) ? -LX_EOPNOTSUPP : -LX_ESRCH;
  if (local_count > 1024 || remote_count > 1024) return -LX_EINVAL;
  if (local_count > 16 || remote_count > 16) return -LX_EOPNOTSUPP;
  if (!local_count || !remote_count) return 0;
  if (!local_arg || !remote_arg) return -LX_EFAULT;
  struct iovec local[16], remote[16];
  if (gmem_guest_export_copyin(NULL, local_arg, local,
                               local_count * sizeof local[0]) != 0 ||
      gmem_guest_export_copyin(NULL, remote_arg, remote,
                               remote_count * sizeof remote[0]) != 0) return -LX_EFAULT;
  size_t local_bytes = 0, remote_bytes = 0;
  for (unsigned long i = 0; i < local_count; ++i) {
    size_t len = local[i].iov_len;
    if (len > 1024ul * 1024 - local_bytes) return -LX_EOPNOTSUPP;
    if (len && (uintptr_t)local[i].iov_base > UINTPTR_MAX - len) return -LX_EFAULT;
    local_bytes += len;
  }
  for (unsigned long i = 0; i < remote_count; ++i) {
    size_t len = remote[i].iov_len;
    if (len > 1024ul * 1024 - remote_bytes) return -LX_EOPNOTSUPP;
    if (len && (uintptr_t)remote[i].iov_base > UINTPTR_MAX - len) return -LX_EFAULT;
    remote_bytes += len;
  }
  for (unsigned long i = 0; i < local_count; ++i)
    for (unsigned long j = 0; j < remote_count; ++j) {
      uintptr_t l = (uintptr_t)local[i].iov_base, r = (uintptr_t)remote[j].iov_base;
      if (local[i].iov_len && remote[j].iov_len &&
          l < r + remote[j].iov_len && r < l + local[i].iov_len)
        return -LX_EOPNOTSUPP;
    }
  size_t scratch_size = local_bytes > 16384 ? local_bytes : 16384;
  char *scratch = malloc(scratch_size);
  if (!scratch) return -LX_ENOMEM;
  // Local buffers are checked before any remote transfer, matching Linux's
  // preflight rule. Writing their existing bytes tests write permission.
  for (unsigned long i = 0; i < local_count; ++i) {
    size_t len = local[i].iov_len;
    uintptr_t addr = (uintptr_t)local[i].iov_base;
    if (len && (gmem_guest_export_copyin(NULL, addr, scratch, len) != 0 ||
                gmem_guest_copyout_checked(addr, scratch, len) != 0)) {
      free(scratch);
      return -LX_EFAULT;
    }
  }
  size_t copied = 0, limit = local_bytes < remote_bytes ? local_bytes : remote_bytes;
  unsigned long li = 0;
  size_t local_off = 0;
  for (unsigned long i = 0; i < remote_count && copied < limit; ++i) {
    uintptr_t addr = (uintptr_t)remote[i].iov_base;
    size_t remaining = remote[i].iov_len;
    if (remaining > limit - copied) remaining = limit - copied;
    while (remaining) {
      size_t step = 16384 - (addr & 16383); // remote fault granularity
      if (step > remaining) step = remaining;
      if (gmem_guest_export_copyin(NULL, addr, scratch, step) != 0) {
        free(scratch);
        return copied ? (long)copied : -LX_EFAULT;
      }
      size_t consumed = 0;
      while (consumed < step) {
        while (li < local_count && local_off == local[li].iov_len) {
          ++li;
          local_off = 0;
        }
        if (li >= local_count) break;
        size_t n = local[li].iov_len - local_off;
        if (n > step - consumed) n = step - consumed;
        uintptr_t dest = (uintptr_t)local[li].iov_base + local_off;
        if (gmem_guest_copyout_checked(dest, scratch + consumed, n) != 0) {
          free(scratch);
          return copied ? (long)copied : -LX_EFAULT;
        }
        local_off += n;
        consumed += n;
        copied += n;
      }
      addr += consumed;
      remaining -= consumed;
    }
  }
  free(scratch);
  return (long)copied;
}

// ---------------------------------------------------------------------------
// New mount API fd -> fstype registry.
//
// systemd/libmount mount /tmp via the new mount API: fsopen("tmpfs") -> fsconfig
// (options) -> fsconfig(FSCONFIG_CMD_CREATE) -> fsmount() -> move_mount(mfd, "",
// AT_FDCWD, "/tmp", MOVE_MOUNT_F_EMPTY_PATH). move_mount() only ever receives the
// fsmount() RESULT fd, so the fstype captured at fsopen() must be carried across the
// fsopen->fsmount handoff onto the new fd. fspick() reopens an existing tree without
// an fstype argument, so it registers no fstype (conservative: move_mount() keeps its
// existing no-op success). Entries are dropped on close(2).
// ---------------------------------------------------------------------------
#define MOUNTAPI_FSTYPE_MAX 32
#define MOUNTAPI_FD_MAX 128
static struct { int fd; char fstype[MOUNTAPI_FSTYPE_MAX]; } g_mountapi[MOUNTAPI_FD_MAX];
static int             g_mountapi_n;
static pthread_mutex_t g_mountapi_mu = PTHREAD_MUTEX_INITIALIZER;

static void mountapi_fd_set(int fd, const char* fstype) {
  if (fd < 0 || !fstype || !*fstype) return;
  size_t n = strlen(fstype);
  if (n >= MOUNTAPI_FSTYPE_MAX) n = MOUNTAPI_FSTYPE_MAX - 1;
  pthread_mutex_lock(&g_mountapi_mu);
  for (int i = 0; i < g_mountapi_n; i++)
    if (g_mountapi[i].fd == fd) {
      memcpy(g_mountapi[i].fstype, fstype, n);
      g_mountapi[i].fstype[n] = 0;
      pthread_mutex_unlock(&g_mountapi_mu);
      return;
    }
  if (g_mountapi_n < MOUNTAPI_FD_MAX) {
    g_mountapi[g_mountapi_n].fd = fd;
    memcpy(g_mountapi[g_mountapi_n].fstype, fstype, n);
    g_mountapi[g_mountapi_n].fstype[n] = 0;
    g_mountapi_n++;
  }
  pthread_mutex_unlock(&g_mountapi_mu);
}

// Returns 0 and fills `out` when fd is registered, -1 otherwise.
static int mountapi_fd_get(int fd, char* out, size_t cap) {
  if (fd < 0 || !out || !cap) return -1;
  pthread_mutex_lock(&g_mountapi_mu);
  int found = -1;
  for (int i = 0; i < g_mountapi_n; i++)
    if (g_mountapi[i].fd == fd) { found = i; break; }
  if (found >= 0) {
    snprintf(out, cap, "%s", g_mountapi[found].fstype);
  }
  pthread_mutex_unlock(&g_mountapi_mu);
  return found >= 0 ? 0 : -1;
}

void mountapi_fd_closed(int fd) {
  if (fd < 0) return;
  pthread_mutex_lock(&g_mountapi_mu);
  for (int i = 0; i < g_mountapi_n; i++)
    if (g_mountapi[i].fd == fd) { g_mountapi[i] = g_mountapi[--g_mountapi_n]; break; }
  pthread_mutex_unlock(&g_mountapi_mu);
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
  case 0: return lx_io_setup((unsigned)a0, a1);                 // io_setup; family gate pending
  case 1: return lx_io_destroy(a0);                            // io_destroy; family gate pending
  case 2: return lx_io_submit(a0, (long)a1, a2);               // io_submit; family gate pending
  case 3: return lx_io_cancel(a0, a1, a2);                      // io_cancel; inline-only AIO
  case 4: return lx_io_getevents(a0, (long)a1, (long)a2, a3, a4);
  case 5: case 6: case 7: {                                    // setxattr / lsetxattr / fsetxattr
    if (nr == 7) {
      // Snapshot all guest input before mutating the descriptor's xattr.
      if (a4 & ~3ul || a4 == 3) return -LX_EINVAL;
      if (a3 > 65536) return -LX_E2BIG; // Linux XATTR_SIZE_MAX
      char name[256]; // Linux XATTR_NAME_MAX plus NUL
      e = copy_guest_cstr(a1, name, sizeof name);
      if (e < 0) return e;
      size_t size = (size_t)a3;
      char *value = malloc(size ? size : 1);
      if (!value) return -LX_ENOMEM;
      if (size && gmem_guest_export_copyin(NULL, a2, value, size) != 0) {
        free(value);
        return -LX_EFAULT;
      }
      int opt = 0;
      if (a4 & 1) opt |= XATTR_CREATE;
      if (a4 & 2) opt |= XATTR_REPLACE;
      int r = fsetxattr((int)a0, name, value, size, 0, opt);
      int saved_errno = errno;
      free(value);
      errno = saved_errno;
      return ok0(r);
    }
    if (nr == 6) {
      // Snapshot the complete value before mutating the link inode. A bad
      // later guest page must not leave a partial host xattr update.
      if (a4 & ~3ul || a4 == 3) return -LX_EINVAL;
      if (a3 > 65536) return -LX_E2BIG; // Linux XATTR_SIZE_MAX
      char guest_path[PATH_MAX], name[256];
      e = copy_guest_cstr(a0, guest_path, sizeof guest_path);
      if (e < 0) return e;
      e = copy_guest_cstr(a1, name, sizeof name);
      if (e < 0) return e;
      e = resolve_path_nf(guest_path, host, sizeof host, 1);
      if (e < 0) return e;
      size_t size = (size_t)a3;
      char *value = malloc(size ? size : 1);
      if (!value) return -LX_ENOMEM;
      if (size && gmem_guest_export_copyin(NULL, a2, value, size) != 0) {
        free(value);
        return -LX_EFAULT;
      }
      int opt = XATTR_NOFOLLOW;
      if (a4 & 1) opt |= XATTR_CREATE;
      if (a4 & 2) opt |= XATTR_REPLACE;
      int r = setxattr(host, name, value, size, 0, opt);
      int saved_errno = errno;
      free(value);
      errno = saved_errno;
      return ok0(r);
    }
    int hopt = 0;                                              // Linux XATTR_CREATE=1 REPLACE=2
    if ((int)a4 & 1) hopt |= XATTR_CREATE;
    if ((int)a4 & 2) hopt |= XATTR_REPLACE;
    if (nr == 5) {
      // The path, name and value all belong to guest memory. Snapshot them
      // before resolving the path or changing any host xattr.
      char guest_path[PATH_MAX], name[256]; // Linux XATTR_NAME_MAX + NUL
      e = copy_guest_cstr(a0, guest_path, sizeof guest_path);
      if (e < 0) return e;
      e = copy_guest_cstr(a1, name, sizeof name);
      if (e < 0) return e;
      if (a3 > 65536) return -LX_E2BIG; // Linux XATTR_SIZE_MAX
      size_t size = (size_t)a3;
      char *value = size ? malloc(size) : NULL;
      if (size && !value) return -LX_ENOMEM;
      if (size && gmem_guest_export_copyin(NULL, a2, value, size) != 0) {
        free(value);
        return -LX_EFAULT;
      }
      e = resolve_path_nf(guest_path, host, sizeof host, 0);
      if (e < 0) { free(value); return e; }
      int rc = setxattr(host, name, value, size, 0, hopt);
      int saved_errno = errno;
      free(value);
      errno = saved_errno;
      return ok0(rc);
    }
    if (nr == 6) hopt |= XATTR_NOFOLLOW;
    e = resolve_path_nf((const char*)a0, host, sizeof host, nr == 6);
    if (e < 0) return e;
    return ok0(setxattr(host, (const char*)a1, (const void*)a2, (size_t)a3, 0, hopt));
  }
  case 8: case 9: case 10: {                                   // getxattr / lgetxattr / fgetxattr
    if (nr == 10) {
      // The xattr name and value destination are guest addresses. Snapshot
      // the bounded name and stage host output before checked guest copyout.
      char name[256]; // Linux XATTR_NAME_MAX plus NUL
      e = copy_guest_cstr(a1, name, sizeof name);
      if (e < 0) return e;
      size_t requested = (size_t)a3;
      size_t cap = requested > 1024 * 1024 ? 1024 * 1024 : requested;
      char *value = cap ? malloc(cap) : NULL;
      if (cap && !value) return -LX_ENOMEM;
      ssize_t r = fgetxattr((int)a0, name, value, cap, 0, 0);
      // This is a read-only query, so only its actual result needs guest
      // space. A large advertised capacity may still hold a small xattr.
      if (requested && r > 0 && gmem_guest_copyout_checked(a2, value, (size_t)r) != 0) {
        free(value);
        return -LX_EFAULT;
      }
      int saved_errno = errno;
      free(value);
      errno = saved_errno;
      return r < 0 ? eret() : r;
    }
    if (nr == 8) {
      char guest_path[PATH_MAX], name[256];
      e = copy_guest_cstr(a0, guest_path, sizeof guest_path);
      if (e < 0) return e;
      e = copy_guest_cstr(a1, name, sizeof name);
      if (e < 0) return e;
      e = resolve_path_nf(guest_path, host, sizeof host, 0);
      if (e < 0) return e;
      size_t requested = (size_t)a3;
      size_t cap = requested > 1024 * 1024 ? 1024 * 1024 : requested;
      char *value = cap ? malloc(cap) : NULL;
      if (cap && !value) return -LX_ENOMEM;
      ssize_t r = getxattr(host, name, value, cap, 0, 0);
      if (requested && r > 0 && gmem_guest_copyout_checked(a2, value, (size_t)r) != 0) {
        free(value);
        return -LX_EFAULT;
      }
      int saved_errno = errno;
      free(value);
      errno = saved_errno;
      return r < 0 ? eret() : r;
    }
    if (nr == 9) {
      char guest_path[PATH_MAX], name[256];
      e = copy_guest_cstr(a0, guest_path, sizeof guest_path);
      if (e < 0) return e;
      e = copy_guest_cstr(a1, name, sizeof name);
      if (e < 0) return e;
      e = resolve_path_nf(guest_path, host, sizeof host, 1);
      if (e < 0) return e;
      size_t requested = (size_t)a3;
      size_t cap = requested > 1024 * 1024 ? 1024 * 1024 : requested;
      char *value = cap ? malloc(cap) : NULL;
      if (cap && !value) return -LX_ENOMEM;
      ssize_t r = getxattr(host, name, value, cap, 0, XATTR_NOFOLLOW);
      if (requested && r > 0 && gmem_guest_copyout_checked(a2, value, (size_t)r) != 0) {
        free(value);
        return -LX_EFAULT;
      }
      int saved_errno = errno;
      free(value);
      errno = saved_errno;
      return r < 0 ? eret() : r;
    }
    ssize_t r;
    e = resolve_path_nf((const char*)a0, host, sizeof host, nr == 9);
    if (e < 0) return e;
    r = getxattr(host, (const char*)a1, (void*)a2, (size_t)a3, 0,
                 nr == 9 ? XATTR_NOFOLLOW : 0);
    return r < 0 ? eret() : r;
  }
  case 11: case 12: case 13: {                                 // listxattr / l / f
    if (nr == 11 || nr == 12) {
      char guest_path[PATH_MAX];
      e = copy_guest_cstr(a0, guest_path, sizeof guest_path);
      if (e < 0) return e;
      e = resolve_path_nf(guest_path, host, sizeof host, nr == 12);
      if (e < 0) return e;
      size_t requested = (size_t)a2;
      size_t cap = requested > 1024 * 1024 ? 1024 * 1024 : requested;
      char *names = cap ? malloc(cap) : NULL;
      if (cap && !names) return -LX_ENOMEM;
      ssize_t r = listxattr(host, names, cap, nr == 12 ? XATTR_NOFOLLOW : 0);
      if (requested && r > 0 && gmem_guest_copyout_checked(a1, names, (size_t)r) != 0) {
        free(names);
        return -LX_EFAULT;
      }
      int saved_errno = errno;
      free(names);
      errno = saved_errno;
      return r < 0 ? eret() : r;
    }
    if (nr == 13) {
      // The list is host-generated output, not a host-writable guest pointer.
      // Linux permits size=0 to query its length without an output buffer.
      size_t requested = (size_t)a2;
      size_t cap = requested > 1024 * 1024 ? 1024 * 1024 : requested;
      char *names = cap ? malloc(cap) : NULL;
      if (cap && !names) return -LX_ENOMEM;
      ssize_t r = flistxattr((int)a0, names, cap, 0);
      if (requested && r > 0 && gmem_guest_copyout_checked(a1, names, (size_t)r) != 0) {
        free(names);
        return -LX_EFAULT;
      }
      int saved_errno = errno;
      free(names);
      errno = saved_errno;
      return r < 0 ? eret() : r;
    }
    return -LX_ENOSYS;  // No other syscall number enters this case.
  }
  case 14: case 15: case 16: {                                 // removexattr / l / f
    if (nr == 14) {
      // Both arguments are guest strings. Resolve and mutate only after
      // checked, bounded copies of the pathname and attribute name.
      char guest_path[PATH_MAX], name[256];
      e = copy_guest_cstr(a0, guest_path, sizeof guest_path);
      if (e < 0) return e;
      e = copy_guest_cstr(a1, name, sizeof name);
      if (e < 0) return e;
      e = resolve_path_nf(guest_path, host, sizeof host, 0);
      if (e < 0) return e;
      return ok0(removexattr(host, name, 0));
    }
    if (nr == 16) {
      char name[256]; // Linux XATTR_NAME_MAX plus NUL.
      e = copy_guest_cstr(a1, name, sizeof name);
      if (e < 0) return e;
      return ok0(fremovexattr((int)a0, name, 0));
    }
    if (nr == 15) {
      // lremovexattr names the symlink itself. Snapshot both guest strings
      // before any host mutation so invalid addresses cannot crash the shim.
      char guest_path[PATH_MAX], name[256];
      e = copy_guest_cstr(a0, guest_path, sizeof guest_path);
      if (e < 0) return e;
      e = copy_guest_cstr(a1, name, sizeof name);
      if (e < 0) return e;
      e = resolve_path_nf(guest_path, host, sizeof host, 1);
      if (e < 0) return e;
      return ok0(removexattr(host, name, XATTR_NOFOLLOW));
    }
    e = resolve_path_nf((const char*)a0, host, sizeof host, nr == 15);
    if (e < 0) return e;
    return ok0(removexattr(host, (const char*)a1, nr == 15 ? XATTR_NOFOLLOW : 0));
  }

  // ===== 18..34: fds, inotify, nodes ========================================
  case 18: return -LX_ENOSYS;                                  // lookup_dcookie
  case 23: {                                                   // dup
    int allowed = inotify_watch_can_dup((int)a0, -1);
    if (allowed < 0) return allowed;
    int r = dup((int)a0);
    if (r >= 0) inotify_watch_dup((int)a0, r);
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
  // inotify: a bounded watched-file backend. glib's GIO registers its file-monitor backend by
  // calling inotify_init1() and giving up if it fails — and when no monitor backend exists it
  // reports "Unable to find default local file monitor type" and then NULL-derefs. So the fd has
  // to be real and pollable (a pipe read end). Watched-file writes and self deletion now
  // produce events; directory child-name events still need a separate implementation.
  case 26: {                                                    // inotify_init1(flags)
    if (a0 & ~(unsigned long)(LX_O_NONBLOCK_ | LX_O_CLOEXEC_)) return -LX_EINVAL;
    int fds[2];
    if (pipe(fds) != 0) return eret();
    // The write end is deliberately KEPT OPEN. Closing it would make the
    // read end instantly readable-at-EOF, and glib treats that as fatal: "inotify unexpectedly
    // hit eof". Holding it open makes the fd quiet until a watch fires. It must
    // ALSO be reserved: udevd's worker spawn (posix_spawn) does close_range(3,~0U) in the vfork'd
    // child, and an unreserved write end gets closed there, making udevd's inotify read end EOF ->
    // a spurious IN_* wake -> udevd reloads its rules dirs in a 100% CPU loop.
    fds[0] = shim_fd_reserve(fds[0]);   // read end: the vfork child's close_range() must skip it too
    fds[1] = shim_fd_reserve(fds[1]);
    int watch_kq = kqueue();
    if (watch_kq < 0) {
      long error = eret();
      shim_fd_unreserve(fds[0]); shim_fd_unreserve(fds[1]);
      close(fds[0]); close(fds[1]);
      return error;
    }
    watch_kq = shim_fd_reserve(watch_kq);
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
    if (inotify_watch_register(fds[0], fds[1], watch_kq,
                               shim_fd_reserve, shim_fd_unreserve) != 0) {
      shim_fd_unreserve(fds[0]); shim_fd_unreserve(fds[1]); shim_fd_unreserve(watch_kq);
      close(fds[0]); close(fds[1]); close(watch_kq);
      return -LX_ENOMEM;
    }
    if (a0 & LX_O_NONBLOCK_) fcntl(fds[0], F_SETFL, O_NONBLOCK);
    if (a0 & LX_O_CLOEXEC_)  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    return fds[0];
  }
  case 27: {                                                    // inotify_add_watch(fd, path, mask)
    char guest_path[PATH_MAX];
    e = copy_guest_cstr(a1, guest_path, sizeof guest_path);
    if (e < 0) return e;
    e = resolve_path_nf(guest_path, host, sizeof host,
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
    char guest_path[PATH_MAX], gabs[PATH_MAX];
    e = copy_guest_cstr(a1, guest_path, sizeof guest_path);
    if (e < 0) return e;
    e = resolve_at_nf_g((int)a0, guest_path, host, sizeof host, &hdirfd, 1,
                        gabs, sizeof gabs);
    if (e < 0) return e;
    // Landlock: mknodat creates a directory entry of the requested type.
    {
      uint64_t acc;
      switch (mode & S_IFMT) {
        case S_IFIFO: acc = LL_FS_MAKE_FIFO; break;
        case S_IFCHR: acc = LL_FS_MAKE_CHAR; break;
        case S_IFBLK: acc = LL_FS_MAKE_BLOCK; break;
        case S_IFSOCK: acc = LL_FS_MAKE_SOCK; break;
        case 0: case S_IFREG:
        default:       acc = LL_FS_MAKE_REG; break;
      }
      if ((e = landlock_check(gabs, acc)) < 0) return e;
    }
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
    // Snapshot both guest strings before touching the host filesystem. The
    // target is data for the symlink, so resolving it would change its meaning.
    char guest_target[PATH_MAX], guest_link[PATH_MAX], gabs[PATH_MAX];
    e = copy_guest_cstr(a0, guest_target, sizeof guest_target);
    if (e < 0) return e;
    e = copy_guest_cstr(a2, guest_link, sizeof guest_link);
    if (e < 0) return e;
    e = resolve_at_nf_g((int)a1, guest_link, host, sizeof host, &hdirfd, 1,
                        gabs, sizeof gabs);
    if (e < 0) return e;
    // Landlock: creating a symlink entry needs MAKE_SYM on the link path.
    if ((e = landlock_check(gabs, LL_FS_MAKE_SYM)) < 0) return e;
    return ok0(symlinkat(guest_target, hdirfd, host));
  }
  case 39: {                                                   // umount2
    /* mount(40) currently synthesizes API filesystems without a mount table.
     * Claiming an unmount succeeded would leave /proc, /dev, etc. visible
     * while callers believe they were detached. Refuse existing targets
     * until a real guest mount namespace and detach path exist. */
    unsigned flags = (unsigned)a1;
    if (flags & ~15u) return -LX_EINVAL; // FORCE, DETACH, EXPIRE, NOFOLLOW
    char guest_path[PATH_MAX];
    e = copy_guest_cstr(a0, guest_path, sizeof guest_path);
    if (e < 0) return e;
    if (!guest_path[0]) return -LX_ENOENT;
    e = resolve_path_nf(guest_path, host, sizeof host, (flags & 8u) != 0);
    if (e < 0) return e;
    struct stat st;
    if ((flags & 8u ? lstat(host, &st) : stat(host, &st)) != 0)
      return eret();
    return -LX_EPERM;
  }
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
    // Even the synthetic mount subset must not dereference raw guest
    // pointers or acknowledge an invalid target as a successful mount.
    char guest_target[PATH_MAX], guest_fstype[PATH_MAX];
    e = copy_guest_cstr(a1, guest_target, sizeof guest_target);
    if (e < 0) return e;
    const char *target = guest_target;
    const char *fstype = NULL;
    if (a2) {
      e = copy_guest_cstr(a2, guest_fstype, sizeof guest_fstype);
      if (e < 0) return e;
      fstype = guest_fstype;
    }
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
      // /tmp is a real host-backed directory, not a synthesized tmpfs. Claiming
      // success here makes systemd's tmp.mount believe the mount took effect,
      // then retry forever when it cannot observe it. Refuse honestly so the
      // unit fails once and /tmp stays a plain sticky directory.
      if (strcmp(target, "/tmp") == 0) return -LX_EPERM;
    }
    return 0;                                   // no-op: the synthetic tree already serves the mount
  }
  case 41: return -LX_EPERM;                                   // pivot_root
  case 42: return -LX_ENOSYS;                                  // nfsservctl
  case 45: {                                                   // truncate
    if ((int64_t)a1 < 0) return -LX_EINVAL;
    char guest_path[PATH_MAX], gabs[PATH_MAX];
    e = copy_guest_cstr(a0, guest_path, sizeof guest_path);
    if (e < 0) return e;
    e = syscalls_resolve(LX_AT_FDCWD, guest_path, 0, host, sizeof host,
                         gabs, sizeof gabs);
    if (e < 0) return e;
    // Landlock: resizing a file needs TRUNCATE on its path.
    if ((e = landlock_check(gabs, LL_FS_TRUNCATE)) < 0) return e;
    return ok0(truncate(host, (off_t)a1));
  }
  case 47: return do_fallocate((int)a0, (int)a1, (long long)a2, (long long)a3);
  case 51: return -LX_EPERM;                                   // chroot
  case 52: {                                                   // fchmod
    int fd = (int)a0;
    // A shim-owned descriptor is not a guest file, even if its host fd
    // number is addressable. Never change permissions on shim internals.
    if (fd < 0 || shim_fd_reserved(fd)) return -LX_EBADF;
    return ok0(fchmod(fd, (mode_t)a1));
  }
  case 53: {                                                   // fchmodat(dfd, path, mode)
    // The original Linux syscall has three arguments. x3 is unspecified here;
    // treating it as flags makes chmod fail at random (ldconfig hits EINVAL).
    char guest_path[PATH_MAX];
    e = copy_guest_cstr(a1, guest_path, sizeof guest_path);
    if (e < 0) return e;
    // A bound AF_UNIX socket lives in the relocated namespace, not the rootfs;
    // chmod on the guest path must hit the real socket file or gpg-agent's
    // post-bind fchmodat returns ENOENT and it aborts socket setup.
    char sock_host[PATH_MAX];
    if (guest_path[0] == '/' &&
        net_unix_socket_host_path(guest_path, sock_host, sizeof sock_host))
      return ok0(fchmodat(AT_FDCWD, sock_host, (mode_t)a2, 0));
    e = resolve_at((int)a0, guest_path, host, sizeof host, &hdirfd);
    if (e < 0) return e;
    return ok0(fchmodat(hdirfd, host, (mode_t)a2, 0));
  }
  case 54: {                                                   // fchownat
    // Snapshot a non-NULL guest pathname before either branch dereferences it
    // or passes it to the resolver. Preserve the existing NULL/empty-path case.
    char guest_path_buf[PATH_MAX];
    const char *guest_path = (const char*)a1;
    if (a1) {
      e = copy_guest_cstr(a1, guest_path_buf, sizeof guest_path_buf);
      if (e < 0) return e;
      guest_path = guest_path_buf;
    }
    if (g_uid0) {
      struct stat st;
      if ((int)a4 & ~(LX_AT_EMPTY_PATH | LX_AT_SYMLINK_NOFOLLOW)) return -LX_EINVAL;
      if ((a4 & LX_AT_EMPTY_PATH) && (!guest_path || !guest_path[0])) {
        if ((int)a0 == LX_AT_FDCWD) { if (stat(".", &st)<0) return eret(); }
        else if (fstat((int)a0, &st)<0) return eret();
      } else {
        e=resolve_at_nf((int)a0,guest_path,host,sizeof host,&hdirfd,
                       ((int)a4 & LX_AT_SYMLINK_NOFOLLOW)!=0);
        if(e<0)return e;
        if(fstatat(hdirfd,host,&st,host_at_flags((int)a4))<0)return eret();
      }
      return ok0(guest_ownership_set(&st,(uint32_t)a2,(uint32_t)a3));
    }
    if ((a4 & LX_AT_EMPTY_PATH) && (!guest_path || !guest_path[0]))
      return ok0(fchown((int)a0, (uid_t)a2, (gid_t)a3));       // "chown the dirfd itself"
    e = resolve_at_nf((int)a0, guest_path, host, sizeof host, &hdirfd,
                      ((int)a4 & LX_AT_SYMLINK_NOFOLLOW) != 0);
    if (e < 0) return e;
    return ok0(fchownat(hdirfd, host, (uid_t)a2, (gid_t)a3, host_at_flags((int)a4)));
  }
  case 55: {                                                   // fchown
    int fd = (int)a0;
    // A shim-owned descriptor is not a guest file. Never attach virtual
    // ownership metadata to it or change its real Darwin ownership.
    if (fd < 0 || shim_fd_reserved(fd)) return -LX_EBADF;
    if (g_uid0) {
      struct stat st; if(fstat(fd,&st)<0)return eret();
      return ok0(guest_ownership_set(&st,(uint32_t)a1,(uint32_t)a2));
    }
    return ok0(fchown(fd,(uid_t)a1,(gid_t)a2));
  }
  case 58: return -LX_EPERM;                                   // vhangup
  case 59: return do_pipe2((int*)a0, (int)a1);                 // pipe2 (net.c normally owns this)
  case 60: return -LX_EPERM;                                   // quotactl

  // ===== 69..77: vectored / zero-copy IO ====================================
  case 69: {                                                   // preadv
    /* ARM64 passes the full signed offset in x3; x4 is redundant.  Read
     * into a bounded host buffer, then copy into admitted guest ranges. */
    int64_t pos = (int64_t)a3;
    if (pos < 0 || a2 > 1024) return -LX_EINVAL;
    int fd = (int)a0;
    if (fd < 0 || shim_fd_reserved(fd)) return -LX_EBADF;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || (fl & O_ACCMODE) == O_WRONLY) return -LX_EBADF;
    struct stat st;
    if (fstat(fd, &st) < 0) return eret();
    if (!S_ISREG(st.st_mode)) return -LX_ESPIPE;
    if (!a2) return 0;
    if (!a1) return -LX_EFAULT;
    struct iovec vec[1024];
    size_t nvec = (size_t)a2;
    if (gmem_guest_export_copyin(NULL, a1, vec, nvec * sizeof vec[0]) != 0)
      return -LX_EFAULT;
    size_t requested = 0;
    for (size_t i = 0; i < nvec; i++) {
      if (vec[i].iov_len > SSIZE_MAX - requested) return -LX_EINVAL;
      requested += vec[i].iov_len;
    }
    size_t count = requested > 1024 * 1024 ? 1024 * 1024 : requested;
    if (!count) return 0;
    // Reject a bad or read-only guest destination before host pread. The
    // existing 1 MiB bound can return a short regular-file read, so inspect
    // exactly that prefix rather than vectors beyond what this call copies.
    size_t admitted = 0;
    for (size_t i = 0; i < nvec && admitted < count; i++) {
      size_t part = vec[i].iov_len;
      if (part > count - admitted) part = count - admitted;
      if (part && gmem_guest_range_writable(
              (uint64_t)(uintptr_t)vec[i].iov_base, part) != 0)
        return -LX_EFAULT;
      admitted += part;
    }
    char *buf = malloc(count);
    if (!buf) return -LX_ENOMEM;
    ssize_t n = pread(fd, buf, count, (off_t)pos);
    if (n < 0) { long error = eret(); free(buf); return error; }
    size_t copied = 0;
    for (size_t i = 0; i < nvec && copied < (size_t)n; i++) {
      size_t part = vec[i].iov_len;
      if (part > (size_t)n - copied) part = (size_t)n - copied;
      if (part && gmem_guest_copyout_checked((uint64_t)(uintptr_t)vec[i].iov_base,
                                              buf + copied, part) != 0) {
        free(buf);
        return copied ? (long)copied : -LX_EFAULT;
      }
      copied += part;
    }
    free(buf);
    return (long)copied;
  }
  case 70: {                                                   // pwritev
    /* ARM64 carries the full signed offset in x3. Gather bounded, checked
     * guest vectors before letting the host write any bytes. */
    int64_t pos = (int64_t)a3;
    if (pos < 0 || a2 > 1024) return -LX_EINVAL;
    int fd = (int)a0;
    if (fd < 0 || shim_fd_reserved(fd)) return -LX_EBADF;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || (fl & O_ACCMODE) == O_RDONLY) return -LX_EBADF;
    if (fl & O_APPEND) return -LX_EOPNOTSUPP;
    struct stat st;
    if (fstat(fd, &st) < 0) return eret();
    if (!S_ISREG(st.st_mode)) return -LX_ESPIPE;
    if (!a2) return 0;
    if (!a1) return -LX_EFAULT;
    struct iovec vec[1024];
    size_t nvec = (size_t)a2;
    if (gmem_guest_export_copyin(NULL, a1, vec, nvec * sizeof vec[0]) != 0)
      return -LX_EFAULT;
    size_t requested = 0;
    for (size_t i = 0; i < nvec; i++) {
      if (vec[i].iov_len > SSIZE_MAX - requested) return -LX_EINVAL;
      requested += vec[i].iov_len;
    }
    size_t count = requested > 1024 * 1024 ? 1024 * 1024 : requested;
    if (!count) return 0;
    if (pos > INT64_MAX - (int64_t)count) return -LX_EINVAL;
    char *buf = malloc(count);
    if (!buf) return -LX_ENOMEM;
    size_t gathered = 0;
    for (size_t i = 0; i < nvec && gathered < count; i++) {
      size_t part = vec[i].iov_len;
      if (part > count - gathered) part = count - gathered;
      if (part && gmem_guest_export_copyin(NULL,
            (uint64_t)(uintptr_t)vec[i].iov_base, buf + gathered, part) != 0) {
        free(buf);
        return -LX_EFAULT;
      }
      gathered += part;
    }
    ssize_t r = pwrite(fd, buf, count, (off_t)pos);
    int saved_errno = errno;
    free(buf);
    errno = saved_errno;
    return r < 0 ? eret() : r;
  }
  case 71: {                                                   // sendfile(out, in, off*, count)
    int out_fd = (int)a0, in_fd = (int)a1;
    long long off = 0, initial = 0;
    if (a2 && gmem_guest_export_copyin(NULL, a2, &off, sizeof off) != 0)
      return -LX_EFAULT;
    // The offset is both input and output. Refuse a read-only guest cell
    // before transmitting bytes, which would otherwise return a late EFAULT.
    if (a2 && gmem_guest_range_writable(a2, sizeof off) != 0)
      return -LX_EFAULT;
    if (a2 && off < 0) return -LX_EINVAL;
    if (in_fd < 0 || out_fd < 0 || shim_fd_reserved(in_fd) ||
        shim_fd_reserved(out_fd)) return -LX_EBADF;
    int in_fl = fcntl(in_fd, F_GETFL), out_fl = fcntl(out_fd, F_GETFL);
    if (in_fl < 0 || out_fl < 0) return -LX_EBADF;
    if ((in_fl & O_ACCMODE) == O_WRONLY ||
        (out_fl & O_ACCMODE) == O_RDONLY) return -LX_EBADF;
    if (out_fl & O_APPEND) return -LX_EINVAL;
    struct stat st, out_st;
    if (fstat(in_fd, &st) < 0 || fstat(out_fd, &out_st) < 0) return eret();
    if (!S_ISREG(st.st_mode)) return -LX_EINVAL;
    if (S_ISREG(out_st.st_mode) && st.st_dev == out_st.st_dev &&
        st.st_ino == out_st.st_ino) return -LX_EINVAL;
    if (a2) initial = off;
    /* Linux may return a short transfer. Keep each host copy bounded. */
    size_t count = (size_t)a3;
    if (count > 1024 * 1024) count = 1024 * 1024;
    if (a2 && initial > INT64_MAX - (int64_t)count) return -LX_EINVAL;
    long r = bounce_copy(in_fd, a2 ? &off : NULL, out_fd, NULL, count, 0);
    if (a2) {
      /* bounce_copy updates its local offset only on its clean-exit path.
       * A short write followed by an error also advances by the bytes sent. */
      if (r > 0) off = initial + r;
      if (gmem_guest_copyout_checked(a2, &off, sizeof off) != 0)
        return -LX_EFAULT;
    }
    return r;
  }
  case 74: {                                                   // signalfd4
    // The backend consumes a host pointer; the ARM64 mask lives in guest
    // memory and must be staged before even its diagnostic dereference.
    if (!a1) return -LX_EFAULT;
    if (a2 != sizeof(uint64_t)) return -LX_EINVAL;
    uint64_t mask;
    if (gmem_guest_export_copyin(NULL, a1, &mask, sizeof mask) != 0)
      return -LX_EFAULT;
    return signalfd4_shim((int)a0, &mask, (size_t)a2, (int)a3);
  }
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
    struct iovec vec[1024];
    size_t nvec = (size_t)a2;
    if (gmem_guest_export_copyin(NULL, a1, vec, nvec * sizeof vec[0]) != 0)
      return -LX_EFAULT;
    size_t requested = 0;
    for (size_t i = 0; i < nvec; i++) {
      if (vec[i].iov_len > SSIZE_MAX - requested) return -LX_EINVAL;
      requested += vec[i].iov_len;
    }
    size_t count = requested > 1024 * 1024 ? 1024 * 1024 : requested;
    if (!count) return 0;
    char *buf = malloc(count);
    if (!buf) return -LX_ENOMEM;
    size_t gathered = 0;
    for (size_t i = 0; i < nvec && gathered < count; i++) {
      size_t part = vec[i].iov_len;
      if (part > count - gathered) part = count - gathered;
      if (part && gmem_guest_export_copyin(NULL,
            (uint64_t)(uintptr_t)vec[i].iov_base, buf + gathered, part) != 0) {
        free(buf);
        return -LX_EFAULT;
      }
      gathered += part;
    }
    ssize_t r = write((int)a0, buf, count);
    int saved_errno = errno;
    free(buf);
    errno = saved_errno;
    return r < 0 ? eret() : r;
  }
  case 76: {                                                   // splice
    if (!a4) return 0;  // Linux returns before validating fds/flags for len=0
    if ((unsigned)a5 & ~0xfu) return -LX_EINVAL;
    int in = (int)a0, out = (int)a2;
    if (in < 0 || out < 0 || shim_fd_reserved(in) || shim_fd_reserved(out))
      return -LX_EBADF;
    int infl = fcntl(in, F_GETFL), outfl = fcntl(out, F_GETFL);
    if (infl < 0 || outfl < 0 || (infl & O_ACCMODE) == O_WRONLY ||
        (outfl & O_ACCMODE) == O_RDONLY) return -LX_EBADF;
    if (outfl & O_APPEND) return -LX_EINVAL;
    struct stat inst, outst;
    if (fstat(in, &inst) < 0 || fstat(out, &outst) < 0) return eret();
    int inpipe = S_ISFIFO(inst.st_mode), outpipe = S_ISFIFO(outst.st_mode);
    if (!inpipe && !outpipe) return -LX_EINVAL;
    if ((inpipe && a1) || (outpipe && a3)) return -LX_ESPIPE;
    // Darwin assigns distinct inode numbers to the two ends of one pipe, so
    // stat cannot establish whether pipe-to-pipe splice is self-referential.
    // Refuse that subset until Shimmy tracks pipe identity across dup/fork.
    if (inpipe && outpipe) return -LX_EOPNOTSUPP;
    long long ioff = 0, ooff = 0, initial_in = 0, initial_out = 0;
    if (a1 && gmem_guest_export_copyin(NULL, a1, &ioff, sizeof ioff) != 0)
      return -LX_EFAULT;
    if (a3 && gmem_guest_export_copyin(NULL, a3, &ooff, sizeof ooff) != 0)
      return -LX_EFAULT;
    // Validate writable offsets before transferring any data. A late EFAULT
    // would otherwise report no transfer after advancing the pipe or file.
    if ((a1 && gmem_guest_copyout_checked(a1, &ioff, sizeof ioff) != 0) ||
        (a3 && gmem_guest_copyout_checked(a3, &ooff, sizeof ooff) != 0))
      return -LX_EFAULT;
    if ((a1 && ioff < 0) || (a3 && ooff < 0)) return -LX_EINVAL;
    size_t count = (size_t)a4;
    if (count > 1024 * 1024) count = 1024 * 1024;
    if ((a1 && ioff > INT64_MAX - (int64_t)count) ||
        (a3 && ooff > INT64_MAX - (int64_t)count)) return -LX_EINVAL;
    initial_in = ioff; initial_out = ooff;
    off_t old_in_pos = -1;
    if (!a1 && S_ISREG(inst.st_mode)) old_in_pos = lseek(in, 0, SEEK_CUR);
    long r = bounce_copy(in, a1 ? &ioff : NULL, out, a3 ? &ooff : NULL,
                         count, (int)(a5 & 0x2));
    if (r > 0 && old_in_pos >= 0)
      (void)lseek(in, old_in_pos + r, SEEK_SET);
    if (r >= 0) {
      if (a1) {
        ioff = initial_in + r;
        if (gmem_guest_copyout_checked(a1, &ioff, sizeof ioff) != 0)
          return -LX_EFAULT;
      }
      if (a3) {
        ooff = initial_out + r;
        if (gmem_guest_copyout_checked(a3, &ooff, sizeof ooff) != 0)
          return -LX_EFAULT;
      }
    }
    return r;
  }
  case 77: return -LX_ENOSYS;                                  // tee

  // ===== 81..92: sync, timers, utimes, caps =================================
  // Darwin's host-wide sync neither scopes writeback to guest files nor
  // guarantees Linux's completion semantics. Never claim it flushed the
  // guest after initiating unrelated host filesystem writeback.
  case 81: return -LX_ENOSYS;                                  // sync
  case 82: {                                                   // fsync
    int fd = (int)a0;
    // The guest must not flush a shim-owned host descriptor by number.
    if (fd < 0 || shim_fd_reserved(fd)) return -LX_EBADF;
    return ok0(fsync(fd));
  }
  case 83: {                                                   // fdatasync
    // macOS fsync does not push the drive cache; F_FULLFSYNC does. fdatasync only owes us
    // metadata-lite durability, so plain fsync is the right (and much cheaper) mapping.
    int fd = (int)a0;
    // A guest sync must not reach a shim-owned host descriptor by number.
    if (fd < 0 || shim_fd_reserved(fd)) return -LX_EBADF;
    return ok0(fsync(fd));
  }
  case 84: {                                                  // sync_file_range
    /* A zero-flags request performs no writeback on Linux.  Do not map a
     * range request to whole-file fsync: its wait/write/error contract is
     * different and is not implemented by this host bridge. */
    int fd = (int)a0;
    if (fd < 0 || shim_fd_reserved(fd)) return -LX_EBADF;
    struct stat st;
    if (fstat(fd, &st) < 0) return eret();
    int64_t offset = (int64_t)a1, nbytes = (int64_t)a2;
    unsigned flags = (unsigned)a3;
    if (flags & ~7u || offset < 0 || nbytes < 0 ||
        (nbytes && offset > INT64_MAX - nbytes)) return -LX_EINVAL;
    if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode) &&
        !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) return -LX_ESPIPE;
    return flags == 0 ? 0 : -LX_EOPNOTSUPP;
  }
  case 85: return timerfd_create_shim((int)a0, (int)a1);
  case 86: {                                                   // timerfd_settime
    linux_itimerspec next, old;
    if (!a2 || gmem_guest_export_copyin(NULL, a2, &next, sizeof next) != 0)
      return -LX_EFAULT;
    long result = timerfd_settime_shim((int)a0, (int)a1, &next, a3 ? &old : NULL);
    if (result < 0) return result;
    // Linux reports a failed old-value copy after the timer was updated.
    if (a3 && gmem_guest_copyout_checked(a3, &old, sizeof old) != 0)
      return -LX_EFAULT;
    return result;
  }
  case 87: {                                                   // timerfd_gettime
    linux_itimerspec current;
    long result = timerfd_gettime_shim((int)a0, &current);
    if (result < 0) return result; // Linux validates fd before copying out
    return gmem_guest_copyout_checked(a1, &current, sizeof current) == 0 ?
           0 : -LX_EFAULT;
  }
  case 88: {                                                   // utimensat(dfd, path, times, flags)
    unsigned flags = (unsigned)a3;
    if (flags & ~(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH)) return -LX_EINVAL;
    // The timespec layout matches, but Linux's UTIME_NOW/OMIT values do not.
    struct timespec times[2];
    const struct timespec *ts = NULL;
    if (a2) {
      if (gmem_guest_export_copyin(NULL, a2, times, sizeof times) != 0)
        return -LX_EFAULT;
      for (int i = 0; i < 2; ++i) {
        long ns = times[i].tv_nsec;
        if (ns == 0x3fffffffL) times[i].tv_nsec = UTIME_NOW;
        else if (ns == 0x3ffffffeL) times[i].tv_nsec = UTIME_OMIT;
        else if (ns < 0 || ns >= 1000000000L) return -LX_EINVAL;
      }
      ts = times;
    }
    if (!a1) {
      if (flags & LX_AT_SYMLINK_NOFOLLOW) return -LX_EINVAL;
      if ((int)a0 == LX_AT_FDCWD) return -LX_EFAULT;
      if ((int)a0 < 0 || shim_fd_reserved((int)a0) ||
          fcntl((int)a0, F_GETFD) < 0) return -LX_EBADF;
      return ok0(futimens((int)a0, ts)); // Linux permits NULL path on fd
    }
    // Copy a bounded guest string before passing it to host path machinery.
    char guest_path[PATH_MAX];
    e = copy_guest_cstr(a1, guest_path, sizeof guest_path);
    if (e < 0) return e;
    if (!guest_path[0]) {
      if (!(flags & LX_AT_EMPTY_PATH)) return -LX_ENOENT;
      if (flags & LX_AT_SYMLINK_NOFOLLOW) return -LX_EOPNOTSUPP;
      if ((int)a0 == LX_AT_FDCWD) {
        e = resolve_at_nf(LX_AT_FDCWD, ".", host, sizeof host, &hdirfd, 0);
        if (e < 0) return e;
        return ok0(utimensat(hdirfd, host, ts, 0));
      }
      if ((int)a0 < 0 || shim_fd_reserved((int)a0) ||
          fcntl((int)a0, F_GETFD) < 0) return -LX_EBADF;
      // Darwin's O_SYMLINK descriptor accepts futimens on the link inode,
      // matching Linux O_PATH|O_NOFOLLOW with AT_EMPTY_PATH.
      return ok0(futimens((int)a0, ts));
    }
    e = resolve_at_nf((int)a0, guest_path, host, sizeof host, &hdirfd,
                      (flags & LX_AT_SYMLINK_NOFOLLOW) != 0);
    if (e < 0) return e;
    return ok0(utimensat(hdirfd, host, ts, host_at_flags((int)flags)));
  }
  case 89: return -LX_EPERM;                                   // acct
  case 90: {                                                   // capget: report "no capabilities"
    // Linux uses a NULL data pointer to negotiate the capability ABI version.
    // libcap-ng initializes with version 0 and expects EINVAL plus the current
    // version written back; a bare success leaves its state uninitialized.
    if (!a0) return -LX_EFAULT;
    uint32_t header[2];
    if (gmem_guest_export_copyin(NULL, a0, header, sizeof header) != 0)
      return -LX_EFAULT;
    const uint32_t current = 0x20080522U; // _LINUX_CAPABILITY_VERSION_3
    uint32_t nwords = (header[0] == 0x19980330U) ? 1 : 2;
    if (header[0] != current && header[0] != 0x20071026U &&
        header[0] != 0x19980330U) {
      if (gmem_guest_copyout_checked(a0, &current, sizeof current) != 0)
        return -LX_EFAULT;
      return -LX_EINVAL;
    }
    if (!a1) return 0;
    uint32_t empty_data[6] = {0}; // two __user_cap_data_struct entries
    return gmem_guest_copyout_checked(a1, empty_data, nwords * 12) == 0
             ? 0 : -LX_EFAULT;
  }
  case 91: return g_uid0 ? 0 : -LX_EPERM;                       // capset: no-op under RDUID0
  case 92: {                                                    // personality: native Linux ABI subset
    // The query sentinel returns the calling task's old personality. This
    // ARM64 shim starts as PER_LINUX (0) and does not emulate the execution,
    // memory or uname behavior of other personalities and compatibility flags.
    // Accepting a flag while ignoring its side effect is unsafe (especially
    // ADDR_NO_RANDOMIZE or READ_IMPLIES_EXEC), so only a reset to 0 succeeds.
    unsigned int requested = (unsigned int)a0;
    if (requested == UINT32_MAX || requested == 0) return 0;
    return -LX_EOPNOTSUPP;
  }

  // ===== 95..106: process =================================================
  case 95: {                                                   // waitid
    /* Linux's raw ABI has a fifth rusage pointer. Darwin waitid cannot
     * provide it, so never consume a child while silently ignoring it. */
    if (a4) return -LX_EOPNOTSUPP;
    unsigned lopt = (unsigned)a3;
    const unsigned events = LX_WSTOPPED | LX_WEXITED | LX_WCONTINUED;
    const unsigned supported = events | LX_WNOHANG | LX_WNOWAIT;
    if ((lopt & ~supported) || !(lopt & events)) return -LX_EINVAL;
    if (a0 > LX_P_PIDFD) return -LX_EINVAL;
    if (a0 == P_PID && (int)a1 <= 0) return -LX_EINVAL;
    if (a0 == P_PGID && (long)(int)a1 < 0) return -LX_EINVAL;
    // Validate writable guest output before a wait can reap a child.
    unsigned char original[128];
    if (a2 && (gmem_guest_export_copyin(NULL, a2, original, sizeof original) != 0 ||
               gmem_guest_copyout_checked(a2, original, sizeof original) != 0))
      return -LX_EFAULT;
    int hopt = 0;
    if (lopt & LX_WNOHANG)    hopt |= WNOHANG;
    if (lopt & LX_WSTOPPED)   hopt |= WSTOPPED;
    if (lopt & LX_WEXITED)    hopt |= WEXITED;
    if (lopt & LX_WCONTINUED) hopt |= WCONTINUED;
    if (lopt & LX_WNOWAIT)    hopt |= WNOWAIT;
    siginfo_t si; memset(&si, 0, sizeof si);
    // waitid(P_PIDFD, pidfd, ...): macOS has no pidfd idtype; resolve the pid from the registry.
    if ((int)a0 == LX_P_PIDFD) {
      if (pidfd_shim_pid((int)a1) <= 0) return -LX_EBADF;
      if (pidfd_shim_waitid((int)a1, (id_t)a1, &si, hopt) != 0) return eret();
    } else if (waitid((idtype_t)a0, (id_t)a1, &si, hopt) != 0) return eret();
    if (si.si_pid > 0 && !(lopt & LX_WNOWAIT)) syscalls_signal_child_reaped(si.si_pid);
    if (a2) {                                                  // Linux siginfo layout, by hand
      unsigned char u[128] = {0};
      if (si.si_pid > 0) {
        *(int*)(u + 0)  = 17;                                  // Linux SIGCHLD
        *(int*)(u + 8)  = si.si_code;                          // CLD_* match
        *(int*)(u + 16) = (int)si.si_pid;
        unsigned uid = (unsigned)si.si_uid;
        if (g_uid0) {
          uint32_t guest_uid, guest_gid;
          uid = guest_cred_registry_lookup(si.si_pid, &guest_uid, &guest_gid) == 0
                  ? guest_uid : 0u; // legacy root child without a registry record
        }
        *(unsigned*)(u + 20) = uid;
        int status = si.si_status;
        if (si.si_code != CLD_EXITED) {
          status = 0;
          for (int lx = 1; lx < 32; ++lx)
            if (host_signo(lx) == si.si_status) { status = lx; break; }
        }
        *(int*)(u + 24) = status;
      }
      if (gmem_guest_copyout_checked(a2, u, sizeof u) != 0) return -LX_EFAULT;
    }
    return 0;
  }
  case 97: return a0 == 0 ? 0 : -LX_EPERM;                     // unshare(0) is a true no-op
  case 100: return guest_get_robust_list((long)a0, a1, a2);    // get_robust_list
  case 101: {                                                  // nanosleep
    struct lx_timespec requested;
    if (!a0 || gmem_guest_export_copyin(NULL, a0, &requested, sizeof requested) != 0)
      return -LX_EFAULT;
    if (requested.tv_sec < 0 || requested.tv_nsec < 0 ||
        requested.tv_nsec >= 1000000000LL) return -LX_EINVAL;
    struct timespec host_request = {.tv_sec=(time_t)requested.tv_sec,
                                    .tv_nsec=(long)requested.tv_nsec};
    struct timespec host_remaining = {0};
    if (nanosleep(&host_request, &host_remaining) == 0) return 0;
    int saved = errno;
    if (saved == EINTR && a1) {
      struct lx_timespec remaining = {.tv_sec=host_remaining.tv_sec,
                                      .tv_nsec=host_remaining.tv_nsec};
      if (gmem_guest_copyout_checked(a1, &remaining, sizeof remaining) != 0)
        return -LX_EFAULT;
    }
    return -linux_errno_from_host(saved);
  }
  case 102: {                                                  // getitimer
    struct itimerval hv;
    if (getitimer((int)a0, &hv) != 0) return eret();
    // Linux copies the result after validating which; a missing or invalid
    // guest destination must not be treated as success or written directly.
    struct lx_itimerval guest;
    lx_tv_put(&guest.it_interval, &hv.it_interval);
    lx_tv_put(&guest.it_value, &hv.it_value);
    return gmem_guest_copyout_checked(a1, &guest, sizeof guest) == 0 ? 0 : -LX_EFAULT;
  }
  case 103: {                                                  // setitimer
    struct itimerval hv, old;
    memset(&hv, 0, sizeof hv);
    if (a1) {
      struct lx_itimerval guest;
      if (gmem_guest_export_copyin(NULL, a1, &guest, sizeof guest) != 0)
        return -LX_EFAULT;
      if (guest.it_interval.tv_sec < 0 || guest.it_interval.tv_usec < 0 ||
          guest.it_interval.tv_usec >= 1000000 || guest.it_value.tv_sec < 0 ||
          guest.it_value.tv_usec < 0 || guest.it_value.tv_usec >= 1000000)
        return -LX_EINVAL;
      lx_tv_get(&hv.it_interval, &guest.it_interval);
      lx_tv_get(&hv.it_value, &guest.it_value);
    }
    // Linux treats a NULL new_value as a zero timer (disarm). Darwin's
    // NULL argument is not that contract; always pass the staged value.
    if (setitimer((int)a0, &hv, a2 ? &old : NULL) != 0) return eret();
    if (a2) {
      struct lx_itimerval guest_old;
      lx_tv_put(&guest_old.it_interval, &old.it_interval);
      lx_tv_put(&guest_old.it_value, &old.it_value);
      if (gmem_guest_copyout_checked(a2, &guest_old, sizeof guest_old) != 0)
        return -LX_EFAULT;
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
  case 116: {                                                  // syslog: no virtual printk ring
    // Linux permits READ_ALL (3) and SIZE_BUFFER (10) without CAP_SYSLOG
    // when dmesg_restrict is off. Shimmy has a guest /dev/kmsg transport but
    // no kernel printk ring with Linux's read/clear/size state, so neither
    // request can truthfully return a count or buffer capacity yet.
    if ((int)a0 == 3 || (int)a0 == 10) return -LX_ENOSYS;
    // Every other action is capability-gated in Linux before type-specific
    // validation. Shimmy's capget currently exposes no CAP_SYSLOG, including
    // under RDUID0, so reporting EPERM is coherent with that capability ABI.
    return -LX_EPERM;
  }
  case 117: return -LX_EPERM;                                  // ptrace
  case 118: {                                                  // sched_setparam: SCHED_OTHER/priority 0
    long target = (long)(int)a0;
    if (target < 0 || !a1) return -LX_EINVAL;
    int priority;
    if (gmem_guest_export_copyin(NULL, a1, &priority, sizeof priority) != 0)
      return -LX_EFAULT;
    if (target && target != guest_tid())
      return guest_tid_known((int)target) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    // Shimmy reports SCHED_OTHER for this thread. Linux requires priority 0
    // for that policy; accepting another value would claim a change we did
    // not make. RT scheduling and other guest threads need separate support.
    return priority == 0 ? 0 : -LX_EINVAL;
  }
  case 119: {                                                  // sched_setscheduler
    /* The guest exposes SCHED_OTHER.  Do not claim an RT policy or change
     * another guest thread's scheduler through a process-wide host API. */
    int target = (int)a0, policy = (int)a1;
    int32_t priority;
    if (policy < 0 || target < 0 || !a2) return -LX_EINVAL;
    if (gmem_guest_export_copyin(NULL, a2, &priority, sizeof priority) != 0)
      return -LX_EFAULT;
    if (policy != 0 && policy != 1 && policy != 2 && policy != 3 &&
        policy != 5 && policy != 6 && policy != 7 &&
        policy != (0x40000000 | 0) && policy != (0x40000000 | 1) &&
        policy != (0x40000000 | 2) && policy != (0x40000000 | 3) &&
        policy != (0x40000000 | 5) && policy != (0x40000000 | 6) &&
        policy != (0x40000000 | 7)) return -LX_EINVAL;
    if (target && target != guest_tid())
      return guest_tid_known(target) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    if (policy == 0) return priority == 0 ? 0 : -LX_EINVAL;
    return -LX_EOPNOTSUPP;
  }
  case 120: {                                                  // sched_getscheduler: current thread
    // The guest exposes SCHED_OTHER for the current thread. A Linux PID here
    // names a thread, not an arbitrary Darwin process. Do not report a policy
    // for a nonexistent guest ID or guess another thread's scheduling state.
    long target = (long)(int)a0;
    if (target < 0) return -LX_EINVAL;
    if (target == 0 || target == guest_tid())
      return 0;
    return guest_tid_known((int)target) ? -LX_EOPNOTSUPP : -LX_ESRCH;
  }
  case 121: {                                                  // sched_getparam
    long target = (long)(int)a0;
    if (target < 0 || !a1) return -LX_EINVAL;
    if (target && target != guest_tid())
      return guest_tid_known((int)target) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    // Only the caller's SCHED_OTHER policy is modeled. Its Linux RT
    // priority is zero regardless of its separate nice value.
    const int32_t priority = 0;
    return gmem_guest_copyout_checked(a1, &priority, sizeof priority) == 0
               ? 0 : -LX_EFAULT;
  }
  case 124: sched_yield(); return 0;                           // sched_yield
  case 125: {                                                  // sched_get_priority_max
    // Linux's priority ABI is fixed: Darwin's RT ranges are different.
    switch ((int)a0) {
      case 1: case 2: return 99;                               // FIFO, RR
      case 0: case 3: case 5: case 6: case 7: return 0;       // OTHER, BATCH, IDLE, DEADLINE, EXT
      default: return -LX_EINVAL;
    }
  }
  case 126: {                                                  // sched_get_priority_min
    // Linux's static priority ranges are part of its guest ABI and must not
    // inherit Darwin's different scheduler priority ranges.
    switch ((int)a0) {
      case 1: case 2: return 1;                                // FIFO, RR
      case 0: case 3: case 5: case 6: case 7: return 0;       // normal, batch, idle, deadline, ext
      default: return -LX_EINVAL;
    }
  }
  case 127: {                                                  // sched_rr_get_interval
    /* The guest currently exposes SCHED_OTHER, but does not model the Linux
     * fair runqueue or its per-task slice. A fixed 1 ms value is fabricated:
     * Linux derives this interval from scheduler state, and may report zero.
     * Reject a valid query until that state exists rather than publish a
     * misleading quantum. Never write through an unchecked guest pointer. */
    long target = (long)(int)a0;
    if (target < 0) return -LX_EINVAL;
    if (target && target != guest_tid())
      return guest_tid_known((int)target) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    if (!a1 || gmem_guest_range_mapped(a1, sizeof(struct lx_timespec)) != 0)
      return -LX_EFAULT;
    return -LX_EOPNOTSUPP;
  }
  case 128:                                                    // restart_syscall
    // Linux calls the thread's restart_block.fn. Shimmy never installs a
    // restart block for interrupted guest operations: nanosleep and
    // clock_nanosleep return EINTR directly, and guest signal frames are not
    // built. With no saved operation/deadline, restarting here would invent
    // work or extend a timeout. Linux's no-restart block returns EINTR.
    return -LX_EINTR;

  // ===== 130..140: signals =================================================
  case 130: case 131: {                                        // tkill(tid,sig) / tgkill(tgid,tid,sig)
    fx_note("SIGNAL tkill nr=%d a0=%ld a1=%ld a2=%ld (guest sig=%d)\n",
            (int)nr, (long)a0, (long)a1, (long)a2, (nr == 130) ? (int)a1 : (int)a2);
    int lsig = (nr == 130) ? (int)a1 : (int)a2;
    long tgt  = (nr == 130) ? (long)(int)a0 : (long)(int)a1;
    if (nr == 131) {
      // tgkill names both a thread group and a thread. A matching group
      // alone does not establish that the requested thread exists.
      long tgid = (long)(int)a0;
      if (tgid <= 0 || tgt <= 0) return -LX_EINVAL;
      if (tgid != syscalls_guest_pid() || !guest_tid_known((int)tgt))
        return -LX_ESRCH;
    }
    if (lsig < 0 || lsig > 64) return -LX_EINVAL;
    // Linux tkill requires a positive, existing thread ID even for signal
    // zero. A guest ID is never a Darwin PID; do not infer liveness from a
    // host PID or route an invalid target back to this process.
    if (nr == 130) {
      if (tgt <= 0) return -LX_EINVAL;
      if (!guest_tid_known((int)tgt)) return -LX_ESRCH;
    }
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
  case 132: return do_sigaltstack(a0, a1);
  case 133: {                                                  // rt_sigsuspend
    if (!a0) return -LX_EFAULT;
    if (a1 != 8) return -LX_EINVAL;
    uint64_t mask;
    if (gmem_guest_export_copyin(NULL, a0, &mask, sizeof mask) != 0)
      return -LX_EFAULT;
    return syscalls_signal_sigsuspend(mask);
  }
  case 136:                                                    // rt_sigpending
    if (a1 != 8) return -LX_EINVAL;
    if (!a0) return -LX_EFAULT;
    { // Only the shim's virtual standard-signal queue is modeled here.
      uint64_t pending = (uint64_t)syscalls_signal_pending();
      return gmem_guest_copyout_checked(a0, &pending, sizeof pending) == 0
                 ? 0 : -LX_EFAULT;
    }
  case 137: return -LX_EAGAIN;                                 // rt_sigtimedwait: timed out
  case 138: {                                                  // rt_sigqueueinfo
    int lsig = (int)a1;
    if (lsig < 0 || lsig > 64) return -LX_EINVAL;
    // Linux copies the sender's siginfo before targeting. Do not silently
    // discard an invalid or missing payload, even for signal-zero queries.
    unsigned char info[128];
    if (!a2 || gmem_guest_export_copyin(NULL, a2, info, sizeof info) != 0)
      return -LX_EFAULT;
    long target = (long)(int)a0;
    if (target <= 0) return -LX_EOPNOTSUPP; // no guest process-group delivery
    if (target != syscalls_guest_pid())
      return guest_child_known((int)target) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    // A self existence probe is useful and sends no signal. Nonzero delivery
    // must preserve siginfo and invoke guest handlers, neither of which the
    // current virtual signal path can promise. Never signal the host shim.
    return lsig == 0 ? 0 : -LX_EOPNOTSUPP;
  }
  case 240: {                                                  // rt_tgsigqueueinfo
    int lsig = (int)a2;
    if (lsig < 0 || lsig > 64) return -LX_EINVAL;
    // Linux copies siginfo before resolving the target. The guest payload
    // cannot be discarded or treated as a host siginfo_t.
    unsigned char info[128];
    if (!a3 || gmem_guest_export_copyin(NULL, a3, info, sizeof info) != 0)
      return -LX_EFAULT;
    long tgid = (long)(int)a0, tid = (long)(int)a1;
    if (tgid <= 0 || tid <= 0) return -LX_EINVAL;
    if (tgid != syscalls_guest_pid())
      return guest_child_known((int)tgid) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    if (tid != guest_tid())
      return guest_tid_known((int)tid) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    // Self signal-zero checks existence without delivery. Nonzero signaling
    // needs guest thread queues, siginfo and handler frames. Never kill the
    // host shim in place of that contract.
    return lsig == 0 ? 0 : -LX_EOPNOTSUPP;
  }
  case 140: {                                                  // setpriority
    // Guest PIDs, process groups and users are not host namespaces. Never
    // forward a guest selector that could renice another macOS process.
    int which = (int)a0, who = (int)a1, niceval = (int)a2;
    if (which < PRIO_PROCESS || which > PRIO_USER) return -LX_EINVAL;
    if (which != PRIO_PROCESS) return -LX_EOPNOTSUPP;
    if (who && who != (int)syscalls_guest_pid())
      return guest_child_known(who) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    if (niceval < -20) niceval = -20;
    if (niceval > 19) niceval = 19;
    return ok0(setpriority(PRIO_PROCESS, 0, niceval));
  }
  case 141: {                                                  // getpriority
    // Guest process/group/user IDs are not Darwin's namespaces. Preserve the
    // useful self query without exposing arbitrary host scheduling state.
    int which = (int)a0, who = (int)a1;
    if (which < PRIO_PROCESS || which > PRIO_USER) return -LX_EINVAL;
    if (which != PRIO_PROCESS) return -LX_EOPNOTSUPP;
    if (who && who != (int)syscalls_guest_pid())
      return guest_child_known(who) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    errno = 0;
    int r = getpriority(PRIO_PROCESS, 0);
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
    // Darwin has no setresuid operation. Never report that a requested saved
    // UID was installed when setreuid cannot express that third argument.
    // Refuse before changing either of the other host credentials.
    if ((int)a2 != -1) return -LX_EOPNOTSUPP;
    uid_t r = (uid_t)a0, ef = (uid_t)a1;
    if ((int)a0 == -1 && (int)a1 == -1) return 0;
    if (setreuid((int)a0 == -1 ? (uid_t)-1 : r, (int)a1 == -1 ? (uid_t)-1 : ef) != 0) return eret();
    return 0;
  }
  case 148: {                                                  // getresuid
    if (!a0 || !a1 || !a2) return -LX_EFAULT;
    uint32_t ids[3];
    if (g_uid0) {
      // The private guest credential model tracks the saved UID separately.
      ids[0] = guest_cred_uid(0);
      ids[1] = guest_cred_uid(1);
      ids[2] = guest_cred_uid(2);
    } else {
      // Darwin's process record carries the saved UID. Its public getuid and
      // geteuid calls alone would lose that value after setreuid.
      struct kinfo_proc kp = {0};
      size_t length = sizeof kp;
      int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
      if (sysctl(mib, 4, &kp, &length, NULL, 0) != 0) return eret();
      if (length != sizeof kp) return -LX_EIO;
      ids[0] = getuid();
      ids[1] = geteuid();
      ids[2] = kp.kp_eproc.e_pcred.p_svuid;
    }
    if (gmem_guest_copyout_checked(a0, &ids[0], sizeof ids[0]) != 0 ||
        gmem_guest_copyout_checked(a1, &ids[1], sizeof ids[1]) != 0 ||
        gmem_guest_copyout_checked(a2, &ids[2], sizeof ids[2]) != 0)
      return -LX_EFAULT;
    return 0;
  }
  case 149: {                                                  // setresgid
    if (g_uid0) return guest_cred_set_gid((int)a0, (int)a1, (int)a2);
    // Darwin has no setresgid.  setregid cannot install the explicit saved
    // GID requested by Linux, so never report that change as successful.
    // Refuse before touching either of the other host credentials.
    if ((int)a2 != -1) return -LX_EOPNOTSUPP;
    if ((int)a0 == -1 && (int)a1 == -1) return 0;
    if (setregid((int)a0 == -1 ? (gid_t)-1 : (gid_t)a0,
                 (int)a1 == -1 ? (gid_t)-1 : (gid_t)a1) != 0) return eret();
    return 0;
  }
  case 150: {                                                  // getresgid
    // Linux requires three writable gid_t outputs. Never dereference guest
    // addresses directly; the saved host GID is not necessarily the egid.
    if (!a0 || !a1 || !a2) return -LX_EFAULT;
    uint32_t ids[3];
    if (g_uid0) {
      for (int i = 0; i < 3; ++i) ids[i] = guest_cred_gid(i);
    } else {
      int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
      struct kinfo_proc proc = {0};
      size_t len = sizeof proc;
      if (sysctl(mib, 4, &proc, &len, NULL, 0) != 0) return eret();
      if (len < sizeof proc) return -LX_EOPNOTSUPP;
      ids[0] = (uint32_t)getgid();
      ids[1] = (uint32_t)getegid();
      ids[2] = (uint32_t)proc.kp_eproc.e_pcred.p_svgid;
    }
    if (gmem_guest_copyout_checked(a0, &ids[0], sizeof ids[0]) != 0 ||
        gmem_guest_copyout_checked(a1, &ids[1], sizeof ids[1]) != 0 ||
        gmem_guest_copyout_checked(a2, &ids[2], sizeof ids[2]) != 0)
      return -LX_EFAULT;
    return 0;
  }
  case 151: {                                                  // setfsuid: no-change subset
    // There is no separate guest fsuid or guest filesystem DAC yet. Linux
    // returns the previous fsuid even when a change is denied, so blindly
    // returning euid for a different requested ID falsely implies a switch
    // to privileged callers. Only a query or same-ID no-op is truthful.
    uint32_t current = g_uid0 ? guest_cred_uid(1) : (uint32_t)geteuid();
    uint32_t requested = (uint32_t)a0;
    if (requested == UINT32_MAX || requested == current) return current;
    return -LX_EOPNOTSUPP;
  }
  case 152: {                                                  // setfsgid: no-change subset
    // The guest has no separate fsgid or filesystem DAC enforcement. Linux
    // returns the previous fsgid even for a denied change, so returning egid
    // for every request would falsely imply a successful switch. The guest's
    // existing invariant is fsgid == egid while no override has succeeded.
    uint32_t current = g_uid0 ? guest_cred_gid(1) : (uint32_t)getegid();
    uint32_t requested = (uint32_t)a0;                         // Linux gid_t
    if (requested == UINT32_MAX || requested == current) return current;
    return -LX_EOPNOTSUPP;
  }
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
  case 154: {                                                  // setpgid
    // A guest PID is not a host PID (init is guest PID 1). Never let a guest
    // selector reassign an unrelated macOS process or join a host group.
    int pid = (int)a0, pgid = (int)a1;
    int self = (int)syscalls_guest_pid();
    if (pgid < 0) return -LX_EINVAL;
    if (pid && pid != self)
      return guest_child_known(pid) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    if (pgid && pgid != self) return -LX_EOPNOTSUPP;
    // Linux's (0, 0), (self, 0), and (self, self) all create the
    // caller's group. Host zeroes restrict both operands to this process.
    return ok0(setpgid(0, 0));
  }
  case 155: {                                                  // getpgid
    // The shim models a single guest process group equal to the guest PID.
    // getpgrp()/getpgid(self) therefore answer that PID; other guest PIDs have
    // no modelled group and stay unsupported. Never expose a host group.
    pid_t target = (pid_t)a0;
    if (target < 0) return -LX_EINVAL;
    if (target && target != syscalls_guest_pid()) return -LX_EOPNOTSUPP;
    return syscalls_guest_pid();
  }
  case 156: {                                                  // getsid
    long target = (long)(int)a0;
    long self = syscalls_guest_pid();
    if (target && target != self)
      return target > 0 && guest_child_known((int)target) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    // Query only this shim process. Passing an arbitrary guest PID to Darwin
    // could inspect an unrelated host process with the same numeric PID.
    pid_t sid = getsid(0);
    if (sid < 0) return eret();
    if (sid == getpid()) return self;
    // A direct guest child inherits its parent's session. The host parent PID
    // may be hidden by the virtual PID-1 mapping, so return the guest parent.
    if (sid == getppid() && syscalls_get_guest_ppid() > 0)
      return syscalls_get_guest_ppid();
    // A host session outside the known guest parent/self pair has no proven
    // guest PID identity. Never leak its host session leader into the guest.
    return -LX_EOPNOTSUPP;
  }
  case 157: {                                                  // setsid
    // The new session is created only for this shim process. Darwin returns
    // its host PID as the SID, but guest init has PID 1 and must never see
    // the host PID. The Linux return value is the caller's guest PID.
    if (setsid() < 0) return eret();
    return syscalls_guest_pid();
  }
  case 158: {                                                  // getgroups (gid_t is u32 on both)
    int sz = (int)a0;
    if (sz < 0) return -LX_EINVAL;
    // Virtual-root mode must not expose the host account's group list.
    // Retain the established synthetic singleton needed by systemd until a
    // real guest credential/VFS model can make setgroups effective.
    if (g_uid0) {
      if (sz == 0) return 1;
      gid_t group = (gid_t)guest_cred_gid(1);
      return gmem_guest_copyout_checked(a1, &group, sizeof group) == 0 ? 1 : -LX_EFAULT;
    }
    // Host-identity mode deliberately uses Darwin's current groups, but
    // stages them in host memory so kernel code never receives a guest ptr.
    int n = getgroups(0, NULL);
    if (n < 0) return eret();
    if (sz == 0) return n;
    if (sz < n) return -LX_EINVAL;
    if (!n) return 0;
    gid_t *groups = calloc((size_t)n, sizeof *groups);
    if (!groups) return -LX_ENOMEM;
    int got = getgroups(n, groups);
    if (got < 0) { long err = eret(); free(groups); return err; }
    long result = gmem_guest_copyout_checked(a1, groups,
                                             (size_t)got * sizeof *groups) == 0 ?
                  got : -LX_EFAULT;
    free(groups);
    return result;
  }
  case 159: return g_uid0 ? 0 : ok0(setgroups((int)a0, (const gid_t*)a1));  // setgroups
  case 161: {                                                  // sethostname
    if (a1 > 64) return -LX_EINVAL;
    char name[65] = {0};
    if (a1 && gmem_guest_export_copyin(NULL, a0, name, (size_t)a1) != 0)
      return -LX_EFAULT;
    return syscalls_guest_sethostname(name, (size_t)a1);
  }
  case 162: {                                                  // setdomainname
    if (a1 > 64) return -LX_EINVAL;
    char name[65] = {0};
    if (a1 && gmem_guest_export_copyin(NULL, a0, name, (size_t)a1) != 0)
      return -LX_EFAULT;
    return syscalls_guest_setdomainname(name, (size_t)a1);
  }
  case 163: {                                                  // getrlimit
    int resource = (int)a0;
    if (resource < 0 || resource > 15) return -LX_EINVAL;
    // Darwin aliases RLIMIT_RSS to RLIMIT_AS; exposing that as an independent
    // Linux limit would be false. The remaining unmapped limits likewise have
    // no guest model, so do not invent an unlimited value.
    if (resource == 5) return -LX_EOPNOTSUPP;
    int hr = host_rlimit(resource);
    if (hr < 0) return -LX_EOPNOTSUPP;
    if (!a1) return -LX_EFAULT;
    struct rlimit rl;
    if (getrlimit(hr, &rl) != 0) return eret();
    // Match prlimit64's guest-visible NOFILE soft cap. This avoids a million
    // guest close() traps before exec when the Darwin limit is very large.
    if (resource == 7 && rl.rlim_cur > 4096) rl.rlim_cur = 4096;
    struct lx_rlimit out = {lx_rlim(rl.rlim_cur), lx_rlim(rl.rlim_max)};
    if (gmem_guest_copyout_checked(a1, &out, sizeof out) != 0)
      return -LX_EFAULT;
    return 0;
  }
  case 164: {                                                  // setrlimit
    // Only Linux resources with a compatible per-process Darwin limit can
    // be changed here. An unsupported limit must not report success, and
    // RLIMIT_RSS (5) aliases RLIMIT_AS on Darwin.
    if (a0 > 15) return -LX_EINVAL;
    if (a0 == 5 || a0 >= 10) return -LX_EOPNOTSUPP;
    int hr = host_rlimit((int)a0);
    if (hr < 0) return -LX_EOPNOTSUPP;
    struct lx_rlimit n;
    if (!a1 || gmem_guest_export_copyin(NULL, a1, &n, sizeof n) != 0)
      return -LX_EFAULT;
    if (n.rlim_cur > n.rlim_max) return -LX_EINVAL;
    // The shim advertises at most 4096 open files through prlimit64 and
    // getrlimit; accepting a larger soft limit would contradict that view.
    if (a0 == 7 && n.rlim_cur > 4096) return -LX_EOPNOTSUPP;
    // Linux's infinity is all-ones, whereas Darwin has a smaller ceiling.
    // Do not silently turn an otherwise finite Linux value into infinity.
    if ((n.rlim_cur != LX_RLIM_INFINITY && n.rlim_cur > (uint64_t)RLIM_INFINITY) ||
        (n.rlim_max != LX_RLIM_INFINITY && n.rlim_max > (uint64_t)RLIM_INFINITY))
      return -LX_EOPNOTSUPP;
    struct rlimit rl;
    rl.rlim_cur = host_rlim(n.rlim_cur); rl.rlim_max = host_rlim(n.rlim_max);
    return ok0(setrlimit(hr, &rl));
  }
  case 166: return (long)umask((mode_t)(a0 & 0777));           // umask: Linux masks to S_IRWXUGO
  case 167: return do_prctl((int)a0, a1, a2, a3, a4);          // prctl
  case 168: {                                                  // getcpu
    // The guest's online CPU set uses the host's contiguous CPU indices,
    // capped at 32 by /sys and sched_getaffinity. Do not always report CPU 0:
    // callers use this number to choose per-CPU data for their actual thread.
    if (!a0 && !a1) return 0;
    size_t host_cpu = 0;
    int rc = pthread_cpu_number_np(&host_cpu);
    if (rc) return -linux_errno_from_host(rc);
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online < 1) online = 1;
    if (online > 32) online = 32;
    // A host with more than 32 CPUs can run us outside the guest's advertised
    // set. Refuse that observation instead of fabricating another CPU ID.
    if (host_cpu >= (size_t)online) return -LX_EOPNOTSUPP;
    unsigned cpu = (unsigned)host_cpu, node = 0; // one guest NUMA node
    if (a0 && gmem_guest_copyout_checked(a0, &cpu, sizeof cpu) != 0)
      return -LX_EFAULT;
    if (a1 && gmem_guest_copyout_checked(a1, &node, sizeof node) != 0)
      return -LX_EFAULT;
    return 0;
  }
  case 169: {                                                  // gettimeofday (Linux timeval!)
    struct timeval tv;
    if (gettimeofday(&tv, 0) != 0) return eret();
    if (a0) {
      struct lx_timeval guest_tv;
      lx_tv_put(&guest_tv, &tv);
      if (gmem_guest_copyout_checked(a0, &guest_tv, sizeof guest_tv) != 0)
        return -LX_EFAULT;
    }
    if (a1) {
      // Linux's obsolete timezone ABI is two 32-bit fields. The shim exposes
      // UTC, matching the existing zero/zero result without a raw guest write.
      const int32_t timezone[2] = {0, 0};
      if (gmem_guest_copyout_checked(a1, timezone, sizeof timezone) != 0)
        return -LX_EFAULT;
    }
    return 0;
  }
  case 170: return -LX_EPERM;                                  // settimeofday
  case 171: {                                                  // adjtimex
    // Linux arm64's timex is 208 bytes. Only the read-only modes=0 query is
    // backed by the host NTP clock. Never allow a guest to discipline the Mac.
    struct lx_timex_clock tx;
    if (!a0 || gmem_guest_export_copyin(NULL, a0, &tx, sizeof tx) != 0)
      return -LX_EFAULT;
    if (tx.modes) return -LX_EOPNOTSUPP;
    struct timex host = {0};
    int state = ntp_adjtime(&host);
    if (state < 0) return eret();
    struct ntptimeval clock = {0};
    if (ntp_gettime(&clock) < 0) return eret();
    memset(&tx, 0, sizeof tx);
    tx.offset = host.offset;
    tx.freq = host.freq;
    tx.maxerror = host.maxerror;
    tx.esterror = host.esterror;
    tx.status = host.status;
    tx.constant = host.constant;
    tx.precision = host.precision;
    tx.tolerance = host.tolerance;
    tx.time_sec = clock.time.tv_sec;
    tx.time_usec = (host.status & STA_NANO) ? clock.time.tv_nsec :
        clock.time.tv_nsec / 1000;
    tx.tick = 1000000 / LX_USER_HZ;
    tx.ppsfreq = host.ppsfreq;
    tx.jitter = host.jitter;
    tx.shift = host.shift;
    tx.stabil = host.stabil;
    tx.jitcnt = host.jitcnt;
    tx.calcnt = host.calcnt;
    tx.errcnt = host.errcnt;
    tx.stbcnt = host.stbcnt;
    tx.tai = clock.tai;
    if (gmem_guest_copyout_checked(a0, &tx, sizeof tx) != 0)
      return -LX_EFAULT;
    return state;
  }

  // ===== 180..197: SysV IPC / POSIX mqueues ================================
  // POSIX mqueues (mq_open/...) have NO macOS equivalent (Darwin implements SysV IPC and POSIX
  // shm, but not POSIX mq). They stay a clean -ENOSYS on purpose: the honest milestone here is
  // the SysV message-queue forwarding below plus /dev/shm for POSIX shm_open. A program that
  // hard-requires mq gets an unambiguous ENOSYS to react to, not a half-correct emulation.
  case 180: case 181: case 182: case 183: case 184: case 185:  // mq_*
    return -LX_ENOSYS;

  // Darwin SysV queues share a host-wide namespace. Passing a guest key to msgget can resolve
  // another macOS process's queue or create a persistent host object. Until all four message
  // calls share an instance-scoped ID registry, refuse queue lookup and creation truthfully.
  // Queue operations are refused below until an instance-scoped backend exists.
  case 186: return -LX_ENOSYS;                                   // msgget
  // A raw guest msqid could name any host-global Darwin queue. Since msgget
  // cannot issue an instance-scoped guest ID yet, no ID is safe to accept.
  // Do not inspect the guest buffer or call host msgsnd, even for zero bytes.
  case 189: return -LX_ENOSYS;                                   // msgsnd
  // A guest-supplied msqid is a raw Darwin ID here. Refuse before touching that queue or the
  // guest buffer until a shared instance-scoped SysV message-queue registry is implemented.
  case 188: return -LX_ENOSYS;                                   // msgrcv
  // Darwin message queue IDs are host-global numbers. A guest can supply any
  // host ID here even though msgget is disabled. In particular IPC_RMID and
  // IPC_SET could delete or alter another macOS process's queue. No command
  // may reach host msgctl until all message syscalls use guest-scoped IDs.
  case 187: return -LX_ENOSYS;                                   // msgctl

  // Darwin's SysV namespace is host-wide. Refuse all guest keys, raw IDs,
  // and attachment addresses until Shimmy has an instance-scoped IPC backend.
  // Qt QSharedMemory and QSystemSemaphore may receive ENOSYS in this state.
  case 194: return -LX_ENOSYS;                                 // shmget
  // A guest ID must not attach a same-user host segment.
  case 196: return -LX_ENOSYS;                                 // shmat
  // A guest pointer does not establish ownership of a host attachment.
  case 197: return -LX_ENOSYS;                                 // shmdt
  // A raw guest ID could name a host segment; IPC_RMID would remove it.
  case 195: return -LX_ENOSYS;                                 // shmctl

  case 190: return -LX_ENOSYS;                                 // semget: no private guest IPC namespace
  // A guest semid is a raw Darwin host-global ID here. Refuse before touching the semaphore
  // set or guest operation array until all SysV semaphore calls share an instance-scoped ID map.
  case 193: return -LX_ENOSYS;                                 // semop
  // Darwin semaphore IDs are host-global. Disabling semget does not prevent
  // a guest from guessing a host ID and using IPC_RMID, SETVAL or SETALL.
  // Refuse every command until the four semaphore calls share guest-scoped
  // IDs and ownership checks. No host IPC or guest pointer is touched here.
  case 191: return -LX_ENOSYS;                                 // semctl
  // Darwin SysV semaphores have a host-global ID space. #190 semget cannot
  // issue a guest-owned ID, so no raw semid is safe to pass to host semop.
  // Darwin also lacks this syscall's timeout; a blocking semop could wait
  // indefinitely when the guest requested a finite bound. Refuse before
  // reading guest pointers or touching a host semaphore.
  case 192: return -LX_ENOSYS;                                 // semtimedop

  // ===== 213..241: memory ==================================================
  case 213: {                                                  // readahead: regular-file async hint
    int fd = (int)a0;
    int64_t offset = (int64_t)a1;
    size_t count = (size_t)a2;
    if (fd < 0 || shim_fd_reserved(fd)) return -LX_EBADF;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) return -LX_EBADF;
    if ((fl & O_ACCMODE) == O_WRONLY) return -LX_EBADF;
    struct stat st;
    if (fstat(fd, &st) < 0) return eret();
    if (!S_ISREG(st.st_mode)) return -LX_EINVAL;
    if (offset < 0) return -LX_EINVAL;
    // Linux's zero count means from offset to EOF. Nothing at/after EOF
    // needs a read hint, but the fd/type/offset checks still apply.
    if (offset >= st.st_size) return 0;
    uint64_t available = (uint64_t)(st.st_size - offset);
    uint64_t length = count && (uint64_t)count < available ? (uint64_t)count : available;
    // Darwin radvisory.ra_count is int, so do not silently truncate a hint.
    if (length > INT_MAX) return -LX_EOPNOTSUPP;
    struct radvisory advice = { .ra_offset = (off_t)offset,
                                .ra_count = (int)length };
    return fcntl(fd, F_RDADVISE, &advice) < 0 ? eret() : 0;
  }
  case 217: case 218: case 219: return -LX_ENOSYS;             // add_key/request_key/keyctl
  case 220: return -LX_ENOSYS;   // clone — handled in the trap handler (needs the register file)
  case 221: {                                                 // execve(path, argv, envp)
    // Replacing the guest image in place would mean unwinding every mapping, the veneer arenas
    // and the trap tables, and re-entering the loader — all while running on the guest's own
    // stack. Re-execing the SHIM is the same thing done by the kernel instead: the host process
    // is replaced by a fresh shim that loads the new guest from scratch. File descriptors
    // survive execve unless they are CLOEXEC, which is exactly what a spawned helper needs.
    char* const* gargv = (char* const*)a1;
    char* const* genvp = (char* const*)a2;
    char path[PATH_MAX];
    long e = copy_guest_cstr(a0, path, sizeof path);
    if (e < 0) return e;
    return guest_execve(path, gargv, genvp);
  }
  case 223: {                                                  // fadvise64: bounded NORMAL advice
    // ARM64 uses (int fd, loff_t offset, loff_t len, int advice). An advice
    // request is nonbinding, but success must still validate its descriptor,
    // range and policy. Shimmy has no per-open-file readahead state, so only
    // NORMAL (the initial/default state) is a truthful no-op today.
    int fd = (int)a0, advice = (int)a3;
    if (fd < 0 || shim_fd_reserved(fd)) return -LX_EBADF;
    struct stat st;
    if (fstat(fd, &st) < 0) return -LX_EBADF;
    if (S_ISFIFO(st.st_mode)) return -LX_ESPIPE;
    if ((int64_t)a1 < 0 || (int64_t)a2 < 0) return -LX_EINVAL;
    if (advice < 0 || advice > 5) return -LX_EINVAL;
    if (!S_ISREG(st.st_mode) || advice != 0) return -LX_EOPNOTSUPP;
    return 0;
  }
  case 224: case 225: return -LX_EPERM;                        // swapon/swapoff
  case 227: {                                                  // msync (MS_SYNC value differs!)
    unsigned int flags = (unsigned int)a2;
    const unsigned long page = 16384;                          // guest AT_PAGESZ
    if (flags & ~(LX_MS_ASYNC | LX_MS_INVALIDATE | LX_MS_SYNC)) return -LX_EINVAL;
    if ((flags & LX_MS_ASYNC) && (flags & LX_MS_SYNC)) return -LX_EINVAL;
    if (a0 & (page - 1)) return -LX_EINVAL;
    if (a1 > ULONG_MAX - (page - 1)) return -LX_ENOMEM;
    unsigned long rounded = (a1 + page - 1) & ~(page - 1);
    if (a0 > ULONG_MAX - rounded) return -LX_ENOMEM;
    if (!rounded) return 0;
    if (rounded > 16ul * 1024 * 1024) return -LX_EOPNOTSUPP;
    if (a0 < FOUR_GB || gmem_guest_range_mapped(a0, (size_t)rounded) != 0 ||
        !host_range_mapped(a0, (size_t)rounded)) return -LX_ENOMEM;
    // Linux MS_ASYNC and flags=0 do not initiate writeback. The host has no
    // verified equivalent for MS_INVALIDATE's Linux locked-page checks.
    if (flags & LX_MS_INVALIDATE) return -LX_EOPNOTSUPP;
    if (!(flags & LX_MS_SYNC)) return 0;
    return ok0(msync((void*)a0, (size_t)rounded, MS_SYNC));
  }
  case 228: return guest_mlock_eager(a0, a1);                   // mlock
  case 229: {                                                   // munlock
    if (!a1) return 0;
    unsigned long base, rounded;
    long status = guest_lock_range(a0, a1, &base, &rounded);
    if (status < 0) return status;
    return ok0(munlock((void*)base, (size_t)rounded));
  }
  case 230: {                                                  // mlockall
    // Linux MCL_CURRENT=1, MCL_FUTURE=2, MCL_ONFAULT=4. Never forward guest
    // flags to Darwin mlockall(): on this Mac it is ENOSYS, and a host success
    // would lock Shimmy-private pages that are not part of the guest address
    // space. Lock only the guest ledger's ranges (MCL_CURRENT) and record the
    // future-mapping policy (MCL_FUTURE) for gmem_mmap to apply.
    const unsigned long mcl_known = 1ul | 2ul | 4ul;
    if (a0 & ~mcl_known) return -LX_EINVAL;
    if (a0 & 4ul) return -LX_EOPNOTSUPP;          // MCL_ONFAULT: deferred fault-time lock
    if (a0 & 1ul) {                               // MCL_CURRENT: lock every current mapping
      size_t n = gmem_guest_range_snapshot(NULL, 0);
      struct gmem_guest_span* spans = n ? calloc(n, sizeof *spans) : NULL;
      if (n && !spans) return -LX_ENOMEM;
      size_t got = gmem_guest_range_snapshot(spans, n);
      long status = 0;
      size_t i = 0;
      for (; i < got; i++) {
        unsigned long lo = (unsigned long)spans[i].lo;
        unsigned long len = (unsigned long)(spans[i].hi - spans[i].lo);
        // Re-validate the snapshot before host mutation: it must still be guest
        // owned and host mapped, or the range may have been unmapped concurrently.
        if (lo < FOUR_GB || !host_range_mapped(lo, len)) { status = -LX_ENOMEM; break; }
        // Executable (MAP_JIT) text cannot be wired on Apple Silicon; skip it
        // and lock the data/stack/heap that actually carry the guest's secrets.
        if (host_range_executable(lo, len)) continue;
        if (mlock((void*)lo, len) != 0) { status = eret(); break; }
      }
      if (status < 0) {                           // atomic rollback of MCL_CURRENT
        for (size_t j = 0; j < i; j++) {          // unlock what was actually pinned
          if (host_range_executable((unsigned long)spans[j].lo,
                                    (unsigned long)(spans[j].hi - spans[j].lo))) continue;
          munlock((void*)spans[j].lo, (size_t)(spans[j].hi - spans[j].lo));
        }
        free(spans);
        return status;
      }
      free(spans);
    }
    if (a0 & 2ul) gmem_mlock_future_set(1);        // MCL_FUTURE
    return 0;
  }
  case 231: {                                                  // munlockall
    // Clear the future-lock policy and unlock every current guest mapping.
    // munlock of a never-locked page is a Linux/macOS no-op, so walking the
    // whole ledger releases exactly what mlock/mlock2/mlockall pinned without
    // touching Shimmy-private (non-ledger) memory. DRM/ALSA mappings are not in
    // the ledger and were never lockable through #228, so they need no unlock.
    gmem_mlock_future_set(0);
    size_t n = gmem_guest_range_snapshot(NULL, 0);
    struct gmem_guest_span* spans = n ? calloc(n, sizeof *spans) : NULL;
    if (n && !spans) return -LX_ENOMEM;
    size_t got = gmem_guest_range_snapshot(spans, n);
    long status = 0;
    for (size_t i = 0; i < got; i++) {
      unsigned long lo = (unsigned long)spans[i].lo;
      unsigned long len = (unsigned long)(spans[i].hi - spans[i].lo);
      // ENOMEM here means the span was unmapped between snapshot and unlock;
      // the kernel already released that lock, so treat it as done.
      if (munlock((void*)lo, len) != 0 && errno != ENOMEM && !status) status = eret();
    }
    free(spans);
    return status;
  }
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
    const unsigned long page = 16384;                          // guest AT_PAGESZ
    if (a0 & (page - 1)) return -LX_EINVAL;
    if (a1 > 16ul * 1024 * 1024) return -LX_EOPNOTSUPP;
    if (a1 > ULONG_MAX - (page - 1)) return -LX_ENOMEM;
    unsigned long rounded = (a1 + page - 1) & ~(page - 1);
    if (a0 > ULONG_MAX - rounded) return -LX_ENOMEM;
    if (!rounded) return 0;
    if (a0 < FOUR_GB || gmem_guest_range_mapped(a0, (size_t)rounded) != 0 ||
        !host_range_mapped(a0, (size_t)rounded)) return -LX_ENOMEM;
    // Never let Darwin write residency bytes through an unchecked guest
    // pointer. The bounded stack vector is copied out only after it succeeds.
    unsigned char vec[1024];
    if (mincore((void*)a0, (size_t)rounded, (char*)vec) != 0) return eret();
    return gmem_guest_copyout_checked(a2, vec, (size_t)(rounded / page)) == 0 ?
           0 : -LX_EFAULT;
  }
  case 234: return -LX_ENOSYS;                                  // remap_file_pages (dead on Linux too)
  case 235: {                                                   // mbind: default-policy subset
    // A single virtual node makes MPOL_DEFAULT a truthful no-op only when
    // the range belongs to guest memory. Do not claim that BIND, INTERLEAVE,
    // migration, or per-VMA policy state was installed when none exists.
    const unsigned long page = 16384;                           // guest AT_PAGESZ
    const unsigned long mode_flags = (1ul << 13) | (1ul << 14) | (1ul << 15);
    unsigned long mode = a2 & ~mode_flags;
    if ((a2 & ~(mode_flags | 0x1ffful)) || mode >= 7 ||
        ((a2 & (1ul << 14)) && (a2 & (1ul << 15))))
      return -LX_EINVAL;
    unsigned flags = (unsigned)a5;                               // Linux ABI is u32
    if (flags & ~7u) return -LX_EINVAL;                           // MPOL_MF_VALID
    if (a0 & (page - 1)) return -LX_EINVAL;
    if (a1 > ULONG_MAX - (page - 1)) return -LX_EINVAL;
    unsigned long rounded = (a1 + page - 1) & ~(page - 1);
    if (rounded > ULONG_MAX - a0) return -LX_EINVAL;
    if (mode || (a2 & mode_flags) || a3 || flags)
      return -LX_EOPNOTSUPP;
    // Linux ignores maxnode when the nodemask pointer is NULL.
    (void)a4;
    if (!rounded) return 0;
    if (a0 < FOUR_GB || !host_range_mapped(a0, (size_t)rounded))
      return -LX_EFAULT;
    // This bounded subset checks every guest page in the range. A host-only
    // mapping or unreadable guest page is outside it, even if Mach says mapped.
    if (rounded > 16ul * 1024 * 1024) return -LX_EOPNOTSUPP;
    for (unsigned long off = 0; off < rounded; off += page) {
      unsigned char probe;
      if (gmem_guest_export_copyin(NULL, a0 + off, &probe, 1) != 0)
        return -LX_EOPNOTSUPP;
    }
    return 0;
  }
  case 236: {                                                   // get_mempolicy
    /* This runtime exposes one NUMA node and only the process default policy.
     * MPOL_F_ADDR needs per-VMA policy state, which is not modeled here. */
    const unsigned long flags = (unsigned long)a4;
    const unsigned long maxnode = (unsigned long)a2;
    const unsigned long MPOL_F_NODE = 1, MPOL_F_ADDR = 2;
    const unsigned long MPOL_F_MEMS_ALLOWED = 4;
    if (flags & ~(MPOL_F_NODE | MPOL_F_ADDR | MPOL_F_MEMS_ALLOWED))
      return -LX_EINVAL;
    if ((flags & MPOL_F_MEMS_ALLOWED) && (flags & (MPOL_F_NODE | MPOL_F_ADDR)))
      return -LX_EINVAL;
    if (flags & MPOL_F_ADDR) return -LX_EOPNOTSUPP;
    if (flags & MPOL_F_NODE) return -LX_EINVAL; // MPOL_DEFAULT has no preferred node
    if (a3 && !(flags & MPOL_F_MEMS_ALLOWED)) return -LX_EINVAL;
    if (a1 && maxnode < 1) return -LX_EINVAL;
    /* Linux copy_nodes_to_user() rounds (maxnode - 1) up to a word and
     * rejects copies larger than one page. Avoid arithmetic overflow. */
    if (a1 && maxnode > 32769UL) return -LX_EINVAL;
    if (a0) {
      const int policy = 0; // MPOL_DEFAULT
      if (gmem_guest_copyout_checked(a0, &policy, sizeof policy) != 0)
        return -LX_EFAULT;
    }
    if (a1) {
      const size_t bytes = (size_t)(((maxnode - 1 + 63) / 64) * 8);
      unsigned char mask[4096] = {0};
      if (flags & MPOL_F_MEMS_ALLOWED) mask[0] = 1;
      if (bytes && gmem_guest_copyout_checked(a1, mask, bytes) != 0)
        return -LX_EFAULT;
    }
    return 0;
  }
  case 237: {                                                   // set_mempolicy: default-only
    // On the one-node guest, clearing the per-task policy is truthful. Other
    // modes require policy state and allocation behavior; success would claim
    // placement that Darwin's allocator does not implement.
    enum { MPOL_MODE_FLAGS = 0xe000u, MPOL_STATIC = 0x8000u,
           MPOL_RELATIVE = 0x4000u, MPOL_NUMA_BALANCING = 0x2000u,
           MPOL_BIND = 2, MPOL_PREFERRED_MANY = 5, MPOL_MAX = 7 };
    unsigned raw = (unsigned)(int)a0;
    unsigned mode = raw & ~MPOL_MODE_FLAGS;
    unsigned flags = raw & MPOL_MODE_FLAGS;
    if (mode >= MPOL_MAX ||
        ((flags & (MPOL_STATIC | MPOL_RELATIVE)) ==
         (MPOL_STATIC | MPOL_RELATIVE)) ||
        ((flags & MPOL_NUMA_BALANCING) &&
         mode != MPOL_BIND && mode != MPOL_PREFERRED_MANY))
      return -LX_EINVAL;
    if (mode || flags) return -LX_EOPNOTSUPP;

    // Linux get_nodes() decrements maxnode, and ignores a NULL mask or a
    // one-bit mask without dereferencing the latter. Preserve those cases.
    if (!a1 || a2 == 1) return 0;
    if (!a2) return -LX_EINVAL;
    if (a2 > 65) return -LX_EOPNOTSUPP; // wider masks are outside this subset
    uint64_t bits;
    if (gmem_guest_export_copyin(NULL, a1, &bits, sizeof bits) != 0)
      return -LX_EFAULT;
    unsigned count = (unsigned)a2 - 1;
    uint64_t relevant = count == 64 ? UINT64_MAX : ((1ULL << count) - 1);
    return (bits & relevant) ? -LX_EINVAL : 0;
  }
  case 238: {                                                   // migrate_pages: one-node no-op
    // The guest exposes only node 0. With no other source or destination
    // node, validated self-migration has no pages to move. Do not claim a
    // cross-process migration or accept a node the guest cannot represent.
    long pid = (long)(int)a0;
    unsigned long maxnode = a1;
    if (maxnode > 32769UL) return -LX_EINVAL; // Linux get_nodes page limit
    if (!maxnode && (a2 || a3)) return -LX_EINVAL;
    if (maxnode > 65) return -LX_EOPNOTSUPP; // bounded one-word masks
    uint64_t old = 0, next = 0;
    if (maxnode > 1 && a2 &&
        gmem_guest_export_copyin(NULL, a2, &old, sizeof old) != 0)
      return -LX_EFAULT;
    if (maxnode > 1 && a3 &&
        gmem_guest_export_copyin(NULL, a3, &next, sizeof next) != 0)
      return -LX_EFAULT;
    if (maxnode > 1) {
      unsigned bits = (unsigned)(maxnode - 1);
      uint64_t relevant = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
      if (((old | next) & relevant & ~UINT64_C(1)) != 0)
        return -LX_EOPNOTSUPP;
    }
    if (pid && pid != syscalls_guest_pid())
      return guest_child_known((int)pid) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    return 0;
  }
  case 239: {                                                   // move_pages: query current process
    /* Shimmy presents one NUMA node. Darwin has no equivalent to migrating
     * Linux pages, but a query can report an actual resident guest page on
     * that node. Do not report a fabricated location for holes or pages the
     * host has not made resident. */
    long pid = (long)(int)a0;
    size_t count = (size_t)a1;
    unsigned flags = (unsigned)a5;
    if (flags & ~3u) return -LX_EINVAL;  // MPOL_MF_MOVE / MOVE_ALL
    if (pid && pid != syscalls_guest_pid()) return -LX_EOPNOTSUPP;
    if (a3 || flags) return -LX_EOPNOTSUPP; // no page migration
    if (!count) return 0;
    if (!a2 || !a4) return -LX_EFAULT;
    // Bound the host stack and copy through the guest-memory registry. A
    // host-mapped pointer alone can name Shimmy's private address space.
    if (count > 256) return -LX_EOPNOTSUPP;
    uint64_t pages[256];
    int32_t status[256];
    if (gmem_guest_export_copyin(NULL, a2, pages, count * sizeof pages[0]) != 0 ||
        gmem_guest_range_mapped(a4, count * sizeof status[0]) != 0)
      return -LX_EFAULT;
    const size_t page = (size_t)getpagesize();
    for (size_t i = 0; i < count; ++i) {
      unsigned long addr = (unsigned long)pages[i];
      unsigned char resident = 0;
      if (addr < FOUR_GB || (addr & (page - 1)) ||
          gmem_guest_range_mapped(addr, page) != 0 ||
          !host_range_mapped(addr, page)) {
        status[i] = -LX_EFAULT;
      } else if (mincore((void *)addr, page, (char *)&resident) < 0) {
        status[i] = -LX_EFAULT;
      } else {
        status[i] = resident & 1 ? 0 : -LX_ENOENT;
      }
    }
    return gmem_guest_copyout_checked(a4, status,
                                      count * sizeof status[0]) == 0 ? 0 : -LX_EFAULT;
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
    // The shim has no exportable file handles or matching open_by_handle_at.
    // An empty handle and host st_dev are not a valid Linux success result.
    if (!a2 || !a3) return -LX_EFAULT;
    if (a4 & ~((uint64_t)LX_AT_SYMLINK_FOLLOW | LX_AT_EMPTY_PATH | LX_AT_HANDLE_FID))
      return -LX_EINVAL;
    unsigned int capacity;
    int mount_id_probe;
    if (gmem_guest_export_copyin(NULL, a2, &capacity, sizeof capacity) != 0 ||
        gmem_guest_export_copyin(NULL, a3, &mount_id_probe, sizeof mount_id_probe) != 0)
      return -LX_EFAULT;
    (void)capacity;
    (void)mount_id_probe;
    char guest_path[PATH_MAX];
    long e = copy_guest_cstr(a1, guest_path, sizeof guest_path);
    if (e < 0) return e;
    if (!guest_path[0]) return (a4 & LX_AT_EMPTY_PATH) ? -LX_EOPNOTSUPP : -LX_ENOENT;
    char host[PATH_MAX]; int hdirfd = -100;
    e = resolve_at((int)a0, guest_path, host, sizeof host, &hdirfd);
    if (e < 0) return e;
    struct stat st;
    if (fstatat(hdirfd, host, &st, (a4 & LX_AT_SYMLINK_FOLLOW) ? 0 : AT_SYMLINK_NOFOLLOW) < 0)
      return eret();
    return -LX_EOPNOTSUPP;
  }
  case 265: {                                                  // open_by_handle_at
    // No guest filesystem exports a decodable file handle. Validate the
    // bounded Linux header and mount anchor before reporting that limit;
    // never reinterpret untrusted handle bytes as a host path or descriptor.
    struct { unsigned int handle_bytes; int handle_type; } fh;
    if (!a1 || gmem_guest_export_copyin(NULL, a1, &fh, sizeof fh) != 0)
      return -LX_EFAULT;
    if (!fh.handle_bytes || fh.handle_bytes > 128 || fh.handle_type < 0)
      return -LX_EINVAL;
    int mount_fd = (int)a0;
    if (mount_fd != LX_AT_FDCWD &&
        (mount_fd < 0 || shim_fd_reserved(mount_fd) ||
         fcntl(mount_fd, F_GETFD) < 0))
      return -LX_EBADF;
    return -LX_EOPNOTSUPP;
  }
  case 266: {                                                   // clock_adjtime
    // The real-time clock shares the read-only timex query with adjtimex.
    // Other clock IDs have distinct kernel support and must not silently
    // report the Mac's real-time NTP state.
    if ((int)a0 != 0) return -LX_EINVAL; // CLOCK_REALTIME
    return linux_syscall_ext(171, a1, 0, 0, 0, 0, 0);
  }
  case 267: {                                                  // syncfs
    /* Darwin sync() may return before writeback completes and cannot report
     * Linux's per-superblock errors.  Validate the fd, but do not claim a
     * durable filesystem flush that the host API cannot provide. */
    if (fcntl((int)a0, F_GETFD) < 0) return eret();
    return -LX_ENOSYS;
  }
  case 268: return -LX_EPERM;                                   // setns
  // 269 sendmmsg is now HANDLED in net.c -- it is not optional, glibc's
  // getaddrinfo(AF_UNSPEC) sends its A and AAAA queries in one sendmmsg and treats a
  // failure as fatal. Leaving it -ENOSYS here made a Debian rootfs unable to resolve
  // any name at all. net_owns() claims it before this table is reached.
  case 270: return lx_process_vm_readv_self(a0, a1, a2, a3, a4, a5);
  case 271: return lx_process_vm_writev_self(a0, a1, a2, a3, a4, a5);
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
    if (target && target != guest_tid())
      return guest_tid_known((int)target) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    uint32_t size;
    if (gmem_guest_export_copyin(NULL, a1, &size, sizeof size) != 0)
      return -LX_EFAULT;
    if (size && size < sizeof(attr)) return -LX_EINVAL;
    if (size > 4096) return -LX_E2BIG;
    if (gmem_guest_export_copyin(NULL, a1, &attr, sizeof attr) != 0)
      return -LX_EFAULT;
    if (size > sizeof(attr)) {
      uint8_t extra[4096 - sizeof(attr)];
      if (a1 > UINT64_MAX - size ||
          gmem_guest_export_copyin(NULL, a1 + sizeof(attr), extra,
                                   size - sizeof(attr)) != 0)
        return -LX_EFAULT;
      for (uint32_t i = 0; i < size - sizeof(attr); i++)
        if (extra[i]) return -LX_E2BIG;
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
    if (target && target != guest_tid())
      return guest_tid_known((int)target) ? -LX_EOPNOTSUPP : -LX_ESRCH;
    if (a3) return -LX_EINVAL;  /* No SCHED_DEADLINE dynamic fields. */
    errno = 0;
    int nice = getpriority(PRIO_PROCESS, 0);
    if (nice == -1 && errno) return eret();
    attr.size = sizeof(attr);
    attr.nice = nice;
    // Build the entire declared v0-plus-extension result before guest copyout.
    // A later unmapped page must not leave a partially written sched_attr.
    uint8_t out[4096] = {0};
    memcpy(out, &attr, sizeof attr);
    return gmem_guest_copyout_checked(a1, out, size) == 0 ? 0 : -LX_EFAULT;
  }
  case 276: {                                                   // renameat2
    unsigned flags = (unsigned)a4;
    // Linux forbids combining NOREPLACE and EXCHANGE. WHITEOUT and unknown
    // bits cannot be reproduced by Darwin's renameatx_np. Check these before
    // any guest pathname access or host filesystem operation.
    if ((flags & ~(unsigned)(LX_RENAME_NOREPLACE | LX_RENAME_EXCHANGE)) ||
        flags == (LX_RENAME_NOREPLACE | LX_RENAME_EXCHANGE)) return -LX_EINVAL;
    char old_guest[PATH_MAX], new_guest[PATH_MAX], g_old[PATH_MAX], g_new[PATH_MAX];
    e = copy_guest_cstr(a1, old_guest, sizeof old_guest);
    if (e < 0) return e;
    e = copy_guest_cstr(a3, new_guest, sizeof new_guest);
    if (e < 0) return e;
    e  = resolve_at_nf_g((int)a0, old_guest, host,  sizeof host,  &hdirfd,  1,
                         g_old, sizeof g_old);
    if (e < 0) return e;
    e  = resolve_at_nf_g((int)a2, new_guest, host2, sizeof host2, &hdirfd2, 1,
                         g_new, sizeof g_new);
    if (e < 0) return e;
    // Landlock: same shape as renameat — same-directory renames need no REFER.
    {
      struct stat st_old, st_new;
      int old_is_dir = lstat(host, &st_old) == 0 && S_ISDIR(st_old.st_mode);
      int new_exists = lstat(host2, &st_new) == 0;
      int new_is_dir = new_exists && S_ISDIR(st_new.st_mode);
      if ((e = landlock_check_rename(g_old, g_new, old_is_dir, new_exists, new_is_dir)) < 0)
        return e;
    }
    if (flags == 0) return ok0(renameat(hdirfd, host, hdirfd2, host2));
#ifdef RENAME_EXCL
    unsigned hf = flags == LX_RENAME_NOREPLACE ? RENAME_EXCL : RENAME_SWAP;
    return ok0(renameatx_np(hdirfd, host, hdirfd2, host2, hf));
#else
    return -LX_EINVAL;
#endif
  }
  case 277: {  /* seccomp(operation, flags, args) */
    unsigned operation = (unsigned)a0, flags = (unsigned)a1;
    if (operation != LX_SECCOMP_SET_MODE_FILTER) return -LX_EINVAL;  // STRICT etc unsupported
    if (flags & ~(unsigned)LX_SECCOMP_FILTER_FLAG_TSYNC) return -LX_EINVAL;
    // TSYNC is accepted but not synchronized across already-live threads: the
    // policy is attached to the calling thread and inherited by its children,
    // which is correct for the single-threaded sandbox workers that reach here.
    if (!a2) return -LX_EFAULT;
    return do_seccomp_set_mode_filter(a2);
  }
  case 279: {                                                 // memfd_create(name, flags)
    char guest_name[250];
    e = copy_guest_cstr(a0, guest_name, sizeof guest_name);
    if (e == -LX_ENAMETOOLONG) return -LX_EINVAL; // prior name limit
    if (e < 0) return e;
    return do_memfd_create(guest_name, (unsigned)a1);
  }
  case 280: return -LX_EPERM;                                   // bpf
  case 281: {                                                   // execveat(dirfd, path, argv, envp, flags)
    unsigned flags = (unsigned)a4;
    if (flags & ~(LX_AT_EMPTY_PATH | LX_AT_SYMLINK_NOFOLLOW)) return -LX_EINVAL;
    char path[PATH_MAX];
    long e = copy_guest_cstr(a1, path, sizeof path);
    if (e < 0) return e;
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
    // A fence on the calling thread does not make the other guest threads
    // cross a Linux membarrier rendezvous. Advertise no barrier commands
    // until that cross-thread mechanism exists. QUERY itself is always
    // supported; callers can choose their normal fallback path.
    if (a1 != 0) return -LX_EINVAL;                             // flags
    if ((int)a0 == 0) return 0;                                  // CMD_QUERY
    return -LX_EINVAL;                                           // unsupported command
  }
  case 284: {                                                   // mlock2: eager-lock subset
    // Darwin mlock is eager. It cannot honor Linux MLOCK_ONFAULT's deferred
    // VMA state, so never report that request as applied. Also keep a guest
    // pointer from pinning unrelated memory in the macOS host process.
    int flags = (int)a2;
    if (flags & ~1) return -LX_EINVAL;                           // MLOCK_ONFAULT = 1
    if (!a1) return 0;
    if (flags) return -LX_EOPNOTSUPP;
    return guest_mlock_eager(a0, a1);
  }
  case 285: {                                                   // copy_file_range: regular-file subset
    int in = (int)a0, out = (int)a2;
    if ((unsigned int)a5) return -LX_EINVAL;                    // Linux requires flags=0
    if (in < 0 || out < 0 || shim_fd_reserved(in) || shim_fd_reserved(out))
      return -LX_EBADF;
    struct stat si, so;
    if (fstat(in, &si) != 0 || fstat(out, &so) != 0) return -LX_EBADF;
    if (S_ISDIR(si.st_mode) || S_ISDIR(so.st_mode)) return -LX_EISDIR;
    if (!S_ISREG(si.st_mode) || !S_ISREG(so.st_mode)) return -LX_EINVAL;
    int ifl = fcntl(in, F_GETFL), ofl = fcntl(out, F_GETFL);
    if (ifl < 0 || ofl < 0 || (ifl & O_ACCMODE) == O_WRONLY ||
        (ofl & O_ACCMODE) == O_RDONLY || (ofl & O_APPEND)) return -LX_EBADF;
    // Unlike shared bounce_copy, pread/pwrite lets an output error report
    // exactly the bytes copied without consuming extra input. Concurrent
    // mutation of either open-file offset is outside this bounded subset.
    if (a4 > 1024ul * 1024) return -LX_EOPNOTSUPP;
    if (a1 && a1 == a3) return -LX_EOPNOTSUPP;
    int64_t ipos, opos;
    if (a1) {
      if (gmem_guest_export_copyin(NULL, a1, &ipos, sizeof ipos) != 0 ||
          gmem_guest_copyout_checked(a1, &ipos, sizeof ipos) != 0) return -LX_EFAULT;
    } else {
      ipos = lseek(in, 0, SEEK_CUR);
      if (ipos < 0) return eret();
    }
    if (a3) {
      if (gmem_guest_export_copyin(NULL, a3, &opos, sizeof opos) != 0 ||
          gmem_guest_copyout_checked(a3, &opos, sizeof opos) != 0) return -LX_EFAULT;
    } else {
      opos = lseek(out, 0, SEEK_CUR);
      if (opos < 0) return eret();
    }
    if (ipos < 0 || opos < 0) return -LX_EINVAL;
    if (a4 > (unsigned long)(LLONG_MAX - ipos) ||
        a4 > (unsigned long)(LLONG_MAX - opos)) return -LX_EOVERFLOW;
    if (!a4) return 0;
    int same = si.st_dev == so.st_dev && si.st_ino == so.st_ino;
    if (same) {
      if (!a1 || !a3) return -LX_EOPNOTSUPP;
      if (a4 && ipos < opos + (int64_t)a4 && opos < ipos + (int64_t)a4)
        return -LX_EINVAL;
    }
    if (si.st_dev != so.st_dev) return -LX_EOPNOTSUPP;
    char buf[65536];
    size_t copied = 0;
    long error = 0;
    while (copied < (size_t)a4) {
      size_t want = (size_t)a4 - copied;
      if (want > sizeof buf) want = sizeof buf;
      ssize_t got = pread(in, buf, want, (off_t)(ipos + (int64_t)copied));
      if (got < 0) { error = eret(); break; }
      if (!got) break;
      size_t put = 0;
      while (put < (size_t)got) {
        ssize_t n = pwrite(out, buf + put, (size_t)got - put,
                           (off_t)(opos + (int64_t)copied + (int64_t)put));
        if (n <= 0) { error = n < 0 ? eret() : -LX_EIO; break; }
        put += (size_t)n;
      }
      copied += put;
      if (error || (size_t)got < want) break;
    }
    if (copied) {
      int64_t nexti = ipos + (int64_t)copied, nexto = opos + (int64_t)copied;
      if (a1) (void)gmem_guest_copyout_checked(a1, &nexti, sizeof nexti);
      else (void)lseek(in, (off_t)nexti, SEEK_SET);
      if (a3) (void)gmem_guest_copyout_checked(a3, &nexto, sizeof nexto);
      else (void)lseek(out, (off_t)nexto, SEEK_SET);
    }
    return copied ? (long)copied : error;
  }
  case 286: {                                                   // preadv2
    // Linux's six-argument ABI puts rwf_t flags in x5; x4 is pos_h and is
    // irrelevant on a 64-bit guest.  A nonzero flag must not silently become
    // a blocking, ordinary read (notably RWF_NOWAIT and RWF_HIPRI).
    if ((unsigned int)a5) return -LX_EOPNOTSUPP;
    int64_t pos = (int64_t)a3;
    if (pos < -1) return -LX_EINVAL;
    // Positioned reads use the already bounded, checked ARM64 preadv path.
    if (pos >= 0) return linux_syscall_ext(69, a0, a1, a2, a3, 0, 0);

    // Offset -1 consumes the open file description's current position. Read
    // into private storage so neither the iovec array nor its destinations
    // are passed as unchecked guest addresses to Darwin.
    if (a2 > 1024) return -LX_EINVAL;
    int fd = (int)a0;
    if (fd < 0) return -LX_EBADF;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || (fl & O_ACCMODE) == O_WRONLY) return -LX_EBADF;
    if (!a2) return 0;
    if (!a1) return -LX_EFAULT;
    struct iovec vec[1024];
    size_t nvec = (size_t)a2;
    if (gmem_guest_export_copyin(NULL, a1, vec, nvec * sizeof vec[0]) != 0)
      return -LX_EFAULT;
    size_t requested = 0;
    for (size_t i = 0; i < nvec; ++i) {
      if (vec[i].iov_len > SSIZE_MAX - requested) return -LX_EINVAL;
      requested += vec[i].iov_len;
    }
    size_t count = requested > 1024 * 1024 ? 1024 * 1024 : requested;
    if (!count) return 0;
    size_t admitted = 0;
    for (size_t i = 0; i < nvec && admitted < count; ++i) {
      size_t part = vec[i].iov_len;
      if (part > count - admitted) part = count - admitted;
      if (part && gmem_guest_range_writable(
              (uint64_t)(uintptr_t)vec[i].iov_base, part) != 0)
        return -LX_EFAULT;
      admitted += part;
    }
    char *buf = malloc(count);
    if (!buf) return -LX_ENOMEM;
    struct iovec host_vec = {.iov_base = buf, .iov_len = count};
    ssize_t n = readv(fd, &host_vec, 1);
    if (n < 0) { long error = eret(); free(buf); return error; }
    size_t copied = 0;
    for (size_t i = 0; i < nvec && copied < (size_t)n; ++i) {
      size_t part = vec[i].iov_len;
      if (part > (size_t)n - copied) part = (size_t)n - copied;
      if (part && gmem_guest_copyout_checked((uint64_t)(uintptr_t)vec[i].iov_base,
                                              buf + copied, part) != 0) {
        free(buf);
        return copied ? (long)copied : -LX_EFAULT;
      }
      copied += part;
    }
    free(buf);
    return (long)copied;
  }
  case 287: {                                                   // pwritev2: unflagged subset
    // On native arm64 Linux, pos_from_hilo() receives the full 64-bit offset
    // in x3; shifting the x4 high argument twice by 32 discards it.  Offset -1
    // means writev at the fd's current position (including pipes), whereas
    // pwritev(-1) fails.  Never silently ignore a per-write durability,
    // append, atomic, or nonblocking flag: the host pwritev cannot honor it.
    if (a5) return -LX_EOPNOTSUPP;
    uint64_t pos = (uint64_t)a3;
    // Native #66/#70 already stage and validate guest vectors and payloads.
    // Keep the ARM64 -1 current-position form and x3 full-width offset.
    if (pos == UINT64_MAX) return linux_syscall(66, a0, a1, a2, 0, 0, 0);
    return linux_syscall_ext(70, a0, a1, a2, pos, 0, 0);
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
  case 291: {
    if (!a4) return -LX_EFAULT;
    // The resolver and synthetic-path checks must never dereference a guest
    // pathname directly. NULL is permitted only for the AT_EMPTY_PATH form.
    char guest_path[PATH_MAX];
    const char *path = NULL;
    if (a1) {
      e = copy_guest_cstr(a1, guest_path, sizeof guest_path);
      if (e < 0) return e;
      path = guest_path;
    } else if (!(a2 & LX_AT_EMPTY_PATH)) {
      return -LX_EFAULT;
    }
    struct lx_statx out;
    e = do_statx((int)a0, path, (int)a2, (unsigned)a3, &out);
    if (e < 0) return e;
    return gmem_guest_copyout_checked(a4, &out, sizeof out) == 0 ? 0 : -LX_EFAULT;
  }
  case 292: return lx_io_pgetevents(a0, (long)a1, (long)a2, a3, a4, a5);
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
    if (a3) return -LX_EINVAL;
    // The guest siginfo ABI and sender identity are not translated here.
    // Never silently discard a requested payload.
    if (a2) return -LX_EOPNOTSUPP;
    if (a1 >= 64) return -LX_EINVAL;
    int guest_sig = (int)a1;
    int hsig = host_signo(guest_sig);
    if (hsig < 0) return -LX_EOPNOTSUPP; // no host equivalent for this Linux signal
    return ok0(pidfd_shim_send_signal((int)a0, hsig));
  }
  case 425: case 426: case 427: return -LX_ENOSYS;              // io_uring_*
  case 428: case 432: {                                       // open_tree / fsmount -> a real dir fd
    // fsmount's result is fd_reopen(mfd, O_DIRECTORY)'d by systemd's credential setup, so it must
    // be a real directory. Return the host /tmp dir fd.
    int fd = open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    // fsmount() must carry the fstype captured at fsopen() onto its NEW fd, because
    // move_mount() only ever sees this fsmount result fd (from_dfd), never the fsopen fd.
    if (nr == 432 && fd >= 0) {
      char fstype[MOUNTAPI_FSTYPE_MAX];
      if (mountapi_fd_get((int)a0, fstype, sizeof fstype) == 0)
        mountapi_fd_set(fd, fstype);
    }
    if (getenv("RDMOUNTAPI_TRACE"))
      fprintf(stderr, "[mountapi] %s(a0=%ld) -> fd=%d\n", nr == 428 ? "open_tree" : "fsmount",
              (long)a0, fd);
    return fd >= 0 ? fd : eret();
  }
  case 430: {                                                  // fsopen(fstype, flags)
    // Parse and record the filesystem type; a NULL/bad guest pointer is -EFAULT, never
    // a phantom success. The fd itself is an unconnected O_NONBLOCK DGRAM: libmount's
    // mnt_context_read_mesgs() does `while (read(fd,...) != -1)` and loops forever on EOF
    // (0); an unconnected socket makes read() return -1 at once. MEASURED: a BLOCKING
    // unconnected DGRAM read() on macOS blocks forever (does not return -1), so /usr/bin/mount
    // (tmp.mount's fsopen path) hung in read() and stalled the boot. O_NONBLOCK makes read()
    // return EAGAIN immediately so libmount's loop breaks.
    char fstype[MOUNTAPI_FSTYPE_MAX];
    e = copy_guest_cstr(a0, fstype, sizeof fstype);
    if (e < 0) return e;
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd >= 0) {
      fcntl(fd, F_SETFD, FD_CLOEXEC); fcntl(fd, F_SETFL, O_NONBLOCK);
      mountapi_fd_set(fd, fstype);
    }
    if (getenv("RDMOUNTAPI_TRACE"))
      fprintf(stderr, "[mountapi] fsopen(fstype='%s' flags=%ld) -> fd=%d\n", fstype, (long)a1, fd);
    return fd >= 0 ? fd : eret();
  }
  case 433: {                                                  // fspick(dirfd, path, flags)
    // fspick() reopens an EXISTING tree for reconfiguration and has no fstype argument.
    // Register no fstype (conservative default): a later move_mount() of the picked tree
    // keeps its existing no-op success, exactly like the synthetic API filesystems.
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd >= 0) { fcntl(fd, F_SETFD, FD_CLOEXEC); fcntl(fd, F_SETFL, O_NONBLOCK); }
    if (getenv("RDMOUNTAPI_TRACE"))
      fprintf(stderr, "[mountapi] fspick(dirfd=%ld flags=%ld) -> fd=%d\n", (long)a0, (long)a2, fd);
    return fd >= 0 ? fd : eret();
  }
  case 429: {                                                  // move_mount(from_dfd, from_path, to_dfd, to_path, flags)
    // Copy in and validate the target path: a NULL/bad guest pointer must be -EFAULT,
    // never acknowledged as a successful mount. from_path may be "" (MOVE_MOUNT_F_EMPTY_PATH).
    char to_path[PATH_MAX];
    e = copy_guest_cstr(a3, to_path, sizeof to_path);
    if (e < 0) return e;
    if (a1) {
      char from_path[PATH_MAX];
      e = copy_guest_cstr(a1, from_path, sizeof from_path);
      if (e < 0) return e;
    }
    // Refuse exactly the tmpfs destinations mount(2) refuses: /tmp and /run/user/<uid>.
    // The fstype rides on from_dfd (the fsmount() result fd). Synthetic API filesystems
    // (proc/sys/dev/run/cgroup2/...) keep their no-op success.
    char carried[MOUNTAPI_FSTYPE_MAX];
    if (mountapi_fd_get((int)a0, carried, sizeof carried) == 0 &&
        strcmp(carried, "tmpfs") == 0) {
      if (strcmp(to_path, "/tmp") == 0) {
        if (getenv("RDMOUNTAPI_TRACE"))
          fprintf(stderr, "[mountapi] move_mount(to='%s' tmpfs) -> EPERM\n", to_path);
        return -LX_EPERM;
      }
      if (strncmp(to_path, "/run/user/", 10) == 0) {
        const char *uid = to_path + 10;
        if (*uid && strspn(uid, "0123456789") == strlen(uid)) {
          if (getenv("RDMOUNTAPI_TRACE"))
            fprintf(stderr, "[mountapi] move_mount(to='%s' tmpfs) -> EPERM\n", to_path);
          return -LX_EPERM;
        }
      }
    }
    if (getenv("RDMOUNTAPI_TRACE"))
      fprintf(stderr, "[mountapi] move_mount(from_dfd=%ld to='%s' flags=%#lx) -> 0\n",
              (long)a0, to_path, a4);
    return 0;                                                  // no-op success
  }
  case 431: {                                                  // fsconfig(fd, cmd, key, value, aux)
    if (getenv("RDMOUNTAPI_TRACE"))
      fprintf(stderr, "[mountapi] fsconfig(fd=%ld cmd=%ld)\n", (long)a0, (long)a1);
    return 0;                                                  // no-op success
  }
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
        mountapi_fd_closed((int)fd);                            // drop a fsopen/fsmount fstype entry
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
    char guest_path[PATH_MAX];
    e = copy_guest_cstr(a1, guest_path, sizeof guest_path);
    if (e < 0) return e;
    // AT_EMPTY_PATH + "" means "check the file the fd refers to". macOS faccessat() has no
    // AT_EMPTY_PATH, so resolve the fd's own path (F_GETPATH) and check that. systemd's
    // pin_callout_binary() does faccessat2(fd, "", X_OK, AT_EMPTY_PATH) on the freshly opened
    // systemd-executor binary; returning ENOENT here made manager_new() fail with -ENOENT and
    // froze PID 1 before it could start getty/dbus.
    if ((int)a3 & LX_AT_EMPTY_PATH) {
      if (!guest_path[0]) {
        int fd = (int)a0;
        if (fd == LX_AT_FDCWD) return ok0(faccessat(AT_FDCWD, ".", (int)a2, AT_EACCESS));
        char hp[PATH_MAX];
        if (fcntl(fd, F_GETPATH, hp) == 0)
          return ok0(faccessat(AT_FDCWD, hp, (int)a2, 0));
        // No path (socket/pipe/memfd): the fd is a live open handle, so F_OK/X_OK/W_OK/R_OK pass.
        return 0;
      }
    }
    e = resolve_at_nf((int)a0, guest_path, host, sizeof host, &hdirfd,
                      ((int)a3 & LX_AT_SYMLINK_NOFOLLOW) != 0);
    if (e < 0) return e;
    int hf = host_at_flags((int)a3);
    if ((int)a3 & LX_AT_EACCESS) hf |= AT_EACCESS;
    return ok0(faccessat(hdirfd, host, (int)a2, hf));
  }
  case 440: {                                                   // process_madvise: bounded self MADV_NORMAL
    // Linux returns the number of bytes advised. Only NORMAL is safe to
    // acknowledge here: Shimmy does not currently model other VMA advice or
    // provide remote address-space operations.
    if (a4) return -LX_EINVAL;
    if (a2 > 1) return -LX_EOPNOTSUPP;
    struct { uint64_t base, len; } iov = {0, 0};
    if (a2 && (!a1 || gmem_guest_export_copyin(NULL, a1, &iov, sizeof iov) != 0))
      return -LX_EFAULT;
    int pfd = (int)a0;
    if (pfd < 0 || fcntl(pfd, F_GETFD) < 0) return -LX_EBADF;
    pid_t pid = pidfd_shim_pid(pfd);
    if (pid <= 0) return -LX_EBADF;
    if (pid != getpid()) return -LX_EOPNOTSUPP;
    if (kevent(pfd, NULL, 0, NULL, 0, NULL) < 0) return -LX_EOPNOTSUPP;
    if ((int)a3 != 0) return -LX_EOPNOTSUPP; // MADV_NORMAL only
    if (!a2) return 0;
    const uint64_t page = (uint64_t)getpagesize();
    if ((iov.base & (page - 1)) || iov.len > UINT64_MAX - (page - 1) ||
        iov.base > UINT64_MAX - iov.len)
      return -LX_EINVAL;
    if (!iov.len) return 0;
    if (iov.len > (uint64_t)SSIZE_MAX) return -LX_EINVAL;
    uint64_t rounded = (iov.len + page - 1) & ~(page - 1);
    if (rounded > SIZE_MAX || iov.base > UINT64_MAX - rounded)
      return -LX_EINVAL;
    if (gmem_guest_range_mapped(iov.base, (size_t)rounded) != 0)
      return -LX_ENOMEM;
    return (long)iov.len;
  }
  case 441: return -LX_ENOSYS;                                  // epoll_pwait2 (falls back)
  case 442:                                                    // mount_setattr: no-op success
    if (getenv("RDMOUNTAPI_TRACE"))
      fprintf(stderr, "[mountapi] mount_setattr(dfd=%ld path=%p flags=%#lx attr=%p size=%ld)\n",
              (long)a0, (void*)a1, a2, (void*)a3, (long)a4);
    return 0;
  case 443: return -LX_EPERM;                                   // quotactl_fd
  case 444: {                                                   // landlock_create_ruleset
    const uint64_t attr = a0, size = a1, flags = a2;
    // Version query: attr == NULL && flags == LANDLOCK_CREATE_RULESET_VERSION.
    if (attr == 0)
      return (flags == LL_CREATE_VERSION) ? (long)LL_ABI_VERSION : -LX_EINVAL;
    if (flags != 0) return -LX_EINVAL;
    // sizeof(struct landlock_ruleset_attr) == 16 (two __u64).
    if (size < 16) return -LX_EINVAL;
    struct { uint64_t handled_access_fs, handled_access_net; } ra;
    if (gmem_guest_export_copyin(NULL, attr, &ra, sizeof ra) != 0)
      return -LX_EFAULT;
    // ABI 3 is filesystem-only; a network mask would need ABI 4, which this
    // shim does not claim. pacman never sets it.
    if (ra.handled_access_net != 0) return -LX_EINVAL;
    int rfd = -1;
    long lr = landlock_create_ruleset(ra.handled_access_fs, &rfd);
    return lr < 0 ? lr : rfd;
  }
  case 445: {                                                   // landlock_add_rule
    const uint64_t ruleset_fd = a0, rule_type = a1, rule_attr = a2, flags = a3;
    if (flags != 0) return -LX_EINVAL;
    if (!rule_attr) return -LX_EFAULT;
    if (rule_type == LL_RULE_NET_PORT) return -LX_EOPNOTSUPP;   // ABI 3: no network rules
    if (rule_type != LL_RULE_PATH_BENEATH) return -LX_EINVAL;
    // struct landlock_path_beneath_attr is packed, 16 bytes:
    //   u64 allowed_access; s32 parent_fd; u32 reserved.
    struct { uint64_t allowed_access; int32_t parent_fd; uint32_t reserved; } pa;
    if (gmem_guest_export_copyin(NULL, rule_attr, &pa, sizeof pa) != 0)
      return -LX_EFAULT;
    char pdir[PATH_MAX];
    long pr = syscalls_guest_path_of_fd(pa.parent_fd, pdir, sizeof pdir);
    if (pr < 0) return pr;                                      // e.g. -EBADF
    return landlock_add_rule((int)ruleset_fd, (int)rule_type,
                             pa.allowed_access, pdir);
  }
  case 446: {                                                   // landlock_restrict_self
    if (a1 != 0) return -LX_EINVAL;                             // flags
    return landlock_restrict_self((int)a0);
  }
  case 450: {                                                   // set_mempolicy_home_node
    // Linux only updates an existing per-VMA BIND or PREFERRED_MANY policy.
    // This guest currently installs no per-VMA policy (mbind supports only
    // MPOL_DEFAULT), so a nonempty range finds no policy to update.
    const unsigned long page = 16384;                           // guest AT_PAGESZ
    if (a0 & (page - 1)) return -LX_EINVAL;
    if (a3 || a2 != 0) return -LX_EINVAL;                       // flags, virtual node
    if (a1 > ULONG_MAX - (page - 1)) return -LX_EINVAL;
    unsigned long rounded = (a1 + page - 1) & ~(page - 1);
    if (rounded > ULONG_MAX - a0) return -LX_EINVAL;
    return rounded ? -LX_ENOENT : 0;
  }
  case 459: {                                                   // lsm_get_self_attr
    // The guest has no active LSM attribute hooks. Match Linux's zero-hook
    // path, including its validation order and size=0 writeback. No label is
    // invented and a successful security-context read is never claimed.
    if ((uint32_t)a0 == 0 || !a2) return -LX_EINVAL;           // undefined attr, null size
    uint32_t available;
    if (gmem_guest_export_copyin(NULL, a2, &available, sizeof available) != 0)
      return -LX_EFAULT;
    (void)available;
    if ((uint32_t)a3) {
      if ((uint32_t)a3 != 1 || !a1) return -LX_EINVAL;          // LSM_FLAG_SINGLE only
      uint64_t id;
      if (gmem_guest_export_copyin(NULL, a1, &id, sizeof id) != 0)
        return -LX_EFAULT;
      if (!id) return -LX_EINVAL;
    }
    const uint32_t required = 0;
    if (gmem_guest_copyout_checked(a2, &required, sizeof required) != 0)
      return -LX_EFAULT;
    return -LX_EOPNOTSUPP;                                     // no getselfattr hook
  }
  case 460: {                                                   // lsm_set_self_attr
    // Linux validates and copies the entire lsm_ctx before invoking a hook.
    // No such hook exists in this guest, so a well-formed request is refused
    // rather than pretending to change a security label.
    const uint32_t size = (uint32_t)a2;
    if ((uint32_t)a3) return -LX_EINVAL;                        // reserved flags
    if (size < 32) return -LX_EINVAL;                           // sizeof(struct lsm_ctx)
    if (size > 16384) return -LX_E2BIG;                         // guest PAGE_SIZE
    unsigned char *ctx = malloc(size);
    if (!ctx) return -LX_ENOMEM;
    if (!a1 || gmem_guest_export_copyin(NULL, a1, ctx, size) != 0) {
      free(ctx);
      return -LX_EFAULT;
    }
    uint64_t len, ctx_len;
    memcpy(&len, ctx + 16, sizeof len);
    memcpy(&ctx_len, ctx + 24, sizeof ctx_len);
    free(ctx);
    if (len > size || ctx_len > UINT64_MAX - 32 || len < 32 + ctx_len)
      return -LX_EINVAL;
    return -LX_EOPNOTSUPP;                                     // no setselfattr hook
  }
  case 461: {                                                   // lsm_list_modules
    // No Linux Security Module is modeled in this guest. Linux permits an
    // empty active-module list: write its byte length (zero), return count
    // zero, and never touch ids. A null or invalid size pointer still faults.
    if ((uint32_t)a2) return -LX_EINVAL;
    uint32_t supplied;
    if (!a1 || gmem_guest_export_copyin(NULL, a1, &supplied, sizeof supplied) != 0)
      return -LX_EFAULT;
    (void)supplied;
    const uint32_t required = 0;
    if (gmem_guest_copyout_checked(a1, &required, sizeof required) != 0)
      return -LX_EFAULT;
    return 0;
  }
  case 452: {                                                   // fchmodat2(dfd, path, mode, flags)
    // Linux fchmodat2 accepts only these two AT_* bits. Passing other Linux bits through
    // host_at_flags silently ignores them, turning an invalid request into a chmod.
    if (a3 & ~(unsigned long)(LX_AT_EMPTY_PATH | LX_AT_SYMLINK_NOFOLLOW))
      return -LX_EINVAL;
    // The empty-path fast path and the resolver both need an owned snapshot:
    // neither may dereference an unchecked guest address before chmod.
    char guest_path[PATH_MAX];
    e = copy_guest_cstr(a1, guest_path, sizeof guest_path);
    if (e < 0) return e;
    // Same bound-socket relocation as fchmodat: operate on the real socket file
    // in the namespace, not a nonexistent rootfs node.
    char sock_host[PATH_MAX];
    if (guest_path[0] == '/' &&
        net_unix_socket_host_path(guest_path, sock_host, sizeof sock_host))
      return ok0(fchmodat(AT_FDCWD, sock_host, (mode_t)a2, host_at_flags((int)a3)));
    // AT_EMPTY_PATH + empty path == "chmod the dirfd itself". systemd's executor uses this while
    // setting up a service's RuntimeDirectory; the generic resolve below fails ENOENT on an empty
    // path and the executor then exits 233 (EXIT_RUNTIME_DIRECTORY), failing the unit.
    if ((a3 & LX_AT_EMPTY_PATH) && !guest_path[0]) {
      // AT_FDCWD means the guest's current directory, which need not equal the host cwd.
      if ((int)a0 == LX_AT_FDCWD) {
        e = resolve_at_nf(LX_AT_FDCWD, ".", host, sizeof host, &hdirfd, 0);
        if (e < 0) return e;
        return ok0(chmod(host, (mode_t)a2));
      }
      return ok0(fchmod((int)a0, (mode_t)a2));
    }
    e = resolve_at_nf((int)a0, guest_path, host, sizeof host, &hdirfd,
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
    // Snapshot guest text before the empty-path shortcut or resolver reads it.
    // NULL remains the AT_EMPTY_PATH fd spelling supported below.
    char guest_path[PATH_MAX];
    if (a1) {
      e = copy_guest_cstr(a1, guest_path, sizeof guest_path);
      if (e < 0) return e;
    }
    const char* path = a1 ? guest_path : NULL;
    size_t size = (size_t)a4;
    if (size > 65536) size = 65536;                              // XATTR_LIST_MAX
    // Darwin must never receive a raw guest output pointer. Keep the size
    // query as NULL/0 and publish a complete result only after host success.
    if (size && !a3) return -LX_EFAULT;
    char *staged = NULL;
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
      staged = size ? malloc(size) : NULL;
      if (size && !staged) return -LX_ENOMEM;
      r = flistxattr(fd, staged, size, 0);
    } else {
      if (!path) return -LX_EFAULT;
      if (!path[0]) return -LX_ENOENT;
      e = resolve_at_nf((int)a0, path, host, sizeof host, &hdirfd,
                        (a2 & LX_AT_SYMLINK_NOFOLLOW) != 0);
      if (e < 0) return e;
      staged = size ? malloc(size) : NULL;
      if (size && !staged) return -LX_ENOMEM;
      r = listxattr(host, staged, size,
                    (a2 & LX_AT_SYMLINK_NOFOLLOW) ? XATTR_NOFOLLOW : 0);
    }
    if (r < 0) {
      long err = errno == ERANGE && size >= 65536 ? -LX_E2BIG : eret();
      free(staged);
      return err;
    }
    long result = r;
    if (size && r > 0 && gmem_guest_copyout_checked(a3, staged, (size_t)r) != 0)
      result = -LX_EFAULT;
    free(staged);
    return result;
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
  case 471:                                                     // rseq_slice_yield
    // rseq(293) never registers a guest thread here, so Shimmy cannot grant
    // an rseq scheduler slice. Linux returns 0 when no slice was granted;
    // that is the only truthful result for this no-argument syscall.
    return 0;

  default:
    return -LX_ENOSYS;                                          // genuinely not implemented
  }
}
