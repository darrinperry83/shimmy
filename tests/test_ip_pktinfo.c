// clang -arch arm64 -O2 -Wall -I src -o build/test_ip_pktinfo \
//   tests/test_ip_pktinfo.c src/net.c src/netlink_route_transport.c src/netlink_route_snapshot.c
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/uio.h>
#include "net.h"

pid_t pidfd_shim_pid(int fd) { (void)fd; return -1; }
void pidfd_shim_register(int fd, pid_t pid) { (void)fd; (void)pid; }
int snd_owns_fd(int fd) { (void)fd; return 0; }
int snd_poll_revents(int fd, short events) { (void)fd; (void)events; return 0; }
const char* syscalls_get_root(void) { return "/"; }
long syscalls_guest_pid(void) { return (long)getpid(); }
int shim_fd_reserve(int fd) { return fd; }
void shim_fd_unreserve(int fd) { (void)fd; }
struct linux_msghdr {
  uint64_t name; uint32_t namelen, pad1; uint64_t iov, iovlen, control, controllen;
  int32_t flags; uint32_t pad2;
};
struct linux_cmsghdr { uint64_t len; int32_t level, type; };
static long sc(unsigned long n, unsigned long a, unsigned long b, unsigned long c,
               unsigned long d, unsigned long e, unsigned long f) {
  return net_syscall(n, a, b, c, d, e, f);
}
int main(void) {
  net_init();
  long receiver = sc(198, 2, 2, 0, 0, 0, 0);
  assert(receiver >= 0);
  int enabled = 1, readback = 0; uint32_t len = sizeof readback;
  assert(sc(208, receiver, 0, 8, (unsigned long)&enabled, sizeof enabled, 0) == 0);
  assert(sc(209, receiver, 0, 8, (unsigned long)&readback, (unsigned long)&len, 0) == 0);
  assert(readback == 1 && len == sizeof readback);
  assert(sc(208, receiver, 0, 8, (unsigned long)&enabled, 1, 0) < 0);
  assert(sc(208, receiver, 0, 999, (unsigned long)&enabled, sizeof enabled, 0) < 0);
  int mode = 0;
  assert(sc(208, receiver, 0, 10, (unsigned long)&mode, sizeof mode, 0) == 0);
  readback = -1; len = sizeof readback;
  assert(sc(209, receiver, 0, 10, (unsigned long)&readback, (unsigned long)&len, 0) == 0);
  assert(readback == 0 && len == sizeof readback);
  for (mode = 1; mode <= 5; mode++)
    assert(sc(208, receiver, 0, 10, (unsigned long)&mode, sizeof mode, 0) == -95);
  mode = 0;
  assert(sc(208, receiver, 0, 10, (unsigned long)&mode, 1, 0) == -22);
  assert(sc(208, receiver, 0, 10, 0, sizeof mode, 0) == -14);
  int dontfrag = 1; socklen_t hostlen = sizeof dontfrag;
  assert(setsockopt((int)receiver, IPPROTO_IP, IP_DONTFRAG, &dontfrag, hostlen) == 0);
  readback = -1; len = sizeof readback;
  assert(sc(209, receiver, 0, 10, (unsigned long)&readback, (unsigned long)&len, 0) == -95);
  assert(sc(208, receiver, 0, 10, (unsigned long)&mode, sizeof mode, 0) == 0);
  assert(getsockopt((int)receiver, IPPROTO_IP, IP_DONTFRAG, &dontfrag, &hostlen) == 0);
  assert(dontfrag == 0);
  long tcp = sc(198, 2, 1, 0, 0, 0, 0);
  assert(tcp >= 0);
  assert(sc(208, tcp, 0, 10, (unsigned long)&mode, sizeof mode, 0) == 0);
  close((int)tcp);
  unsigned char addr[16] = {2, 0, 0, 0, 127, 0, 0, 1};
  long bind_result = sc(200, receiver, (unsigned long)addr, sizeof addr, 0, 0, 0);
  if (bind_result) fprintf(stderr, "loopback bind result %ld\n", bind_result);
  assert(bind_result == 0);
  uint32_t addrlen = sizeof addr;
  assert(sc(204, receiver, (unsigned long)addr, (unsigned long)&addrlen, 0, 0, 0) == 0);
  assert(addrlen == sizeof addr && (addr[2] || addr[3]));
  long sender = sc(198, 2, 2, 0, 0, 0, 0);
  assert(sender >= 0);
  const char payload = 'x';
  assert(sc(206, sender, (unsigned long)&payload, 1, 0, (unsigned long)addr, sizeof addr) == 1);
  char received = 0; struct iovec iov = { &received, 1 };
  unsigned char control[128] = {0};
  struct linux_msghdr msg = { .iov = (uint64_t)&iov, .iovlen = 1,
                              .control = (uint64_t)control, .controllen = sizeof control };
  assert(sc(212, receiver, (unsigned long)&msg, 0, 0, 0, 0) == 1);
  assert(received == 'x' && msg.controllen >= 28);
  struct linux_cmsghdr header; memcpy(&header, control, sizeof header);
  assert(header.len == 28 && header.level == 0 && header.type == 8);
  uint32_t ifindex; memcpy(&ifindex, control + 16, sizeof ifindex);
  assert(ifindex != 0);
  assert(sc(208, receiver, 0, 12, (unsigned long)&enabled, sizeof enabled, 0) == 0);
  readback = 0; len = sizeof readback;
  assert(sc(209, receiver, 0, 12, (unsigned long)&readback, (unsigned long)&len, 0) == 0);
  assert(readback == 1 && len == sizeof readback);
  unsigned char send_control[32] = {0};
  struct linux_cmsghdr send_header = { .len = 28, .level = 0, .type = 8 };
  memcpy(send_control, &send_header, sizeof send_header);
  struct iovec send_iov = { (void*)&payload, 1 };
  struct linux_msghdr send_msg = { .name = (uint64_t)addr, .namelen = sizeof addr,
                                   .iov = (uint64_t)&send_iov, .iovlen = 1,
                                   .control = (uint64_t)send_control,
                                   .controllen = sizeof send_control };
  assert(sc(211, sender, (unsigned long)&send_msg, 0, 0, 0, 0) == 1);
  memset(control, 0, sizeof control); msg.controllen = sizeof control;
  assert(sc(212, receiver, (unsigned long)&msg, 0, 0, 0, 0) == 1);
  int saw_pktinfo = 0, saw_ttl = 0;
  for (size_t offset = 0; offset + sizeof header <= msg.controllen;) {
    memcpy(&header, control + offset, sizeof header);
    assert(header.len >= sizeof header && offset + header.len <= msg.controllen);
    if (header.level == 0 && header.type == 8) {
      assert(header.len == 28); saw_pktinfo = 1;
    }
    if (header.level == 0 && header.type == 2) {
      int32_t ttl = 0; assert(header.len == 20);
      memcpy(&ttl, control + offset + sizeof header, sizeof ttl);
      assert(ttl > 0 && ttl <= 255); saw_ttl = 1;
    }
    offset += (header.len + 7) & ~7ull;
  }
  assert(saw_pktinfo && saw_ttl);
  close((int)sender); close((int)receiver);
  puts("PASS IPv4 packet-info, TTL, and disable-DF socket behavior");
}
