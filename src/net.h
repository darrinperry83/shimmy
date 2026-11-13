// net.h — Linux sockets / poll / epoll / pipe / eventfd -> macOS.
//
// Owns the syscalls an X11 or Wayland client needs to talk to our in-process server over a
// unix socket, so it must be routed BEFORE linux_syscall() in the trap handler.
#ifndef RDRIFT_NET_H
#define RDRIFT_NET_H
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

// True for the syscall numbers this module implements. Must cover at least:
//   19 eventfd2, 20/21/22 epoll_create1/epoll_ctl/epoll_pwait, 23/24 dup/dup3(if desired),
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
