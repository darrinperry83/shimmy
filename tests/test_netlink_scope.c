// Run against net.c itself: clang -arch arm64 -O2 -Wall -I src \
//   -o build/test_netlink_scope tests/test_netlink_scope.c src/net.c \
//   src/netlink_route_transport.c src/netlink_route_snapshot.c
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include "net.h"
#include "netlink_route_transport.h"

// Unused dependencies of net.c in this focused socket test.
pid_t pidfd_shim_pid(int fd) { (void)fd; return -1; }
void pidfd_shim_register(int fd, pid_t pid) { (void)fd; (void)pid; }
int snd_owns_fd(int fd) { (void)fd; return 0; }
int snd_poll_revents(int fd, short events) { (void)fd; (void)events; return 0; }
const char* syscalls_get_root(void) { return "/"; }
long syscalls_guest_pid(void) { return (long)getpid(); }
void net_fd_closed(int fd);
int shim_fd_reserve(int fd) { return fd; }
void shim_fd_unreserve(int fd) { (void)fd; }
struct linux_msghdr {
  uint64_t name; uint32_t namelen, pad1; uint64_t iov, iovlen, control, controllen;
  int32_t flags; uint32_t pad2;
};

static long sc(unsigned long n, unsigned long a, unsigned long b, unsigned long c,
               unsigned long d, unsigned long e) {
  return net_syscall(n, a, b, c, d, e, 0);
}

