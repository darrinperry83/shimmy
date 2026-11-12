#include "signalfd_shim.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <stdatomic.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

// Linux signalfd_siginfo is exactly 128 bytes. Weston consumes ssi_signo;
// the PID, UID, and child status are filled where Darwin supplies them.
struct linux_sigfd_info {
  uint32_t signo; int32_t err, code; uint32_t pid, uid;
  int32_t fd; uint32_t tid, band, overrun, trapno; int32_t status, integer;
  uint64_t ptr, utime, stime, addr; uint16_t addr_lsb;
  uint8_t pad[46];
};
_Static_assert(sizeof(struct linux_sigfd_info) == 128, "Linux signalfd record size");

#define MAX_SIGFD 16
#define L_NONBLOCK 0x800
#define L_CLOEXEC 0x80000
static struct { _Atomic(int) read_fd, write_fd; _Atomic(uint64_t) mask; } slots[MAX_SIGFD];
static _Atomic(int) count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct sigaction previous[NSIG];
static int installed[NSIG];

static int host_signal(int lx) {
  switch (lx) {
    case 1: return SIGHUP; case 2: return SIGINT; case 3: return SIGQUIT;
    case 12: return SIGUSR2; case 15: return SIGTERM; case 17: return SIGCHLD;
    default: return -1;
  }
}
static int linux_signal(int host) {
  switch (host) {
    case SIGHUP: return 1; case SIGINT: return 2; case SIGQUIT: return 3;
    case SIGUSR2: return 12; case SIGTERM: return 15; case SIGCHLD: return 17;
    default: return -1;
  }
}
static void delivered(int host, siginfo_t* info, void* context) {
  (void)context;
  int lx = linux_signal(host);
  if (lx < 0) return;
  uint64_t bit = 1ULL << (lx - 1);
  int n = atomic_load_explicit(&count, memory_order_acquire);
  for (int i = 0; i < n; i++) {
    if (!(atomic_load_explicit(&slots[i].mask, memory_order_relaxed) & bit)) continue;
    int fd = atomic_load_explicit(&slots[i].write_fd, memory_order_relaxed);
    if (fd < 0) continue;
    struct linux_sigfd_info rec = {0};
    rec.signo = (uint32_t)lx;
    if (info) {
      rec.pid = (uint32_t)info->si_pid;
      rec.uid = (uint32_t)info->si_uid;
      if (host == SIGCHLD) rec.status = info->si_status;
    }
    // Datagram write is atomic and nonblocking; no libc allocation or lock in
    // the handler. Standard signals can coalesce, as they do on Linux.
    (void)write(fd, &rec, sizeof rec);
    break; // a pending signal is consumed by one signalfd, not broadcast
  }
}
static int supported(uint64_t mask) {
  for (int lx = 1; lx <= 64; lx++)
    if ((mask & (1ULL << (lx - 1))) && host_signal(lx) < 0) return 0;
  return 1;
}
static int install_for(uint64_t mask) {
  for (int lx = 1; lx <= 64; lx++) {
    if (!(mask & (1ULL << (lx - 1)))) continue;
    int sig = host_signal(lx);
    if (sig < 0) return -22;
    if (installed[sig]) continue;
    struct sigaction old, action = {0};
    if (sigaction(sig, NULL, &old) < 0) return -errno;
    // Do not hijack any signal already owned by the host shim or another library.
    if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) return -22;
    action.sa_sigaction = delivered;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(sig, &action, &old) < 0) return -errno;
    previous[sig] = old;
    installed[sig] = 1;
  }
  return 0;
}
static void restore_unused(void) {
  int n = atomic_load(&count);
  for (int lx = 1; lx <= 64; lx++) {
    int sig = host_signal(lx);
    if (sig < 0 || !installed[sig]) continue;
    uint64_t bit = 1ULL << (lx - 1);
    int used = 0;
    for (int i = 0; i < n; i++)
      if (atomic_load(&slots[i].mask) & bit) { used = 1; break; }
    if (!used) { sigaction(sig, &previous[sig], NULL); installed[sig] = 0; }
  }
}
long signalfd4_shim(int fd, const uint64_t* mask, size_t size, int flags) {
  if (!mask) return -14;
  if (size != 8 || (flags & ~(L_NONBLOCK | L_CLOEXEC)) || !supported(*mask)) return -22;
  pthread_mutex_lock(&lock);
  int n = atomic_load_explicit(&count, memory_order_relaxed);
  if (fd >= 0) {
    for (int i = 0; i < n; i++) if (atomic_load(&slots[i].read_fd) == fd) {
      int rc = install_for(*mask);
      if (!rc) { atomic_store(&slots[i].mask, *mask); restore_unused(); }
      pthread_mutex_unlock(&lock);
      return rc ? rc : fd;
    }
    pthread_mutex_unlock(&lock); return -9;
  }
  if (fd != -1) { pthread_mutex_unlock(&lock); return -22; }
  int index = n;
  for (int i = 0; i < n; i++) if (atomic_load(&slots[i].read_fd) == -1) { index = i; break; }
  if (index >= MAX_SIGFD) { pthread_mutex_unlock(&lock); return -22; }
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) < 0) {
    int rc = -errno; pthread_mutex_unlock(&lock); return rc;
  }
  fcntl(pair[1], F_SETFL, O_NONBLOCK);
  if (flags & L_NONBLOCK) fcntl(pair[0], F_SETFL, O_NONBLOCK);
  if (flags & L_CLOEXEC) {
    fcntl(pair[0], F_SETFD, FD_CLOEXEC);
    fcntl(pair[1], F_SETFD, FD_CLOEXEC);
  }
  int rc = install_for(*mask);
  if (rc) { close(pair[0]); close(pair[1]); pthread_mutex_unlock(&lock); return rc; }
  atomic_store(&slots[index].read_fd, pair[0]);
  atomic_store(&slots[index].write_fd, pair[1]);
  atomic_store(&slots[index].mask, *mask);
  if (index == n) atomic_store_explicit(&count, n + 1, memory_order_release);
  pthread_mutex_unlock(&lock);
  return pair[0];
}
void signalfd_shim_closed(int fd) {
  pthread_mutex_lock(&lock);
  int n = atomic_load(&count);
  for (int i = 0; i < n; i++) if (atomic_load(&slots[i].read_fd) == fd) {
    atomic_store(&slots[i].mask, 0);
    atomic_store(&slots[i].read_fd, -1);
    int writer = atomic_exchange(&slots[i].write_fd, -1);
    if (writer >= 0) close(writer);
    restore_unused();
    break;
  }
  pthread_mutex_unlock(&lock);
}
