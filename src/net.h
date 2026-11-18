// net.h — Linux sockets / poll / epoll / pipe / eventfd -> macOS.
//
// Owns the syscalls an X11 or Wayland client needs to talk to our in-process server over a
// unix socket, so it must be routed BEFORE linux_syscall() in the trap handler.
#ifndef RDRIFT_NET_H
#define RDRIFT_NET_H
#include <stddef.h>
#include <sys/stat.h>
#ifdef __cplusplus
extern "C" {
#endif

void net_init(void);

// Guest unix-socket paths are relocated under this host prefix, so the guest's
// /tmp/.X11-unix/X0 becomes <prefix>/tmp/.X11-unix/X0 on the host. Abstract sockets
// (sun_path[0]==0) are hex-encoded into a directory under the prefix.
void net_set_unix_prefix(const char* host_prefix);

// Called by the spawn path with the pid of a child that has just been created and will inherit
// this process's non-CLOEXEC fds. See net.c: macOS cannot report the peer of a socketpair, so the
// shim has to remember which process it handed the other end to.
void net_note_spawned_child(int pid);

// Return a guest-visible reference token for a pathname socket in the guest
// namespace. NET_OPATH_NOT_BOUND means no socket at the path; other negative
// values are Linux errnos. Do not reserve the fd:
// unlike shim-owned transport fds it belongs to the guest and may be inherited.
#define NET_OPATH_NOT_BOUND (-4096L)
long net_open_bound_socket_opath(const char* guest_path, int cloexec);
// Map a guest-absolute path to its relocated host path if (and only if) it is a
// currently-bound AF_UNIX pathname socket in the guest namespace. Returns 1 and
// fills host_out on success, 0 if the path is not a bound socket (caller falls back
// to the ordinary rootfs resolver). Never fabricates a mapping for a nonexistent path.
int net_unix_socket_host_path(const char* guest_path, char* host_out, size_t host_cap);
// Path metadata for a reference token (1=found, 0=ordinary fd).
int net_opath_socket_stat(int fd, struct stat* out);
int net_opath_socket_is(int fd);
// Resolve a /proc/self/fd/<N> socket reference to the private hard link that
// survives unlink of the original pathname. 1=ours, 0=not an O_PATH token.
int net_opath_socket_host_path(int fd, char* out, size_t out_size);
void net_fd_closed(int fd);

// True for the syscall numbers this module implements. Must cover at least:
//   19 eventfd2, 20/21/22 epoll_create1/epoll_ctl/epoll_pwait, 441 epoll_pwait2,
//   59 pipe2, 72 pselect6, 73 ppoll,
//   198..207 socket, socketpair, bind, listen, accept, connect, getsockname,
//             getpeername, sendto, recvfrom,
//   208..212 setsockopt, getsockopt, shutdown, sendmsg, recvmsg,
//   242 accept4
// (Return 0 for anything it does not implement so it falls through to linux_syscall.)
int  net_owns_syscall(unsigned long nr);

// Negative LINUX errno on failure, like linux_syscall().
long net_syscall(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
                 unsigned long a3, unsigned long a4, unsigned long a5);

#ifdef __cplusplus
}
#endif
#endif
