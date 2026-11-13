// syscalls.h — Linux aarch64 syscall numbers -> macOS/BSD. Core set.
#ifndef RDRIFT_SYSCALLS_H
#define RDRIFT_SYSCALLS_H
#ifdef __cplusplus
extern "C" {
#endif

// Guest filesystem root: every guest-absolute path is resolved as <root><path>.
void syscalls_set_root(const char* root);
// The guest path of the running program (for /proc/self/exe and AT_EXECFN).
void syscalls_set_exe(const char* guest_path);
// The guest cwd. execve is serviced by re-exec'ing the shim, so this must be carried across it
// explicitly or every exec'd program silently restarts at "/" — see the note in syscalls.c.
const char* syscalls_get_cwd(void);
void syscalls_set_cwd(const char* guest_cwd);
// RDUID0=1: report uid/gid 0 to the guest (the shim IS its kernel, so it can map). Opt-in — file
// operations still run as the real uid. Read once in syscalls_set_root; see syscalls.c case 174.
extern int g_uid0;
// The main guest thread's real stack extent, so /proc/self/maps can report it truthfully.
// glibc's pthread_getattr_np() finds the INITIAL thread's stack by scanning that file.
void syscalls_set_main_stack(unsigned long lo, unsigned long hi);

// THE path resolver. (dirfd, guest path) -> host path, with symlinks followed GUEST-SIDE so that
// an absolute link target restarts at the rootfs root and ".." clamps there instead of climbing
// out onto the host. Every path-taking syscall must go through this; a raw <root>+<path>
// concatenation hands the link to the host kernel, which resolves it against the HOST's "/" and
// silently serves host file contents. See the long comment on guest_resolve_links in syscalls.c.
//
// nofollow_final: leave a symlink in the LAST component alone (readlink/lstat/unlink/rename/
// mkdir/symlink targets, and open with O_NOFOLLOW). Intermediate components are ALWAYS followed.
// gabsout may be NULL; when given it receives the resolved GUEST-absolute path.
// Returns 0, or a negative Linux errno (-ELOOP, -ENAMETOOLONG, -EFAULT, -EBADF).
long syscalls_resolve(int lx_dirfd, const char* path, int nofollow_final,
                      char* hostout, size_t hcap, char* gabsout, size_t gcap);

// Dispatch one Linux syscall. Returns the raw Linux-ABI result: >=0 on success, or a
// NEGATIVE LINUX errno (e.g. -2 for ENOENT) — never -1/errno.
// Unhandled numbers must tail-call linux_syscall_ext().
long linux_syscall(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
                   unsigned long a3, unsigned long a4, unsigned long a5);

// macOS errno -> Linux errno (they differ above ~34). Shared with the other modules.
int  linux_errno_from_host(int host_errno);

// ---------------------------------------------------------------------------------------------
// SHIM-OWNED DESCRIPTORS
//
// The guest has no fd namespace of its own — a guest fd IS a host fd — so a guest that closes a
// number closes whatever the HOST has there, including descriptors the shim opened for itself.
// That is fine for close(2), where the guest names one fd it believes it owns, and catastrophic
// for close_range(2), where it names a RANGE it has never looked at: the "close everything before
// exec" idiom (glibc closefrom/posix_spawn, python subprocess, glib g_spawn, libdbus) sweeps away
// the shim's private stderr and any server socket in one call, silently.
//
// Anything the shim opens and needs to keep must be registered here. close_range() skips
// registered descriptors; nothing else is affected, and a guest closing such an fd BY NUMBER is
// still allowed through (it can only do that if it genuinely owns it).
void shim_fd_reserve(int fd);
void shim_fd_unreserve(int fd);
int  shim_fd_reserved(int fd);

#ifdef __cplusplus
}
#endif
#endif
