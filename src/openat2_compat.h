// The arm64 openat2 ABI. Keep validation separate from path opening so the
// supported subset can reuse Shimmy's existing openat device/proc/socket path.
#ifndef RDSHIM_OPENAT2_COMPAT_H
#define RDSHIM_OPENAT2_COMPAT_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct rd_open_how { uint64_t flags, mode, resolve; };

static inline long rd_openat2_decode(const void *arg, size_t size,
                                     struct rd_open_how *out) {
  if (!arg) return -14;                 // EFAULT
  if (size < sizeof *out) return -22;   // EINVAL
  if (size > 4096) return -7;           // E2BIG, bounded before reading guest memory
  memcpy(out, arg, sizeof *out);
  if (size > sizeof *out) {
    const unsigned char *tail = (const unsigned char *)arg + sizeof *out;
    for (size_t i = 0; i < size - sizeof *out; ++i)
      if (tail[i]) return -7;           // unknown future field
  }
  // openat2 cannot silently discard Linux RESOLVE_* guarantees. They need a
  // guest namespace walk with race-proof dirfd constraints, which openat lacks.
  if (out->resolve) return -95;         // EOPNOTSUPP
  // Flags with no faithful Darwin translation are rejected, never ignored.
  // The remaining set is exactly what open_flags_l2m() understands. O_LARGEFILE
  // is harmless on the shim's 64-bit file offsets; O_SYNC contains O_DSYNC.
  const uint64_t accepted = 0x3 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 |
      0x800 | 0x1000 | 0x2000 | 0x4000 | 0x8000 | 0x20000 |
      0x80000 | 0x100000 | 0x200000;
  const uint64_t known_but_unsupported = 0x10000 | 0x40000 | 0x410000;
  if (out->flags & known_but_unsupported) return -95;
  if (out->flags & ~accepted) return -22;
  if (out->mode & ~07777ULL) return -22;
  if (out->mode && !(out->flags & 0x40)) return -22; // mode needs O_CREAT
  if ((out->flags & 0x200000) &&
      (out->flags & ~(uint64_t)(0x200000 | 0x80000 | 0x4000 | 0x8000)))
    return -22;                         // Linux O_PATH flag restrictions
  return 0;
}
#endif
