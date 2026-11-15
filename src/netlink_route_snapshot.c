#include "netlink_route_snapshot.h"

#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <netinet/in.h>
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
        if (dl->sdl_alen <= sizeof links[i].address) {
          links[i].address_length = dl->sdl_alen;
          memcpy(links[i].address, LLADDR(dl), dl->sdl_alen);
        }
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
      if (dl->sdl_alen <= sizeof out->address) {
        out->address_length = dl->sdl_alen;
        memcpy(out->address, LLADDR(dl), dl->sdl_alen);
      }
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

int rd_route_encode_link(const struct rd_route_link* link, uint32_t sequence, uint32_t port_id,
                         void* output, size_t capacity) {
  if (!link || !output || !link->index || !memchr(link->name, 0, sizeof link->name)) {
    errno = EINVAL; return -1;
  }
  size_t name_length = strlen(link->name) + 1;
  size_t name_attr = align4(sizeof(struct rd_rtattr) + name_length);
  size_t mtu_attr = link->mtu ? align4(sizeof(struct rd_rtattr) + sizeof(uint32_t)) : 0;
  if (link->address_length > sizeof link->address) { errno = EINVAL; return -1; }
  size_t address_attr = link->address_length ? align4(sizeof(struct rd_rtattr) + link->address_length) : 0;
  size_t total = sizeof(struct rd_nlmsghdr) + sizeof(struct rd_ifinfomsg) + name_attr + mtu_attr + address_attr;
  if (capacity < total) { errno = ENOSPC; return -1; }
  unsigned char* dst = output;
  memset(dst, 0, total);
  struct rd_nlmsghdr header = { (uint32_t)total, 16 /*RTM_NEWLINK*/, 2 /*NLM_F_MULTI*/,
                                sequence, port_id /*requester's netlink port*/ };
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
    offset += mtu_attr;
  }
  if (link->address_length) {
    struct rd_rtattr address = { (uint16_t)(sizeof address + link->address_length),
                                 1 /*IFLA_ADDRESS*/ };
    memcpy(dst + offset, &address, sizeof address);
    memcpy(dst + offset + sizeof address, link->address, link->address_length);
  }
  return (int)total;
}

static int prefix_bits(const unsigned char* mask, size_t length) {
  int bits = 0, saw_zero = 0;
  for (size_t i = 0; i < length; i++) for (int b = 7; b >= 0; b--) {
    int set = (mask[i] >> b) & 1;
    if (set && saw_zero) return -1;
    bits += set;
    if (!set) saw_zero = 1;
  }
  return bits;
}

int rd_route_collect_addresses(struct rd_route_address* addresses, size_t capacity,
                               unsigned family_filter) {
  if ((!addresses && capacity) || (family_filter != 0 && family_filter != 2 && family_filter != 10)) {
    errno = EINVAL; return -1;
  }
  struct ifaddrs* all = NULL;
  if (getifaddrs(&all) < 0) return -1;
  size_t count = 0;
  for (const struct ifaddrs* p = all; p; p = p->ifa_next) {
    if (!p->ifa_name || !p->ifa_addr || !p->ifa_netmask) continue;
    int host_family = p->ifa_addr->sa_family;
    unsigned family = host_family == AF_INET ? 2 : host_family == AF_INET6 ? 10 : 0;
    if (!family || (family_filter && family != family_filter)) continue;
    unsigned index = if_nametoindex(p->ifa_name);
    if (!index) continue;
    if (count == capacity) { freeifaddrs(all); errno = ENOSPC; return -1; }
    struct rd_route_address* out = &addresses[count];
    memset(out, 0, sizeof *out);
    out->index = index; out->family = (unsigned char)family;
    const unsigned char* local; const unsigned char* mask;
    size_t address_length = family == 2 ? 4 : 16;
    if (family == 2) {
      local = (const unsigned char*)&((const struct sockaddr_in*)p->ifa_addr)->sin_addr;
      mask = (const unsigned char*)&((const struct sockaddr_in*)p->ifa_netmask)->sin_addr;
    } else {
      local = (const unsigned char*)&((const struct sockaddr_in6*)p->ifa_addr)->sin6_addr;
      mask = (const unsigned char*)&((const struct sockaddr_in6*)p->ifa_netmask)->sin6_addr;
    }
    int prefix = prefix_bits(mask, address_length);
    if (prefix < 0) { freeifaddrs(all); errno = EINVAL; return -1; }
    out->prefix_length = (unsigned char)prefix;
    memcpy(out->local, local, address_length);
    memcpy(out->peer, local, address_length);
    if ((p->ifa_flags & IFF_POINTOPOINT) && p->ifa_dstaddr &&
        p->ifa_dstaddr->sa_family == host_family) {
      const unsigned char* peer = family == 2 ?
          (const unsigned char*)&((const struct sockaddr_in*)p->ifa_dstaddr)->sin_addr :
          (const unsigned char*)&((const struct sockaddr_in6*)p->ifa_dstaddr)->sin6_addr;
      memcpy(out->peer, peer, address_length);
    }
    if (p->ifa_flags & IFF_LOOPBACK) {
      out->scope = 254; // RT_SCOPE_HOST
      out->flags = 0x80; // IFA_F_PERMANENT: loopback is not DHCP/autoconfig
    }
    else if ((family == 2 && out->local[0] == 169 && out->local[1] == 254) ||
             (family == 10 && out->local[0] == 0xfe && (out->local[1] & 0xc0) == 0x80))
      out->scope = 253; // RT_SCOPE_LINK
    strncpy(out->label, p->ifa_name, sizeof out->label - 1);
    count++;
  }
  freeifaddrs(all);
  return (int)count;
}

