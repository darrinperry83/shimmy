#ifndef RD_PIDFD_SHIM_H
#define RD_PIDFD_SHIM_H
#include <sys/types.h>
#include <signal.h>
#include <stdint.h>
// pidfd emulation: a kqueue fd that becomes readable when the target (host) process exits,
// matching Linux pidfd semantics closely enough for systemd's sd-event child sources.
long pidfd_open_shim(pid_t pid, unsigned int flags);
// Start this host Shimmy process's private receiver for target-owned pidfd
// peers before entering guest code. Harmless when RDSOCKETGROUP is unset.
int pidfd_shim_target_init(void);
void pidfd_shim_closed(int fd);
// Look up the pid behind a pidfd, or 0 if unknown.
pid_t pidfd_shim_pid(int fd);
/* For an outgoing SCM_RIGHTS fd, return a target-owned, transferable pidfd;
 * return 0 for an ordinary fd, or a negative errno on conversion failure.
 * The caller owns a positive returned fd. */
int pidfd_shim_transferable(int fd);
// Register an externally-received pidfd (SCM_RIGHTS across exec'd shim processes) so fd -> pid
// lookups keep working on the receiving side. systemd's executor sends the service pidfd to PID 1
// over a socketpair; without this the manager cannot resolve the transferred fd to a pid.
void pidfd_shim_register(int fd, pid_t pid);
// Reap the process behind a pidfd. Returns the same shape as host waitid (0 / -1 errno).
int  pidfd_shim_waitid(int fd, id_t id, siginfo_t* si, int hopt);
// Signal the process behind a pidfd (Linux pidfd_send_signal).
int  pidfd_shim_send_signal(int fd, int sig);

// Linux struct pidfd_info (kernel 6.13+ PIDFD_GET_INFO), reconstructed so the shim can answer the
// ioctl systemd's pidfd_get_ppid()/pidfd_get_pid() issue when pidfs is unavailable.
struct rd_pidfd_info {
  uint64_t mask;
  uint32_t pid, ppid;
  uint32_t ruid, rgid, euid, egid, suid, sgid, fsuid, fsgid;
  uint32_t spare0[1];
  uint64_t cgroupid;
  uint64_t spare1[7];
};
#define RD_PIDFD_INFO_PID       (1ULL << 0)
#define RD_PIDFD_INFO_CREDS     (1ULL << 1)
#define RD_PIDFD_INFO_CGROUPID  (1ULL << 2)
#endif
