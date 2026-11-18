// landlock.h — Linux Landlock filesystem-sandbox emulation.
//
// Landlock (syscalls 444/445/446) lets a Linux process restrict its OWN future
// filesystem accesses to a set of "path-beneath" rules. pacman 7 uses this to
// confine its download worker: a read-only "/" rule plus a read-write rule for
// the package cache directory.
//
// The shim enforces this HONESTLY. After landlock_restrict_self commits a
// ruleset, the guest path of every access-granting / mutating filesystem syscall
// (open/openat/openat2, mkdir, unlink, link, rename, truncate, mknod, symlink,
// execve) is checked against the active domain, and denied accesses fail with
// -EACCES. There is no "return success and hope" path: a right that is handled
// by a committed ruleset and not granted by any matching path-beneath rule is
// refused. See landlock_check() for the exact intersection semantics.
#ifndef RDRIFT_LANDLOCK_H
#define RDRIFT_LANDLOCK_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Linux UAPI (linux/landlock.h). aarch64 values, exact.
#define LL_FS_EXECUTE      (1ULL << 0)
#define LL_FS_WRITE_FILE   (1ULL << 1)
#define LL_FS_READ_FILE    (1ULL << 2)
#define LL_FS_READ_DIR     (1ULL << 3)
#define LL_FS_REMOVE_DIR   (1ULL << 4)
#define LL_FS_REMOVE_FILE  (1ULL << 5)
#define LL_FS_MAKE_CHAR    (1ULL << 6)
#define LL_FS_MAKE_DIR     (1ULL << 7)
#define LL_FS_MAKE_REG     (1ULL << 8)
#define LL_FS_MAKE_SOCK    (1ULL << 9)
#define LL_FS_MAKE_FIFO    (1ULL << 10)
#define LL_FS_MAKE_BLOCK   (1ULL << 11)
#define LL_FS_MAKE_SYM     (1ULL << 12)
#define LL_FS_REFER        (1ULL << 13)
#define LL_FS_TRUNCATE     (1ULL << 14)
#define LL_FS_ALL          ((1ULL << 15) - 1ULL)

#define LL_RULE_PATH_BENEATH 1
#define LL_RULE_NET_PORT     2
#define LL_CREATE_VERSION    1
#define LL_ABI_VERSION       3   // filesystem-only ABI; pacman handles <4 by dropping TRUNCATE

// landlock_create_ruleset(attr, size, flags) core: allocate a ruleset with the
// given handled_access_fs and hand back an anonymous fd (a real host fd). On
// success *out_fd >= 0 and 0 is returned; otherwise a negative Linux errno.
long landlock_create_ruleset(uint64_t handled_access_fs, int *out_fd);

// Append a path-beneath rule. `parent_path` is the canonical guest-absolute path
// of the directory referenced by parent_fd (already resolved by the caller via
// syscalls_guest_path_of_fd). Returns 0 or a negative Linux errno.
long landlock_add_rule(int ruleset_fd, int rule_type, uint64_t allowed_access,
                       const char *parent_path);

// Commit the ruleset as this process's active domain. Returns 0 or -errno.
long landlock_restrict_self(int ruleset_fd);

// Enforce: `path` is a canonical guest-absolute path (as produced by the shared
// resolver). `access` is the set of LL_FS_* rights the syscall requests. Returns
// 0 when the access is permitted and -EACCES when a committed ruleset handles
// one of the requested rights but grants it nowhere on this path. Returns 0
// immediately when no domain is active or access is empty.
long landlock_check(const char *path, uint64_t access);

// 1 when `fd` is a live ruleset fd (for close/dup bookkeeping).
int landlock_ruleset_fd(int fd);
// Drop any ruleset registry entry for `fd` (called from close()).
void landlock_fd_closed(int fd);

// Is a domain currently committed? (diagnostics / carry decisions)
int landlock_domain_active(void);

// Serialize the committed domain to an anonymous fd (pipe read end), or -1.
// Mirrors shim_seccomp_state_export_fd: the re-exec'ing shim passes the fd to
// its replacement through an RDLANDLOCK_FD=<n> host env var.
int landlock_domain_export_fd(void);
// Rebuild the committed domain from such a fd. Returns 0 on success, or a
// negative Linux errno. The caller closes the fd.
long landlock_domain_import_fd(int fd);

#ifdef __cplusplus
}
#endif
#endif
