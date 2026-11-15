#include "pidfd_shim.h"
#include <sys/event.h>
#include <sys/wait.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

// Linux pidfd_open(pid, flags) -> a fd that signals EPOLLIN when `pid` exits. macOS has no pidfd,
// but kqueue(2) with EVFILT_PROC|NOTE_EXIT watches exactly that, and the kqueue fd is pollable and
// can be added to another kqueue (the shim's epoll emulation) via EVFILT_READ. systemd 261's
// sd-event uses pidfd for every child source and returns -errno (no SIGCHLD fallback) when
// pidfd_open() fails, so returning -ENOSYS froze PID 1 at "Failed to acquire PID reference on
// ourselves". The pidfd registry lets waitid(P_PIDFD, ...) and pidfd_send_signal() recover the pid.
#define MAX_PIDFD 512
static struct { int fd; pid_t pid; } slots[MAX_PIDFD];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

long pidfd_open_shim(pid_t pid, unsigned int flags) {
  if (flags != 0) return -22;               // Linux pidfd_open takes no flags
  if (pid <= 0) return -3;                  // -ESRCH
  int kq = kqueue();
  if (kq < 0) return -errno;
  // The pidfd is leaked (never closed, see pidfd_shim_closed) so its NUMBER stays unique for the
  // registry. Mark it CLOEXEC so that leak does not spread: a fork+exec'd child (systemd's executor)
  // closes it on exec instead of inheriting a live pidfd for a process it doesn't own. Relocate to
  // >= 256 too: systemd's pack_fds() consolidates the service's listen fds onto 3,4,5 and spins
  // forever if a low fd is occupied.
  fcntl(kq, F_SETFD, FD_CLOEXEC);
  if (kq < 256) {
    int hi = fcntl(kq, F_DUPFD_CLOEXEC, 256);
    if (hi >= 0) { close(kq); kq = hi; fcntl(kq, F_SETFD, FD_CLOEXEC); }
  }
  struct kevent kev;
  EV_SET(&kev, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, NULL);
  if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) { int e = errno; close(kq); return -e; }
  pthread_mutex_lock(&lock);
  int slot = -1;
  for (int i = 0; i < MAX_PIDFD; i++) if (slots[i].fd == kq) { slot = i; break; }  // reuse a recycled fd's slot
  if (slot < 0)
    for (int i = 0; i < MAX_PIDFD; i++) if (slots[i].fd == 0) { slot = i; break; }
  if (slot < 0) {
    if (getenv("RDPIDFDDBG")) {
      FILE* pf = fopen("/tmp/rd_pidfd_dbg.txt", "a");
      if (pf) { fprintf(pf, "pidfd_open REGISTRY FULL (max=%d) pid=%d\n", MAX_PIDFD, pid); fclose(pf); }
    }
    pthread_mutex_unlock(&lock); close(kq); return -24;  // -EMFILE
  }
  slots[slot].fd = kq;
  slots[slot].pid = pid;
  pthread_mutex_unlock(&lock);
  if (getenv("RDPIDFDDBG")) {
    FILE* pf = fopen("/tmp/rd_pidfd_dbg.txt", "a");
    if (pf) { fprintf(pf, "pidfd_open pid=%d -> fd=%d\n", pid, kq); fclose(pf); }
  }
  return kq;
}

void pidfd_shim_closed(int fd) {
  // Deliberately a no-op: systemd's pidref lifecycle closes the pidfd before waitid(P_PIDFD, fd),
  // and if we clear the fd->pid slot here the subsequent waitid sees pid==0 and falls back to
  // waitid(P_ALL) — reaping a RANDOM child instead of the executor, which wedges the unit's state
  // transition ("Starting Coldplug" never finishes). Keep the mapping; pidfd_open_shim() overwrites
  // the slot when the fd number is recycled, which is the only moment the old mapping becomes wrong.
  (void)fd;
}

void pidfd_shim_register(int fd, pid_t pid) {
  if (fd < 0 || pid <= 0) return;
  pthread_mutex_lock(&lock);
  // Reuse an existing slot for this fd if present, else take the next free slot.
  int slot = -1;
  for (int i = 0; i < MAX_PIDFD; i++) if (slots[i].fd == fd) { slot = i; break; }
  if (slot < 0)
    for (int i = 0; i < MAX_PIDFD; i++) if (slots[i].fd == 0) { slot = i; break; }
  if (slot >= 0) { slots[slot].fd = fd; slots[slot].pid = pid; }
  pthread_mutex_unlock(&lock);
  if (getenv("RDPIDFDDBG")) {
    FILE* pf = fopen("/tmp/rd_pidfd_dbg.txt", "a");
    if (pf) { fprintf(pf, "pidfd_register fd=%d pid=%d\n", fd, pid); fclose(pf); }
  }
}

pid_t pidfd_shim_pid(int fd) {
  pid_t pid = 0;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < MAX_PIDFD; i++) if (slots[i].fd == fd) { pid = slots[i].pid; break; }
  pthread_mutex_unlock(&lock);
  return pid;
}

int pidfd_shim_waitid(int fd, id_t id, siginfo_t* si, int hopt) {
  pid_t pid = pidfd_shim_pid(fd);
  if (pid <= 0) {
    // Not a pidfd we issued. The guest may hold a stale/unset pidfd (systemd uses fd=INT_MAX as a
    // "no pidfd" sentinel in some paths). On Linux this is EBADF; emulate "wait for any child"
    // instead so pidfd verification doesn't wedge every executor spawn with EBADF.
    if (getenv("RDPIDFDDBG")) {
      FILE* pf = fopen("/tmp/rd_pidfd_dbg.txt", "a");
      if (pf) { fprintf(pf, "pidfd_waitid fd=%d -> fallback P_ALL\n", fd); fclose(pf); }
    }
    (void)id;
    return waitid(P_ALL, 0, si, hopt);
  }
  (void)id;
  return waitid(P_PID, pid, si, hopt);
}

int pidfd_shim_send_signal(int fd, int sig) {
  pid_t pid = pidfd_shim_pid(fd);
  if (pid <= 0) {
    // Stale/unset pidfd (systemd's fd=INT_MAX sentinel). A signal-0 existence probe is answered
    // optimistically: the process DOES exist, we just lost the fd->pid mapping.
    if (sig == 0) return 0;
    errno = EBADF; return -1;
  }
  return kill(pid, sig);
}
