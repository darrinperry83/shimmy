// Run against net.c itself: clang -arch arm64 -O2 -Wall -I src \
//   -o build/test_netlink_scope tests/test_netlink_scope.c src/net.c
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include "net.h"

// Unused dependencies of net.c in this focused socket test.
pid_t pidfd_shim_pid(int fd) { (void)fd; return -1; }
void pidfd_shim_register(int fd, pid_t pid) { (void)fd; (void)pid; }
int snd_owns_fd(int fd) { (void)fd; return 0; }
int snd_poll_revents(int fd, short events) { (void)fd; (void)events; return 0; }
const char* syscalls_get_root(void) { return "/"; }
long syscalls_guest_pid(void) { return (long)getpid(); }

static long sc(unsigned long n, unsigned long a, unsigned long b, unsigned long c,
               unsigned long d, unsigned long e) {
  return net_syscall(n, a, b, c, d, e, 0);
}

int main(void) {
  net_init();
  // No NETLINK_ROUTE dump or audit backend exists. Refuse both at creation.
  assert(sc(198, 16, 2, 0, 0, 0) == -97);   // AF_NETLINK, DGRAM, NETLINK_ROUTE
  assert(sc(198, 16, 2, 9, 0, 0) == -97);   // NETLINK_AUDIT
  assert(sc(198, 16, 2, 99, 0, 0) == -97);  // Unknown protocol
  assert(sc(198, 16, 3, 15, 0, 0) == -97);  // RAW KOBJECT also unsupported

  long fd = sc(198, 16, 2, 15, 0, 0);       // KOBJECT_UEVENT DGRAM monitor
  assert(fd >= 0);
  int buf = 8192;
  assert(sc(208, fd, 1, 8, (unsigned long)&buf, sizeof buf) == 0); // SO_RCVBUF
  const int unsupported_options[] = {3, 11, 12};
  for (size_t i = 0; i < sizeof unsupported_options / sizeof unsupported_options[0]; i++) {
    // No pktinfo, extended ACK, or strict-check behavior is implemented.
    assert(sc(208, fd, 270, unsupported_options[i], (unsigned long)&buf, sizeof buf) == -92);
  }
  close((int)fd);
  puts("PASS KOBJECT monitor subset; route/audit/raw and netlink options rejected");
  return 0;
}