int rd_route_encode_address(const struct rd_route_address* address, uint32_t sequence,
                            uint32_t port_id, void* output, size_t capacity) {
  if (!address || !output || !address->index ||
      (address->family != 2 && address->family != 10) ||
      address->prefix_length > (address->family == 2 ? 32 : 128) ||
      !memchr(address->label, 0, sizeof address->label)) {
    errno = EINVAL; return -1;
  }
  size_t size = address->family == 2 ? 4 : 16;
  size_t label_length = address->family == 2 ? strlen(address->label) + 1 : 0;
  size_t label_attr = label_length ? align4(sizeof(struct rd_rtattr) + label_length) : 0;
  size_t total = sizeof(struct rd_nlmsghdr) + 8 + 2 * align4(sizeof(struct rd_rtattr) + size) +
                 label_attr + align4(sizeof(struct rd_rtattr) + sizeof(uint32_t));
  if (capacity < total) { errno = ENOSPC; return -1; }
  unsigned char* dst = output; memset(dst, 0, total);
  struct rd_nlmsghdr header = { (uint32_t)total, 20 /*RTM_NEWADDR*/, 2 /*NLM_F_MULTI*/,
                                sequence, port_id };
  memcpy(dst, &header, sizeof header);
  dst[16] = address->family; dst[17] = address->prefix_length;
  dst[18] = address->flags; dst[19] = address->scope;
  uint32_t index = address->index; memcpy(dst + 20, &index, 4);
  size_t offset = 24;
  struct rd_rtattr attr = { (uint16_t)(sizeof attr + size), 1 /*IFA_ADDRESS*/ };
  memcpy(dst + offset, &attr, sizeof attr);
  memcpy(dst + offset + sizeof attr, address->peer, size);
  offset += align4(sizeof attr + size);
  attr.type = 2 /*IFA_LOCAL*/;
  memcpy(dst + offset, &attr, sizeof attr);
  memcpy(dst + offset + sizeof attr, address->local, size);
  offset += align4(sizeof attr + size);
  if (label_length) {
    attr.length = (uint16_t)(sizeof attr + label_length);
    attr.type = 3 /*IFA_LABEL*/;
    memcpy(dst + offset, &attr, sizeof attr);
    memcpy(dst + offset + sizeof attr, address->label, label_length);
    offset += label_attr;
  }
  // systemd-resolved reads the 32-bit IFA_FLAGS attribute, not only the
  // legacy 8-bit ifa_flags field. Carry only flags justified by host data.
  struct rd_rtattr flags = { sizeof flags + sizeof(uint32_t), 8 /*IFA_FLAGS*/ };
  uint32_t value = address->flags;
  memcpy(dst + offset, &flags, sizeof flags);
  memcpy(dst + offset + sizeof flags, &value, sizeof value);
  return (int)total;
}
