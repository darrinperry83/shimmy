#include "netlink_route_snapshot.h"

#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

// Linux UAPI layout on arm64. Keep this independent of Darwin's routing
// socket ABI; the two protocols are not wire-compatible.
struct rd_nlmsghdr {
  uint32_t length;
  uint16_t type;
  uint16_t flags;
  uint32_t sequence;
  uint32_t pid;
};
struct rd_ifinfomsg {
  uint8_t family;
  uint8_t pad;
  uint16_t type;
  int32_t index;
  uint32_t flags;
  uint32_t change;
};
struct rd_rtattr { uint16_t length; uint16_t type; };
_Static_assert(sizeof(struct rd_nlmsghdr) == 16, "Linux nlmsghdr layout");
_Static_assert(sizeof(struct rd_ifinfomsg) == 16, "Linux ifinfomsg layout");

static size_t align4(size_t n) { return (n + 3u) & ~3u; }

int rd_route_collect_links(struct rd_route_link* links, size_t capacity) {
  if (!links && capacity) { errno = EINVAL; return -1; }
  struct ifaddrs* all = NULL;
  if (getifaddrs(&all) < 0) return -1;
  int query = socket(AF_INET, SOCK_DGRAM, 0);
  if (query < 0) { int saved = errno; freeifaddrs(all); errno = saved; return -1; }
  size_t count = 0;
  for (const struct ifaddrs* p = all; p; p = p->ifa_next) {
    if (!p->ifa_name) continue;
    unsigned index = if_nametoindex(p->ifa_name);
    if (!index) continue;
    size_t i = 0;
    for (; i < count; i++) if (links[i].index == index) break;
    if (i < count) {
      if (p->ifa_addr && p->ifa_addr->sa_family == AF_LINK) {
        const struct sockaddr_dl* dl = (const struct sockaddr_dl*)p->ifa_addr;
        links[i].linux_type = dl->sdl_type == IFT_ETHER ? 1 /*ARPHRD_ETHER*/ :
                              dl->sdl_type == IFT_LOOP ? 772 /*ARPHRD_LOOPBACK*/ :
                              65534 /*ARPHRD_NONE*/;
      }
      continue;
    }
    if (count == capacity) { close(query); freeifaddrs(all); errno = ENOSPC; return -1; }
    struct rd_route_link* out = &links[count];
    memset(out, 0, sizeof *out);
    out->index = index;
    out->linux_type = 65534; // unknown host link layer: do not claim Ethernet
    if (p->ifa_addr && p->ifa_addr->sa_family == AF_LINK) {
      const struct sockaddr_dl* dl = (const struct sockaddr_dl*)p->ifa_addr;
      out->linux_type = dl->sdl_type == IFT_ETHER ? 1 :
                        dl->sdl_type == IFT_LOOP ? 772 : 65534;
    }
    strncpy(out->name, p->ifa_name, sizeof out->name - 1);
    if (p->ifa_flags & IFF_UP) out->linux_flags |= 0x1;
    if (p->ifa_flags & IFF_LOOPBACK) out->linux_flags |= 0x8;
    if (p->ifa_flags & IFF_RUNNING) out->linux_flags |= 0x40;
    if (p->ifa_flags & IFF_MULTICAST) out->linux_flags |= 0x1000;
    struct ifreq req;
    memset(&req, 0, sizeof req);
    strncpy(req.ifr_name, out->name, sizeof req.ifr_name - 1);
    if (ioctl(query, SIOCGIFMTU, &req) == 0 && req.ifr_mtu > 0)
      out->mtu = (unsigned)req.ifr_mtu;
    count++;
  }
  close(query);
  freeifaddrs(all);
  return (int)count;
}

int rd_route_encode_link(const struct rd_route_link* link, uint32_t sequence,
                         void* output, size_t capacity) {
  if (!link || !output || !link->index || !memchr(link->name, 0, sizeof link->name)) {
    errno = EINVAL; return -1;
  }
  size_t name_length = strlen(link->name) + 1;
  size_t name_attr = align4(sizeof(struct rd_rtattr) + name_length);
  size_t mtu_attr = link->mtu ? align4(sizeof(struct rd_rtattr) + sizeof(uint32_t)) : 0;
  size_t total = sizeof(struct rd_nlmsghdr) + sizeof(struct rd_ifinfomsg) + name_attr + mtu_attr;
  if (capacity < total) { errno = ENOSPC; return -1; }
  unsigned char* dst = output;
  memset(dst, 0, total);
  struct rd_nlmsghdr header = { (uint32_t)total, 16 /*RTM_NEWLINK*/, 2 /*NLM_F_MULTI*/,
                                sequence, 0 /*kernel sender*/ };
  memcpy(dst, &header, sizeof header);
  struct rd_ifinfomsg info = { 0, 0, link->linux_type, (int32_t)link->index,
                               link->linux_flags, 0 };
  memcpy(dst + sizeof header, &info, sizeof info);
  size_t offset = sizeof header + sizeof info;
  struct rd_rtattr name = { (uint16_t)(sizeof name + name_length), 3 /*IFLA_IFNAME*/ };
  memcpy(dst + offset, &name, sizeof name);
  memcpy(dst + offset + sizeof name, link->name, name_length);
  offset += name_attr;
  if (link->mtu) {
    struct rd_rtattr mtu = { sizeof mtu + sizeof(uint32_t), 4 /*IFLA_MTU*/ };
    memcpy(dst + offset, &mtu, sizeof mtu);
    memcpy(dst + offset + sizeof mtu, &link->mtu, sizeof(uint32_t));
  }
  return (int)total;
}
