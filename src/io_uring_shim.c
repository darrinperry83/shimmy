// io_uring_shim.c — minimal synchronous io_uring emulation (see io_uring_shim.h).
//
// Ring layout (mirrors Linux SINGLE_MMAP, struct io_rings on LP64):
//   The SQ ring and CQ ring fields are packed into one contiguous region that
//   BOTH mmap(IORING_OFF_SQ_RING) and mmap(IORING_OFF_CQ_RING) resolve to (the
//   kernel returns ctx->rings for both). The SQE array is a separate region
//   reached by mmap(IORING_OFF_SQES). Field offsets below are the canonical
//   Linux v6.x values and are published to the guest through io_uring_params
//   sq_off/cq_off, so real liburing computes the same addresses.
//
//     offset  0: sq.head            offset  8: cq.head
//     offset  4: sq.tail            offset 12: cq.tail
//     offset 16: sq.ring_mask       offset 20: cq.ring_mask
//     offset 24: sq.ring_entries    offset 28: cq.ring_entries
//     offset 32: sq.dropped         offset 40: cq.flags
//     offset 36: sq.flags           offset 44: cq.overflow
//     offset 64: cq.cqes[cq_entries]  (cacheline-aligned)
//     offset  sq_array_off: sq.array[sq_entries]  (ALIGN(64 + cq_entries*16, 64))
//
// The whole backing file holds [rings][pad to page][sqes]. The shim keeps one
// canonical MAP_SHARED mapping; every guest mmap is a fresh MAP_SHARED alias, so
// the two views share memory and a guest munmap never tears down the canonical
// view (the same pattern drmasahi.c bo_backing_map uses for dumb buffers).
#include "io_uring_shim.h"
#include "syscalls.h"     // linux_syscall(), syscalls_signal_mask_swap/restore()
#include "gmem.h"         // gmem_guest_export_copyin / copyout_checked

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/types.h>

// Linux errno values (guest ABI; macOS values differ above ~34).
#define LX_EBADF        9
#define LX_ENOMEM       12
#define LX_EFAULT       14
#define LX_EINVAL       22
#define LX_ESPIPE       29
#define LX_EOPNOTSUPP   95

// ---------------------------------------------------------------------------
// Linux uapi structs (aarch64 LP64). Reconstructed from
// include/uapi/linux/io_uring.h and include/linux/io_uring_types.h (v6.6).
// ---------------------------------------------------------------------------
struct ioring_sqe {
  uint8_t  opcode;       // 0
  uint8_t  flags;        // 1
  uint16_t ioprio;       // 2
  int32_t  fd;           // 4
  uint64_t off;          // 8  (union off/addr2)
  uint64_t addr;         // 16
  uint32_t len;          // 24
  uint32_t rw_flags;     // 28
  uint64_t user_data;    // 32
  uint8_t  __pad[24];    // 40..64
};
_Static_assert(sizeof(struct ioring_sqe) == 64, "io_uring_sqe must be 64 bytes");
_Static_assert(offsetof(struct ioring_sqe, opcode) == 0, "sqe opcode offset");
_Static_assert(offsetof(struct ioring_sqe, fd) == 4, "sqe fd offset");
_Static_assert(offsetof(struct ioring_sqe, off) == 8, "sqe off offset");
_Static_assert(offsetof(struct ioring_sqe, addr) == 16, "sqe addr offset");
_Static_assert(offsetof(struct ioring_sqe, len) == 24, "sqe len offset");
_Static_assert(offsetof(struct ioring_sqe, rw_flags) == 28, "sqe rw_flags offset");
_Static_assert(offsetof(struct ioring_sqe, user_data) == 32, "sqe user_data offset");

struct ioring_cqe {
  uint64_t user_data;    // 0
  int32_t  res;          // 8
  uint32_t flags;        // 12
};
_Static_assert(sizeof(struct ioring_cqe) == 16, "io_uring_cqe must be 16 bytes");

struct ioring_sqring_offsets {
  uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array, resv1;
  uint64_t user_addr;
};
_Static_assert(sizeof(struct ioring_sqring_offsets) == 40, "io_sqring_offsets size");

struct ioring_cqring_offsets {
  uint32_t head, tail, ring_mask, ring_entries, overflow, cqes, flags, resv1;
  uint64_t user_addr;
};
_Static_assert(sizeof(struct ioring_cqring_offsets) == 40, "io_cqring_offsets size");

