#ifndef SHIMMY_NETLINK_ROUTE_TRANSPORT_H
#define SHIMMY_NETLINK_ROUTE_TRANSPORT_H
#include <stddef.h>

// Host errno convention: -1 sets errno. The caller translates to Linux errno.
// This intentionally supports only read-only RTM_GETLINK dumps.
int rd_rtnl_open(int linux_type, int nonblock, int cloexec, unsigned port_id);
int rd_rtnl_is_fd(int fd);
int rd_rtnl_socket_type(int fd);
unsigned rd_rtnl_port_id(int fd);
int rd_rtnl_send(int fd, const void* request, size_t length);
void rd_rtnl_dup(int oldfd, int newfd);
void rd_rtnl_close(int fd);
#endif
