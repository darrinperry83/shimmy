#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <net/if.h>
#include "netlink_route_snapshot.h"

static uint16_t u16(const unsigned char* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t u32(const unsigned char* p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(void) {
  struct rd_route_link link = { .index = 7, .linux_flags = 0x1041, .mtu = 1500,
                                .linux_type = 1,
                                .name = "en0" };
  unsigned char packet[128];
  assert(rd_route_encode_link(&link, 0x12345678, packet, sizeof packet) == 48);
  assert(u32(packet) == 48 && u16(packet + 4) == 16); // RTM_NEWLINK
  assert(u16(packet + 6) == 2 && u32(packet + 8) == 0x12345678);
  assert(u32(packet + 12) == 0 && u16(packet + 18) == 1 && u32(packet + 20) == 7);
  assert(u32(packet + 24) == 0x1041);
  assert(u16(packet + 32) == 8 && u16(packet + 34) == 3); // IFLA_IFNAME
  assert(strcmp((char*)packet + 36, "en0") == 0);
  assert(u16(packet + 40) == 8 && u16(packet + 42) == 4); // IFLA_MTU
  assert(u32(packet + 44) == 1500);
  errno = 0;
  assert(rd_route_encode_link(&link, 1, packet, 47) == -1 && errno == ENOSPC);
  link.index = 0;
  errno = 0;
  assert(rd_route_encode_link(&link, 1, packet, sizeof packet) == -1 && errno == EINVAL);

  struct rd_route_link host[128];
  int n = rd_route_collect_links(host, 128);
  assert(n > 0);
  for (int i = 0; i < n; i++) {
    assert(host[i].index == if_nametoindex(host[i].name));
    assert(rd_route_encode_link(&host[i], 55, packet, sizeof packet) > 0);
    for (int j = 0; j < i; j++) assert(host[j].index != host[i].index);
  }
  errno = 0;
  assert(rd_route_collect_links(NULL, 0) == -1 && errno == ENOSPC);
  printf("PASS truthful host link snapshot and Linux RTM_NEWLINK encoding (%d links)\n", n);
}