struct ioring_params {
  uint32_t sq_entries;   // 0
  uint32_t cq_entries;   // 4
  uint32_t flags;        // 8
  uint32_t sq_thread_cpu;// 12
  uint32_t sq_thread_idle;//16
  uint32_t features;     // 20
  uint32_t wq_fd;        // 24
  uint32_t resv[3];      // 28..36
  struct ioring_sqring_offsets sq_off; // 40 (40 bytes)
  struct ioring_cqring_offsets cq_off; // 80 (40 bytes)
};
_Static_assert(sizeof(struct ioring_params) == 120, "io_uring_params size");
_Static_assert(offsetof(struct ioring_params, sq_off) == 40, "sq_off offset");
_Static_assert(offsetof(struct ioring_params, cq_off) == 80, "cq_off offset");

// opcodes
#define IOP_NOP           0
#define IOP_READV         1
#define IOP_WRITEV        2
#define IOP_POLL_ADD      6
#define IOP_TIMEOUT       11
#define IOP_READ          22
#define IOP_WRITE         23

// mmap offsets
#define IORING_OFF_SQ_RING   0ULL
#define IORING_OFF_CQ_RING   0x8000000ULL
#define IORING_OFF_SQES      0x10000000ULL
#define IORING_OFF_MMAP_MASK 0xf8000000ULL

// setup/enter/feature flags
#define IORING_SETUP_CQSIZE  (1U << 3)
#define IORING_SETUP_CLAMP   (1U << 4)
#define IORING_FEAT_SINGLE_MMAP (1U << 0)
#define IORING_ENTER_GETEVENTS  (1U << 0)
#define IORING_ENTER_SQ_WAKEUP  (1U << 1)
#define IORING_ENTER_SQ_WAIT    (1U << 2)

// register opcodes (real Linux values; note FILES_UPDATE==6, BUFFERS_UPDATE==16)
#define IORING_REGISTER_BUFFERS          0
#define IORING_UNREGISTER_BUFFERS        1
#define IORING_REGISTER_FILES            2
#define IORING_UNREGISTER_FILES          3
#define IORING_REGISTER_EVENTFD          4
#define IORING_UNREGISTER_EVENTFD        5
#define IORING_REGISTER_FILES_UPDATE     6
#define IORING_REGISTER_BUFFERS_UPDATE   16

// Canonical ring field offsets (Linux v6.6 struct io_rings).
#define IO_SQ_HEAD          0
#define IO_SQ_TAIL          4
#define IO_CQ_HEAD          8
#define IO_CQ_TAIL          12
#define IO_SQ_RING_MASK     16
#define IO_CQ_RING_MASK     20
#define IO_SQ_RING_ENTRIES  24
#define IO_CQ_RING_ENTRIES  28
#define IO_SQ_DROPPED       32
#define IO_SQ_FLAGS         36
#define IO_CQ_FLAGS         40
#define IO_CQ_OVERFLOW      44
#define IO_CQ_CQES          64

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
#define IORING_MAX_RINGS 64
#define IORING_MAX_FDS   256

// One ring's backing resources, shared by every descriptor that dups the ring fd.
struct ioring_ring {
  int      in_use;
  int      backing_fd;     // unlinked temp file backing the MAP_SHARED aliases
  uint8_t *base;           // canonical mapping of the whole backing file
  uint32_t sq_entries;
  uint32_t cq_entries;
  uint32_t sq_array_off;   // byte offset of sq.array within the rings region
  size_t   rings_size;     // page-rounded SQ ring region size
  size_t   sqes_size;      // page-rounded SQE array size
  size_t   sqes_file_off;  // file offset of the SQE array (= rings_size)
  size_t   total_size;     // whole backing file size
  int      refs;           // live guest descriptors referencing this ring
};

// fd number -> ring index.
struct ioring_fd {
  int used;
  int fd;
  int ring;                // index into g_rings, or -1
};

static struct ioring_ring g_rings[IORING_MAX_RINGS];
static struct ioring_fd   g_fds[IORING_MAX_FDS];
static pthread_mutex_t    g_mu = PTHREAD_MUTEX_INITIALIZER;

static struct ioring_ring* ring_by_fd(int fd) {
  for (int i = 0; i < IORING_MAX_FDS; i++)
    if (g_fds[i].used && g_fds[i].fd == fd && g_fds[i].ring >= 0)
      return &g_rings[g_fds[i].ring];
  return NULL;
}

