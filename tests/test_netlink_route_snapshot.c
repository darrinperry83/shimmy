#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <net/if.h>
#include <net/if_media.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include "netlink_route_snapshot.h"

static uint16_t u16(const unsigned char* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t u32(const unsigned char* p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(void) {
  struct rd_route_link link = { .index = 7, .linux_flags = 0x1041, .mtu = 1500,
                                .linux_type = 1,
                                .name = "en0" };
  unsigned char packet[128];
  assert(rd_route_encode_link(&link, 0x12345678, 500, packet, sizeof packet) == 48);
  assert(u32(packet) == 48 && u16(packet + 4) == 16); // RTM_NEWLINK
  assert(u16(packet + 6) == 2 && u32(packet + 8) == 0x12345678);
  assert(u32(packet + 12) == 500 && u16(packet + 18) == 1 && u32(packet + 20) == 7);
  assert(u32(packet + 24) == 0x1041);
  assert(u16(packet + 32) == 8 && u16(packet + 34) == 3); // IFLA_IFNAME
  assert(strcmp((char*)packet + 36, "en0") == 0);
  assert(u16(packet + 40) == 8 && u16(packet + 42) == 4); // IFLA_MTU
  assert(u32(packet + 44) == 1500);
  link.address_length = 6;
  const unsigned char mac[6] = {2, 4, 6, 8, 10, 12};
  memcpy(link.address, mac, sizeof mac);
  assert(rd_route_encode_link(&link, 0x12345678, 500, packet, sizeof packet) == 60);
  assert(u16(packet + 48) == 10 && u16(packet + 50) == 1); // IFLA_ADDRESS
  assert(memcmp(packet + 52, mac, sizeof mac) == 0);
  errno = 0;
  assert(rd_route_encode_link(&link, 1, 500, packet, 47) == -1 && errno == ENOSPC);
  link.index = 0;
  errno = 0;
  assert(rd_route_encode_link(&link, 1, 500, packet, sizeof packet) == -1 && errno == EINVAL);

  struct rd_route_link host[128];
  int n = rd_route_collect_links(host, 128);
  assert(n > 0);
  int media_socket = socket(AF_INET, SOCK_DGRAM, 0);
  assert(media_socket >= 0);
  int carrier_links = 0;
  for (int i = 0; i < n; i++) {
    assert(host[i].index == if_nametoindex(host[i].name));
    assert(rd_route_encode_link(&host[i], 55, 500, packet, sizeof packet) > 0);
    struct ifmediareq media = {0};
    strncpy(media.ifm_name, host[i].name, sizeof media.ifm_name - 1);
    int proven_carrier = (host[i].linux_flags & (0x1 | 0x40)) == (0x1 | 0x40) &&
                         ioctl(media_socket, SIOCGIFMEDIA, &media) == 0 &&
                         (media.ifm_status & (IFM_AVALID | IFM_ACTIVE)) == (IFM_AVALID | IFM_ACTIVE);
    assert(!!(host[i].linux_flags & 0x10000) == !!proven_carrier);
    carrier_links += proven_carrier;
    for (int j = 0; j < i; j++) assert(host[j].index != host[i].index);
  }
  close(media_socket);
  errno = 0;
  assert(rd_route_collect_links(NULL, 0) == -1 && errno == ENOSPC);
  struct rd_route_address ipv4 = { .index = 7, .family = 2, .prefix_length = 24,
                                   .flags = 0x80,
                                   .label = "en0", .local = {192, 0, 2, 8},
                                   .peer = {192, 0, 2, 8} };
  assert(rd_route_encode_address(&ipv4, 77, 500, packet, sizeof packet) == 56);
  assert(u32(packet) == 56 && u16(packet + 4) == 20 && u32(packet + 8) == 77);
  assert(u32(packet + 12) == 500 && packet[16] == 2 && packet[17] == 24 && packet[18] == 0x80);
  assert(u32(packet + 20) == 7 && u16(packet + 26) == 1);
  assert(memcmp(packet + 28, ipv4.peer, 4) == 0 && u16(packet + 34) == 2);
  assert(memcmp(packet + 36, ipv4.local, 4) == 0 && u16(packet + 42) == 3);
  assert(strcmp((char*)packet + 44, "en0") == 0);
  assert(u16(packet + 48) == 8 && u16(packet + 50) == 8 && u32(packet + 52) == 0x80);
  assert(rd_route_encode_address(&ipv4, 77, 500, packet, 55) == -1);
  struct rd_route_address ipv6 = { .index = 8, .family = 10, .prefix_length = 64,
                                   .local = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 1},
                                   .peer = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 1} };
  assert(rd_route_encode_address(&ipv6, 78, 500, packet, sizeof packet) == 72);
  assert(packet[16] == 10 && packet[17] == 64 && u16(packet + 26) == 1);
  assert(memcmp(packet + 28, ipv6.peer, 16) == 0 && u16(packet + 46) == 2);
  assert(memcmp(packet + 48, ipv6.local, 16) == 0);
  assert(u16(packet + 64) == 8 && u16(packet + 66) == 8 && u32(packet + 68) == 0);
  struct rd_route_address host_addr[128];
  int count = rd_route_collect_addresses(host_addr, 128, 0);
  assert(count > 0);
  int v4 = 0, v6 = 0;
  for (int i = 0; i < count; i++) {
    assert(host_addr[i].index == if_nametoindex(host_addr[i].label));
    assert(rd_route_encode_address(&host_addr[i], 79, 500, packet, sizeof packet) > 0);
    v4 += host_addr[i].family == 2; v6 += host_addr[i].family == 10;
  }
  assert(v4 > 0 && v6 > 0);
  errno = 0;
  assert(rd_route_collect_addresses(host_addr, 128, 99) == -1 && errno == EINVAL);
  printf("PASS host link/address snapshots and Linux encoding (%d links, %d carrier, %d IPv4, %d IPv6)\n",
         n, carrier_links, v4, v6);
}
