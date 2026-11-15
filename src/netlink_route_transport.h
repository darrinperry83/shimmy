#ifndef SHIMMY_NETLINK_ROUTE_TRANSPORT_H
#define SHIMMY_NETLINK_ROUTE_TRANSPORT_H
#include <stddef.h>
#include "netlink_route_snapshot.h"

// Host errno convention: -1 sets errno. The caller translates to Linux errno.
// Read-only link/address dumps and host-backed link/address notifications.
int rd_rtnl_open(int linux_type, int nonblock, int cloexec, unsigned port_id);
int rd_rtnl_is_fd(int fd);
int rd_rtnl_socket_type(int fd);
unsigned rd_rtnl_port_id(int fd);
int rd_rtnl_set_pktinfo(int fd, int enabled);
int rd_rtnl_get_pktinfo(int fd);
int rd_rtnl_membership(int fd, unsigned group, int join);
int rd_rtnl_memberships(int fd, void* output, size_t* length);
unsigned rd_rtnl_event_group(const void* packet, size_t length);
int rd_rtnl_take_overflow(int fd);
// Reconcile a host-backed snapshot after an actual PF_ROUTE notification.
// Exposed for deterministic tests; production calls it only from the monitor.
int rd_rtnl_reconcile(void);
int rd_rtnl_reconcile_snapshot(const struct rd_route_link* links, size_t link_count,
                               const struct rd_route_address* addresses, size_t address_count);
int rd_rtnl_send(int fd, const void* request, size_t length);
void rd_rtnl_dup(int oldfd, int newfd);
void rd_rtnl_close(int fd);
#endif