static uint32_t ring_load32(uint32_t *p) {
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static void ring_store32(uint32_t *p, uint32_t v) {
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static uint32_t align64(size_t v) { return (uint32_t)((v + 63) & ~(size_t)63); }
// Apple Silicon page size is 16K; the guest's mmap offset must be page-aligned
// or macOS mmap(MAP_SHARED) fails with EINVAL.
#define IORING_PAGE 16384UL
static size_t align_page(size_t v) { return (v + IORING_PAGE - 1) & ~(IORING_PAGE - 1); }
static unsigned round_pow2(unsigned v) {
  if (v == 0) return 0;
  unsigned p = 1;
  while (p < v && p <= 0x80000000u) p <<= 1;
  return p;
}

// Allocate the anonymous backing file and its canonical MAP_SHARED mapping.
// Returns 0 on success; on failure returns a negative LINUX errno.
static long backing_alloc(struct ioring_ring *r) {
  char tmpl[] = "/tmp/rdioring-XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) return -LX_ENOMEM;
  unlink(tmpl);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  if (ftruncate(fd, (off_t)r->total_size) != 0) { close(fd); return -LX_ENOMEM; }
  void *m = mmap(NULL, r->total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (m == MAP_FAILED) { close(fd); return -LX_ENOMEM; }
  memset(m, 0, r->total_size);
  r->backing_fd = fd;
  r->base = (uint8_t*)m;
  return 0;
}

static void ring_free(struct ioring_ring *r) {
  if (r->base) { munmap(r->base, r->total_size); r->base = NULL; }
  if (r->backing_fd >= 0) { close(r->backing_fd); r->backing_fd = -1; }
  r->in_use = 0;
}

// ---------------------------------------------------------------------------
// io_uring_setup
// ---------------------------------------------------------------------------
long ioring_setup(unsigned entries, uint64_t params_guest) {
  if (!params_guest) return -LX_EFAULT;

  struct ioring_params p;
  if (gmem_guest_export_copyin(NULL, params_guest, &p, sizeof p) != 0)
    return -LX_EFAULT;

  // Only the flags we can honour synchronously are accepted.
  unsigned allowed = IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP;
  if (p.flags & ~allowed) return -LX_EINVAL;
  unsigned inflags = p.flags;

  // SQ entries: 1..4096, power of two (clamped/rounded).
  unsigned sq = entries;
  if (sq == 0) sq = 1;
  if (sq > 4096) {
    if (!(p.flags & IORING_SETUP_CLAMP)) return -LX_EINVAL;
    sq = 4096;
  }
  sq = round_pow2(sq);
  if (sq > 4096) sq = 4096;
  if (sq < 1) sq = 1;

  // CQ entries: default 2x SQ; app-specified under IORING_SETUP_CQSIZE.
  unsigned cq;
  if (p.flags & IORING_SETUP_CQSIZE) {
    cq = p.cq_entries;
    if (cq == 0) cq = sq;
    cq = round_pow2(cq);
    if (cq < sq) cq = sq;
    if (cq > 8192) {
      if (!(p.flags & IORING_SETUP_CLAMP)) return -LX_EINVAL;
      cq = 8192;
    }
  } else {
    cq = 2 * sq;
  }

  // Ring geometry (canonical Linux layout).
  uint32_t sq_array_off = align64((size_t)IO_CQ_CQES + (size_t)cq * 16);
  size_t sq_ring_size   = (size_t)sq_array_off + (size_t)sq * 4;
  size_t rings_size     = align_page(sq_ring_size);
  size_t sqes_size      = align_page((size_t)sq * 64);
  size_t total          = rings_size + sqes_size;

  // Find a free ring slot + backing first, so we can fail cleanly.
  pthread_mutex_lock(&g_mu);
  int rslot = -1, fslot = -1;
  for (int i = 0; i < IORING_MAX_RINGS; i++) if (!g_rings[i].in_use) { rslot = i; break; }
  for (int i = 0; i < IORING_MAX_FDS; i++) if (!g_fds[i].used) { fslot = i; break; }
  if (rslot < 0 || fslot < 0) { pthread_mutex_unlock(&g_mu); return -LX_ENOMEM; }

  struct ioring_ring ring;
  memset(&ring, 0, sizeof ring);
  ring.sq_entries    = sq;
  ring.cq_entries    = cq;
  ring.sq_array_off  = sq_array_off;
  ring.rings_size    = rings_size;
  ring.sqes_size     = sqes_size;
  ring.sqes_file_off = rings_size;
  ring.total_size    = total;
  ring.refs          = 1;

  long e = backing_alloc(&ring);
  if (e < 0) { pthread_mutex_unlock(&g_mu); return e; }

  // Initialise the shared ring words.
  uint8_t *rings = ring.base;
  ring_store32((uint32_t*)(rings + IO_SQ_RING_MASK), sq - 1);
  ring_store32((uint32_t*)(rings + IO_SQ_RING_ENTRIES), sq);
  ring_store32((uint32_t*)(rings + IO_CQ_RING_MASK), cq - 1);
  ring_store32((uint32_t*)(rings + IO_CQ_RING_ENTRIES), cq);

  // The guest's ring fd is a CLOEXEC dup of the backing fd: a guest fd IS a
  // host fd, and keeping the shim's backing_fd separate lets the close(2) path
  // close the guest fd while ring_free() closes the backing fd without a
  // double-close (the same split drmasahi.c uses for its BO backing files).
  int guest_fd = dup(ring.backing_fd);
  if (guest_fd < 0) { ring_free(&ring); pthread_mutex_unlock(&g_mu); return -LX_ENOMEM; }
  fcntl(guest_fd, F_SETFD, FD_CLOEXEC);

  g_rings[rslot] = ring;
  g_fds[fslot].used = 1;
  g_fds[fslot].fd   = guest_fd;
  g_fds[fslot].ring = rslot;
  pthread_mutex_unlock(&g_mu);

  // Fill the guest-visible params.
  memset(&p, 0, sizeof p);
  p.sq_entries     = sq;
  p.cq_entries     = cq;
  p.flags          = inflags; // echo CLAMP/CQSIZE request bits back
  p.sq_thread_cpu  = 0;
  p.sq_thread_idle = 0;
  p.features       = IORING_FEAT_SINGLE_MMAP;
  p.wq_fd          = 0;
  p.sq_off.head        = IO_SQ_HEAD;
  p.sq_off.tail        = IO_SQ_TAIL;
  p.sq_off.ring_mask   = IO_SQ_RING_MASK;
  p.sq_off.ring_entries= IO_SQ_RING_ENTRIES;
  p.sq_off.flags       = IO_SQ_FLAGS;
  p.sq_off.dropped     = IO_SQ_DROPPED;
  p.sq_off.array       = sq_array_off;
  p.sq_off.resv1       = 0;
  p.sq_off.user_addr   = 0;
  p.cq_off.head        = IO_CQ_HEAD;
  p.cq_off.tail        = IO_CQ_TAIL;
  p.cq_off.ring_mask   = IO_CQ_RING_MASK;
  p.cq_off.ring_entries= IO_CQ_RING_ENTRIES;
  p.cq_off.overflow    = IO_CQ_OVERFLOW;
  p.cq_off.cqes        = IO_CQ_CQES;
  p.cq_off.flags       = IO_CQ_FLAGS;
  p.cq_off.resv1       = 0;
  p.cq_off.user_addr   = 0;

  if (gmem_guest_copyout_checked(params_guest, &p, sizeof p) != 0) {
    // Roll back: drop the descriptor + ring we just registered.
    pthread_mutex_lock(&g_mu);
    g_fds[fslot].used = 0;
    ring_free(&g_rings[rslot]);
    pthread_mutex_unlock(&g_mu);
    close(guest_fd);
    return -LX_EFAULT;
  }

  return guest_fd;
}

// ---------------------------------------------------------------------------
// io_uring_enter
// ---------------------------------------------------------------------------
static long ioring_execute(const struct ioring_sqe *sqe) {
  switch (sqe->opcode) {
    case IOP_NOP:
      return 0;
    case IOP_READV:
      // Synchronous fallback: no offset (sockets/pipes are the common case).
      return linux_syscall(65 /*readv*/, sqe->fd, sqe->addr, sqe->len, 0, 0, 0);
    case IOP_WRITEV:
      return linux_syscall(66 /*writev*/, sqe->fd, sqe->addr, sqe->len, 0, 0, 0);
    case IOP_READ: {
      long r = linux_syscall(67 /*pread64*/, sqe->fd, sqe->addr, sqe->len,
                             sqe->off, 0, 0);
      if (r == -LX_ESPIPE)
        r = linux_syscall(63 /*read*/, sqe->fd, sqe->addr, sqe->len, 0, 0, 0);
      return r;
    }
    case IOP_WRITE: {
      long r = linux_syscall(68 /*pwrite64*/, sqe->fd, sqe->addr, sqe->len,
                             sqe->off, 0, 0);
      if (r == -LX_ESPIPE)
        r = linux_syscall(64 /*write*/, sqe->fd, sqe->addr, sqe->len, 0, 0, 0);
      return r;
    }
    case IOP_POLL_ADD:
    case IOP_TIMEOUT:
      // No async engine: report immediate success so callers proceed.
      return 0;
    default:
      return -LX_EOPNOTSUPP;
  }
}

// Submit up to `to_submit` SQEs synchronously. Returns the number of SQEs
// completed (each produces a CQE), or a negative LINUX errno.
static long ioring_submit(struct ioring_ring *r, unsigned to_submit) {
  uint8_t *rings = r->base;
  uint8_t *sqes  = r->base + r->sqes_file_off;
  uint32_t sq_mask = r->sq_entries - 1;
  uint32_t cq_mask = r->cq_entries - 1;

  uint32_t head = ring_load32((uint32_t*)(rings + IO_SQ_HEAD));
  uint32_t tail = ring_load32((uint32_t*)(rings + IO_SQ_TAIL));

  unsigned done = 0;
  while (head != tail && done < to_submit) {
    uint32_t idx = ring_load32((uint32_t*)(rings + r->sq_array_off +
                                           (size_t)(head & sq_mask) * 4));
    head++;
    if (idx >= r->sq_entries) {
      // Invalid SQE index: Linux drops the entry and counts it as dropped.
      uint32_t dropped = ring_load32((uint32_t*)(rings + IO_SQ_DROPPED));
      ring_store32((uint32_t*)(rings + IO_SQ_DROPPED), dropped + 1);
      continue;
    }

    struct ioring_sqe sqe;
    memcpy(&sqe, sqes + (size_t)idx * 64, sizeof sqe);
    long res = ioring_execute(&sqe);

    uint32_t cq_tail = ring_load32((uint32_t*)(rings + IO_CQ_TAIL));
    struct ioring_cqe cqe;
    cqe.user_data = sqe.user_data;
    cqe.res       = (int32_t)res;
    cqe.flags     = 0;
    memcpy(rings + IO_CQ_CQES + (size_t)(cq_tail & cq_mask) * 16, &cqe, sizeof cqe);
    // Release-store the CQ tail after the CQE store so the guest sees both.
    ring_store32((uint32_t*)(rings + IO_CQ_TAIL), cq_tail + 1);
    done++;
  }

  ring_store32((uint32_t*)(rings + IO_SQ_HEAD), head);
  return (long)done;
}

long ioring_enter(unsigned fd, unsigned to_submit, unsigned min_complete,
                  unsigned flags, uint64_t sig_guest, uint64_t sigsz) {
  struct ioring_ring *r = ring_by_fd((int)fd);
  if (!r) return -LX_EBADF;

  unsigned allowed = IORING_ENTER_GETEVENTS | IORING_ENTER_SQ_WAKEUP |
                     IORING_ENTER_SQ_WAIT;
  if (flags & ~allowed) return -LX_EINVAL;

  // Apply the transient signal mask while we run the submissions, if requested.
  unsigned long long prev = 0;
  int have_sigmask = 0;
  if (sigsz != 0) {
    if (sigsz != sizeof(unsigned long long)) return -LX_EINVAL;
    if (!sig_guest) return -LX_EFAULT;
    unsigned long long mask;
    if (gmem_guest_export_copyin(NULL, sig_guest, &mask, sizeof mask) != 0)
      return -LX_EFAULT;
    prev = syscalls_signal_mask_swap(mask);
    have_sigmask = 1;
  }

  long ret = 0;
  if (to_submit) {
    ret = ioring_submit(r, to_submit);
  } else if (flags & IORING_ENTER_GETEVENTS) {
    // No async engine: nothing can complete in the background, so there are
    // never more CQEs than were returned by the submit that produced them.
    ret = 0;
  }
  // min_complete is satisfied by construction: every submitted SQE completes
  // inline. Nothing here blocks waiting for an async completion that cannot
  // arrive, which is the intended synchronous fallback behaviour.

  if (have_sigmask) syscalls_signal_mask_restore(prev);
  return ret;
}

// ---------------------------------------------------------------------------
// io_uring_register — the shim has no fixed buffers/files or eventfd wakeups,
// so every registration op is a validated no-op that returns success.
// ---------------------------------------------------------------------------
long ioring_register(unsigned fd, unsigned opcode, uint64_t arg_guest,
                     unsigned nr_args) {
  (void)arg_guest;
  if (!ring_by_fd((int)fd)) return -LX_EBADF;

  switch (opcode) {
    case IORING_REGISTER_BUFFERS:
    case IORING_UNREGISTER_BUFFERS:
    case IORING_REGISTER_FILES:
    case IORING_UNREGISTER_FILES:
    case IORING_REGISTER_EVENTFD:
    case IORING_UNREGISTER_EVENTFD:
    case IORING_REGISTER_FILES_UPDATE:
    case IORING_REGISTER_BUFFERS_UPDATE:
      (void)nr_args;
      return 0;
    default:
      return -LX_EINVAL;
  }
}

// ---------------------------------------------------------------------------
// mmap hook + fd lifecycle
// ---------------------------------------------------------------------------
int ioring_owns_fd(int fd) {
  pthread_mutex_lock(&g_mu);
  int owned = ring_by_fd(fd) != NULL;
  pthread_mutex_unlock(&g_mu);
  return owned;
}

void* ioring_mmap(int fd, long off, size_t len) {
  pthread_mutex_lock(&g_mu);
  struct ioring_ring *r = ring_by_fd(fd);
  if (!r) { pthread_mutex_unlock(&g_mu); return NULL; }

  size_t file_off;
  switch ((uint64_t)off & IORING_OFF_MMAP_MASK) {
    case IORING_OFF_SQ_RING:
    case IORING_OFF_CQ_RING:
      file_off = 0;             // both resolve to the packed SQ/CQ rings region
      break;
    case IORING_OFF_SQES:
      file_off = r->sqes_file_off;
      break;
    default:
      pthread_mutex_unlock(&g_mu);
      return NULL;
  }
  int backing = r->backing_fd;
  pthread_mutex_unlock(&g_mu);

  if (len == 0) return NULL;
  void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, backing,
                 (off_t)file_off);
  if (m == MAP_FAILED) return NULL;
  return m;
}

void ioring_register_fd_copy(int newfd, int oldfd) {
  pthread_mutex_lock(&g_mu);
  struct ioring_ring *r = ring_by_fd(oldfd);
  if (!r) { pthread_mutex_unlock(&g_mu); return; }
  // Find which ring index oldfd maps to, then register newfd to it.
  int ring = -1;
  for (int i = 0; i < IORING_MAX_FDS; i++)
    if (g_fds[i].used && g_fds[i].fd == oldfd) { ring = g_fds[i].ring; break; }
  if (ring < 0) { pthread_mutex_unlock(&g_mu); return; }
  // dup2/dup3 may overwrite an existing descriptor: retire any stale entry that
  // already carries newfd before adding the fresh mapping.
  for (int i = 0; i < IORING_MAX_FDS; i++)
    if (g_fds[i].used && g_fds[i].fd == newfd) {
      int oldring = g_fds[i].ring;
      g_fds[i].used = 0; g_fds[i].ring = -1;
      if (oldring >= 0 && g_rings[oldring].refs > 0) {
        if (--g_rings[oldring].refs == 0) ring_free(&g_rings[oldring]);
      }
    }
  int slot = -1;
  for (int i = 0; i < IORING_MAX_FDS; i++) if (!g_fds[i].used) { slot = i; break; }
  if (slot < 0) { pthread_mutex_unlock(&g_mu); return; }
  g_fds[slot].used = 1;
  g_fds[slot].fd   = newfd;
  g_fds[slot].ring = ring;
  g_rings[ring].refs++;
  pthread_mutex_unlock(&g_mu);
}

void ioring_fd_closed(int fd) {
  pthread_mutex_lock(&g_mu);
  int ring = -1;
  for (int i = 0; i < IORING_MAX_FDS; i++)
    if (g_fds[i].used && g_fds[i].fd == fd) {
      g_fds[i].used = 0;
      ring = g_fds[i].ring;
      g_fds[i].ring = -1;
      break;
    }
  if (ring < 0) { pthread_mutex_unlock(&g_mu); return; }
  if (g_rings[ring].refs > 0) g_rings[ring].refs--;
  if (g_rings[ring].refs == 0) ring_free(&g_rings[ring]);
  pthread_mutex_unlock(&g_mu);
}
