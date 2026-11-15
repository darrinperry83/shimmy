#ifndef SHIMMY_NETLINK_ROUTE_SNAPSHOT_H
#define SHIMMY_NETLINK_ROUTE_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>
#include <net/if.h>

struct rd_route_link {
  unsigned index;
  unsigned linux_flags;
  unsigned mtu;
  unsigned short linux_type;
  unsigned char address[32];
  unsigned char address_length;
  char name[IFNAMSIZ];
};

// Capture a bounded, deduplicated snapshot of actual host interfaces. Returns
// the number copied, or -1 with errno set. No guest network availability is
// inferred from an interface merely existing.
int rd_route_collect_links(struct rd_route_link* links, size_t capacity);

// Encode one Linux RTM_NEWLINK message, including the original request's
// sequence number. Returns bytes written or -1 (errno=ENOSPC/EINVAL).
int rd_route_encode_link(const struct rd_route_link* link, uint32_t sequence, uint32_t port_id,
                         void* output, size_t capacity);

struct rd_route_address {
  unsigned index;
  unsigned char family; // Linux AF_INET (2) or AF_INET6 (10)
  unsigned char prefix_length;
  unsigned char scope; // Linux RT_SCOPE_UNIVERSE/LINK/HOST
  unsigned char flags; // only host-proven Linux IFA flags
  unsigned char local[16];
  unsigned char peer[16]; // IFA_ADDRESS; equals local except for point-to-point
  char label[IFNAMSIZ];
};

int rd_route_collect_addresses(struct rd_route_address* addresses, size_t capacity,
                               unsigned family_filter);
int rd_route_encode_address(const struct rd_route_address* address, uint32_t sequence,
                            uint32_t port_id, void* output, size_t capacity);

#endif
