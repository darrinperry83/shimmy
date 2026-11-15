#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include "net.h"

// Standalone net.c test hooks; no guest, rootfs, or live service is launched.
pid_t pidfd_shim_pid(int fd) { (void)fd; return -1; }
void pidfd_shim_register(int fd, pid_t pid) { (void)fd; (void)pid; }
int snd_owns_fd(int fd) { (void)fd; return 0; }
int snd_poll_revents(int fd, short events) { (void)fd; (void)events; return 0; }
const char* syscalls_get_root(void) { return "/"; }
long syscalls_guest_pid(void) { return (long)getpid(); }
int shim_fd_reserve(int fd) { return fd; }
void shim_fd_unreserve(int fd) { (void)fd; }
int g_uid0;
unsigned guest_cred_uid(int which) { (void)which; return (unsigned)getuid(); }
unsigned guest_cred_gid(int which) { (void)which; return (unsigned)getgid(); }
int guest_cred_registry_init(const char* group, uint32_t uid, uint32_t gid) {
  (void)group; (void)uid; (void)gid; return 0;
}
int guest_cred_registry_lookup(pid_t pid, uint32_t* uid, uint32_t* gid) {
  (void)pid; (void)uid; (void)gid; return -1;
}

struct linux_epoll_event { uint32_t events, pad; uint64_t data; };
struct linux_timespec { int64_t sec, nsec; };
static long sc(unsigned long nr, unsigned long a, unsigned long b,
               unsigned long c, unsigned long d, unsigned long e, unsigned long f) {
  return net_syscall(nr, a, b, c, d, e, f);
}
static double ms(void) {
  struct timespec t;
  assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  return (double)t.tv_sec * 1000 + (double)t.tv_nsec / 1e6;
}

int main(void) {
  assert(net_owns_syscall(441));
  long ep = sc(20, 0, 0, 0, 0, 0, 0);
  assert(ep >= 0);
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  struct linux_epoll_event interest = { .events = 1, .data = 0x1234567890abcdefULL };
  struct linux_epoll_event out[2];
  assert(sc(21, ep, 1, pair[1], (unsigned long)&interest, 0, 0) == 0);

  struct linux_timespec zero = {0, 0};
  assert(sc(441, ep, (unsigned long)out, 2, (unsigned long)&zero, 0, 0) == 0);
  assert(sc(441, ep, (unsigned long)out, 0, (unsigned long)&zero, 0, 0) == -22);
  struct linux_timespec bad_sec = {-1, 0}, bad_nsec = {0, -1}, high_nsec = {0, 1000000000};
  assert(sc(441, ep, (unsigned long)out, 2, (unsigned long)&bad_sec, 0, 0) == -22);
  assert(sc(441, ep, (unsigned long)out, 2, (unsigned long)&bad_nsec, 0, 0) == -22);
  assert(sc(441, ep, (unsigned long)out, 2, (unsigned long)&high_nsec, 0, 0) == -22);

  uint64_t mask = 0;
  assert(sc(441, ep, (unsigned long)out, 2, (unsigned long)&zero,
            (unsigned long)&mask, 4) == -22);
  assert(sc(441, ep, (unsigned long)out, 2, (unsigned long)&zero,
            (unsigned long)&mask, 8) == -95);

  struct linux_timespec short_wait = {0, 15000000};
  double before = ms();
  assert(sc(441, ep, (unsigned long)out, 2, (unsigned long)&short_wait, 0, 0) == 0);
  double elapsed = ms() - before;
  assert(elapsed >= 5.0 && elapsed < 1000.0);

  assert(write(pair[0], "x", 1) == 1);
  assert(sc(441, ep, (unsigned long)out, 2, (unsigned long)&zero, 0, 0) == 1);
  assert(out[0].events & 1);
  assert(out[0].data == interest.data);
  assert(out[0].pad == 0);
  assert(sc(22, ep, (unsigned long)out, 2, 0, 0, 0) == 1);

  close(pair[0]); close(pair[1]); close((int)ep);
  printf("epoll_pwait2: validation, nanosecond timeout, readiness and mask handling passed (%.2f ms wait)\n", elapsed);
  return 0;
}
