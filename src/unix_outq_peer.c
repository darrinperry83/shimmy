#include "unix_outq_peer.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/proc_info.h>
#include <libproc.h>
#include <unistd.h>

// Linux AF_UNIX SIOCOUTQ uses sk_wmem_alloc, including outstanding SCM_RIGHTS
// messages until the peer receives them. Darwin's SO_NWRITE reports zero as
// soon as bytes move to the peer's receive queue. proc_pidfdinfo exposes the
// actual peer socket handle and its receive-buffer memory charge instead.
int rd_unix_peer_outq(int fd, int* queued_bytes) {
  if (!queued_bytes) { errno = EFAULT; return -1; }
  if (fcntl(fd, F_GETFD) < 0) return -1;
  struct socket_fdinfo local = {0};
  int info_bytes = proc_pidfdinfo(getpid(), fd, PROC_PIDFDSOCKETINFO, &local, sizeof local);
  if (info_bytes != sizeof local) {
    if (info_bytes >= 0) errno = EAGAIN;
    return -1;
  }
  if (local.psi.soi_family != AF_UNIX ||
      local.psi.soi_proto.pri_un.unsi_conn_so == 0) {
    errno = EOPNOTSUPP;
    return -1;
  }

  int peer_pid = 0;
  socklen_t peer_len = sizeof peer_pid;
  if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &peer_pid, &peer_len) < 0) return -1;
  if (peer_len != sizeof peer_pid || peer_pid <= 0) { errno = EAGAIN; return -1; }

  // proc_pidinfo(NULL,0) gives a sizing estimate, including slack. Cap the
  // scan so an adversarial peer cannot force unbounded allocation or work.
  int estimate = proc_pidinfo(peer_pid, PROC_PIDLISTFDS, 0, NULL, 0);
  if (estimate <= 0) { errno = EAGAIN; return -1; }
  size_t capacity = (size_t)estimate + 4096;
  if (capacity > 1024 * 1024) { errno = E2BIG; return -1; }
  struct proc_fdinfo* fds = malloc(capacity);
  if (!fds) return -1;
  int nbytes = proc_pidinfo(peer_pid, PROC_PIDLISTFDS, 0, fds, (int)capacity);
  if (nbytes < 0 || (size_t)nbytes >= capacity) {
    free(fds);
    errno = EAGAIN;
    return -1;
  }

  int result = -1;
  uint64_t peer_socket = local.psi.soi_proto.pri_un.unsi_conn_so;
  for (int i = 0; i < nbytes / (int)sizeof *fds; i++) {
    if (fds[i].proc_fdtype != PROX_FDTYPE_SOCKET) continue;
    struct socket_fdinfo remote = {0};
    if (proc_pidfdinfo(peer_pid, fds[i].proc_fd, PROC_PIDFDSOCKETINFO,
                       &remote, sizeof remote) != sizeof remote) continue;
    if (remote.psi.soi_so != peer_socket) continue;
    uint32_t charge = remote.psi.soi_rcv.sbi_mbcnt;
    if (remote.psi.soi_rcv.sbi_cc > charge) charge = remote.psi.soi_rcv.sbi_cc;
    result = charge > INT_MAX ? INT_MAX : (int)charge;
    break;
  }
  free(fds);
  if (result < 0) { errno = EAGAIN; return -1; }

  // A disconnect/reconnect during the scan must never be mistaken for an
  // empty queue. Recheck the local socket's kernel peer handle before return.
  struct socket_fdinfo after = {0};
  if (proc_pidfdinfo(getpid(), fd, PROC_PIDFDSOCKETINFO, &after, sizeof after) != sizeof after ||
      after.psi.soi_proto.pri_un.unsi_conn_so != peer_socket) {
    errno = EAGAIN;
    return -1;
  }
  *queued_bytes = result;
  return 0;
}