int main(void) {
  net_init();
  // No NETLINK_ROUTE dump or audit backend exists. Refuse both at creation.
  assert(sc(198, 16, 2, 0, 0, 0) == -95);   // ROUTE without CLOEXEC cannot survive re-exec
  assert(sc(198, 16, 2, 9, 0, 0) == -97);   // NETLINK_AUDIT
  assert(sc(198, 16, 2, 99, 0, 0) == -97);  // Unknown protocol
  long raw_kobject = sc(198, 16, 3, 15, 0, 0); // udev's RAW KOBJECT monitor
  assert(raw_kobject >= 0);
  close((int)raw_kobject);

  long fd = sc(198, 16, 2, 15, 0, 0);       // KOBJECT_UEVENT DGRAM monitor
  assert(fd >= 0);
  int buf = 8192;
  assert(sc(208, fd, 1, 8, (unsigned long)&buf, sizeof buf) == 0); // SO_RCVBUF
  const int unsupported_options[] = {3, 11, 12};
  for (size_t i = 0; i < sizeof unsupported_options / sizeof unsupported_options[0]; i++) {
    // KOBJECT has no pktinfo, extended ACK, or strict-check behavior.
    assert(sc(208, fd, 270, unsupported_options[i], (unsigned long)&buf, sizeof buf) == -92);
  }
  close((int)fd);
  long route = sc(198, 16, 3 | 0x80000, 0, 0, 0); // SOCK_RAW|CLOEXEC as iproute2 uses
  assert(route >= 0);
  int type = 0; uint32_t type_len = sizeof type;
  assert(sc(209, route, 1, 3, (unsigned long)&type, (unsigned long)&type_len) == 0);
  assert(type == 3 && type_len == sizeof type);
  int domain = 0; uint32_t domain_len = sizeof domain;
  assert(sc(209, route, 1, 39, (unsigned long)&domain, (unsigned long)&domain_len) == 0);
  assert(domain == 16 && domain_len == sizeof domain);
  int pktinfo = 1; uint32_t pktinfo_len = sizeof pktinfo;
  assert(sc(208, route, 270, 3, (unsigned long)&pktinfo, sizeof pktinfo) == 0);
  pktinfo = 0;
  assert(sc(209, route, 270, 3, (unsigned long)&pktinfo, (unsigned long)&pktinfo_len) == 0);
  assert(pktinfo == 1 && pktinfo_len == sizeof pktinfo);
  assert(sc(208, route, 270, 3, (unsigned long)&pktinfo, 1) == -22);
  uint32_t groups_len = 0;
  assert(sc(209, route, 270, 9, 0, (unsigned long)&groups_len) == 0);
  assert(groups_len == 0);
  assert(sc(209, route, 270, 9, 0, 0) == -14);
  int unsupported_group = 7; // route updates have no event source yet
  assert(sc(208, route, 270, 1, (unsigned long)&unsupported_group, sizeof unsupported_group) == -95);
  for (int option = 11; option <= 12; option++)
    assert(sc(208, route, 270, option, (unsigned long)&domain, sizeof domain) == -92);
  unsigned char req[32] = {0};
  uint32_t nlen = 32, sequence = 1234; uint16_t getlink = 18, dump = 0x301;
  memcpy(req, &nlen, 4); memcpy(req + 4, &getlink, 2);
  memcpy(req + 6, &dump, 2); memcpy(req + 8, &sequence, 4);
  assert(sc(206, route, (unsigned long)req, sizeof req, 0, 0) == 32); // sendto
  struct linux_msghdr length_probe = {0};
  long datagram_length = sc(212, route, (unsigned long)&length_probe, 0x22,
                            0, 0); // MSG_PEEK|MSG_TRUNC, no iovec
  assert(datagram_length > 32);
  unsigned char reply[32768];
  long received = sc(207, route, (unsigned long)reply, sizeof reply, 0, 0); // recvfrom
  assert(received == datagram_length);
  uint16_t first_type, final_type;
  memcpy(&first_type, reply + 4, 2);
  memcpy(&final_type, reply + received - 16, 2);
  assert(first_type == 16 && final_type == 3); // NEWLINK ... NLMSG_DONE
  assert(sc(206, route, (unsigned long)req, 31, 0, 0) == -22);
  struct iovec send_iov = { req, sizeof req }, recv_iov = { reply, sizeof reply };
  unsigned char control[64] = {0};
  unsigned char address[12] = {0}; uint16_t family = 16;
  memcpy(address, &family, 2);
  struct linux_msghdr send_msg = { .name = (uint64_t)address, .namelen = sizeof address,
                                    .iov = (uint64_t)&send_iov, .iovlen = 1 };
  assert(sc(211, route, (unsigned long)&send_msg, 0, 0, 0) == 32);
  struct linux_msghdr recv_msg = { .name = (uint64_t)address, .namelen = sizeof address,
                                    .iov = (uint64_t)&recv_iov, .iovlen = 1,
                                    .control = (uint64_t)control, .controllen = sizeof control };
  memset(address, 0, sizeof address);
  assert(sc(212, route, (unsigned long)&recv_msg, 0, 0, 0) > 32);
  memcpy(&family, address, 2);
  assert(family == 16 && recv_msg.namelen == 12 && recv_msg.controllen == 0);
  assert(sc(207, route, (unsigned long)reply, sizeof reply, 0x40, 0) == -11);
  address[8] = 1; // multicast groups cannot be supplied
  assert(sc(200, route, (unsigned long)address, sizeof address, 0, 0) == -95);
  if (getenv("RD_ROUTE_EVENTS_TEST")) {
    int link_group = 1;
    assert(sc(208, route, 270, 1, (unsigned long)&link_group, sizeof link_group) == 0);
    struct rd_route_link links[128]; struct rd_route_address addresses[128];
    int lc = rd_route_collect_links(links, 128);
    int ac = rd_route_collect_addresses(addresses, 128, 0);
    assert(lc > 0 && ac >= 0);
    assert(rd_rtnl_reconcile_snapshot(links, (size_t)lc, addresses, (size_t)ac) == 0);
    while (sc(207, route, (unsigned long)reply, sizeof reply, 0x40, 0) > 0) {}
    links[0].mtu++;
    assert(rd_rtnl_reconcile_snapshot(links, (size_t)lc, addresses, (size_t)ac) == 0);
    memset(address, 0, sizeof address);
    uint8_t cmsg[32] = {0};
    recv_msg.namelen = sizeof address;
    recv_msg.control = (uint64_t)cmsg; recv_msg.controllen = sizeof cmsg;
    assert(sc(212, route, (unsigned long)&recv_msg, 0, 0, 0) > 24);
    uint16_t kind; uint32_t mcast_mask;
    memcpy(&kind, reply + 4, 2); memcpy(&mcast_mask, address + 8, 4);
    assert(kind == 16 && mcast_mask == 1 && recv_msg.controllen == 24);
    uint64_t cmsg_length; int32_t cmsg_level, cmsg_type; uint32_t cmsg_group;
    memcpy(&cmsg_length, cmsg, 8); memcpy(&cmsg_level, cmsg + 8, 4);
    memcpy(&cmsg_type, cmsg + 12, 4); memcpy(&cmsg_group, cmsg + 16, 4);
    assert(cmsg_length == 20 && cmsg_level == 270 && cmsg_type == 3 && cmsg_group == 1);
    assert(sc(208, route, 270, 2, (unsigned long)&link_group, sizeof link_group) == 0);
  }
  net_fd_closed((int)route); close((int)route);
  puts("PASS KOBJECT monitor and bounded ROUTE GETLINK path");
  return 0;
}
