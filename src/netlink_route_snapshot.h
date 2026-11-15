#ifndef SHIMMY_NETLINK_ROUTE_SNAPSHOT_H
#define SHIMMY_NETLINK_ROUTE_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>
#include <net/if.h>

struct rd_route_link {
  unsigned index;
  unsigned linux_flags;
  unsigned mtu;
  char name[IFNAMSIZ];
};

// Capture a bounded, deduplicated snapshot of actual host interfaces. Returns
// the number copied, or -1 with errno set. No guest network availability is
// inferred from an interface merely existing.
int rd_route_collect_links(struct rd_route_link* links, size_t capacity);

// Encode one Linux RTM_NEWLINK message, including the original request's
// sequence number. Returns bytes written or -1 (errno=ENOSPC/EINVAL).
int rd_route_encode_link(const struct rd_route_link* link, uint32_t sequence,
                         void* output, size_t capacity);

#endif
