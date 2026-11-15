#include "netlink_route_transport.h"
#include "netlink_route_snapshot.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define RD_RTNL_MAX_SOCKETS 64
#define RD_RTNL_MAX_LINKS 128
#define RD_RTNL_MAX_ADDRESSES 128
#define RD_RTNL_MAX_REPLY 32768

struct route_socket { int fd, peer, linux_type; unsigned port_id; dev_t device; ino_t inode; };
static struct route_socket sockets[RD_RTNL_MAX_SOCKETS];
static size_t socket_count;
static pthread_mutex_t route_lock = PTHREAD_MUTEX_INITIALIZER;
// Reserve the private peer so guest close_range cannot destroy it.
// Standalone tests provide small implementations of these syscall-layer hooks.
extern int shim_fd_reserve(int fd);
extern void shim_fd_unreserve(int fd);
static void close_peer(int fd) {
  shim_fd_unreserve(fd);
  close(fd);
}

static int slot_locked(int fd) {
  if (!socket_count) return -1;
  struct stat st;
  if (fstat(fd, &st) < 0) return -1;
  for (size_t i = 0; i < socket_count; i++)
    if (sockets[i].fd == fd && sockets[i].device == st.st_dev && sockets[i].inode == st.st_ino)
      return (int)i;
  return -1;
}

int rd_rtnl_is_fd(int fd) {
  pthread_mutex_lock(&route_lock);
  int yes = slot_locked(fd) >= 0;
  pthread_mutex_unlock(&route_lock);
  return yes;
}

int rd_rtnl_socket_type(int fd) {
  pthread_mutex_lock(&route_lock);
  int slot = slot_locked(fd);
  int type = slot < 0 ? -1 : sockets[slot].linux_type;
  pthread_mutex_unlock(&route_lock);
  return type;
}

unsigned rd_rtnl_port_id(int fd) {
  pthread_mutex_lock(&route_lock);
  int slot = slot_locked(fd);
  unsigned port = slot < 0 ? 0 : sockets[slot].port_id;
  pthread_mutex_unlock(&route_lock);
  return port;
}

int rd_rtnl_open(int linux_type, int nonblock, int cloexec, unsigned port_id) {
  if (linux_type != SOCK_DGRAM && linux_type != SOCK_RAW) { errno = EOPNOTSUPP; return -1; }
  // A route endpoint must not escape an exec: its response peer lives inside
  // this shim process and cannot be reconstructed after exec. Reject rather
  // than hand out an fd which would silently stop answering in a new image.
  if (!cloexec) { errno = EOPNOTSUPP; return -1; }
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) < 0) return -1;
  int no_sigpipe = 1;
  setsockopt(pair[1], SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof no_sigpipe);
  fcntl(pair[0], F_SETFD, FD_CLOEXEC);
  fcntl(pair[1], F_SETFD, FD_CLOEXEC);
  pair[1] = shim_fd_reserve(pair[1]);
  if (nonblock) fcntl(pair[0], F_SETFL, fcntl(pair[0], F_GETFL) | O_NONBLOCK);
  fcntl(pair[1], F_SETFL, fcntl(pair[1], F_GETFL) | O_NONBLOCK);
  struct stat st;
  if (fstat(pair[0], &st) < 0) { int e = errno; close(pair[0]); close_peer(pair[1]); errno = e; return -1; }
  pthread_mutex_lock(&route_lock);
  if (socket_count == RD_RTNL_MAX_SOCKETS) {
    pthread_mutex_unlock(&route_lock); close(pair[0]); close_peer(pair[1]); errno = EMFILE; return -1;
  }
  sockets[socket_count++] = (struct route_socket){ pair[0], pair[1], linux_type,
                                                   port_id, st.st_dev, st.st_ino };
  pthread_mutex_unlock(&route_lock);
  return pair[0];
}

void rd_rtnl_dup(int oldfd, int newfd) {
  pthread_mutex_lock(&route_lock);
  int old = slot_locked(oldfd);
  if (newfd != oldfd) {
    // dup2/dup3 closes an existing destination implicitly. Remove its old
    // registration by descriptor number even though fstat now sees the new fd.
    for (size_t i = 0; i < socket_count; i++) if (sockets[i].fd == newfd) {
      int old_peer = sockets[i].peer;
      sockets[i] = sockets[--socket_count];
      if (old == (int)socket_count) old = (int)i;
      int used = 0;
      for (size_t j = 0; j < socket_count; j++) if (sockets[j].peer == old_peer) used = 1;
      if (!used) close_peer(old_peer);
      break;
    }
  }
  if (old >= 0 && socket_count < RD_RTNL_MAX_SOCKETS) {
    struct stat st;
    if (fstat(newfd, &st) == 0)
      sockets[socket_count++] = (struct route_socket){ newfd, sockets[old].peer,
                                                     sockets[old].linux_type,
                                                     sockets[old].port_id, st.st_dev, st.st_ino };
  }
  pthread_mutex_unlock(&route_lock);
}

void rd_rtnl_close(int fd) {
  pthread_mutex_lock(&route_lock);
  int slot = slot_locked(fd);
  if (slot >= 0) {
    int peer = sockets[slot].peer;
    sockets[slot] = sockets[--socket_count];
    int still_used = 0;
    for (size_t i = 0; i < socket_count; i++) if (sockets[i].peer == peer) still_used = 1;
    if (!still_used) close_peer(peer);
  }
  pthread_mutex_unlock(&route_lock);
}

static uint16_t read16(const unsigned char* src) { uint16_t value; memcpy(&value, src, 2); return value; }
static uint32_t read32(const unsigned char* src) { uint32_t value; memcpy(&value, src, 4); return value; }
static void put16(unsigned char* dst, uint16_t value) { memcpy(dst, &value, 2); }
static void put32(unsigned char* dst, uint32_t value) { memcpy(dst, &value, 4); }
static size_t align4(size_t n) { return (n + 3u) & ~3u; }

int rd_rtnl_send(int fd, const void* request, size_t length) {
  if (!request || length < 24 || length > 4096) { errno = EINVAL; return -1; }
  const unsigned char* req = request;
  uint32_t wire_length = read32(req);
  if (wire_length < 24 || wire_length > length) { errno = EINVAL; return -1; }
  for (size_t i = wire_length; i < length; i++)
    if (req[i] != 0) { errno = EINVAL; return -1; }
  // One complete GETLINK dump request per datagram. Refuse unknown attributes,
  // control operations, and malformed multipart requests before enqueueing any
  // reply. A silent partial dump would be mistaken for real network state.
  static const unsigned char empty_ifinfo[16] = {0};
  int link_request = read16(req + 4) == 18 /*RTM_GETLINK*/;
  int link_family = req[16] == 0 || req[16] == 2 || req[16] == 10 || req[16] == 17;
  int plain = wire_length == 32 && link_family &&
              memcmp(req + 17, empty_ifinfo, 15) == 0;
  // iproute2 asks for AF_PACKET plus IFLA_EXT_MASK=(VF|SKIP_STATS).
  // Shimmy exposes no virtual functions and emits no stats, so that exact
  // filter has a truthful empty result for its optional extensions.
  int ip_extended = wire_length == 40 && (req[16] == 0 || req[16] == 17 /*AF_PACKET*/) &&
                    memcmp(req + 17, empty_ifinfo, 15) == 0 &&
                    read16(req + 32) == 8 && read16(req + 34) == 29 /*IFLA_EXT_MASK*/ &&
                    read32(req + 36) == 9 /*VF|SKIP_STATS*/;
  int address_request = read16(req + 4) == 22 /*RTM_GETADDR*/ && wire_length == 24 &&
                        (req[16] == 0 || req[16] == 2 || req[16] == 10) &&
                        memcmp(req + 17, empty_ifinfo, 7) == 0;
  if (read16(req + 6) != 0x301 /*REQUEST|DUMP, no ACK requested*/ ||
      (!(link_request && (plain || ip_extended)) && !address_request)) {
    errno = EOPNOTSUPP; return -1;
  }
  uint32_t sequence = read32(req + 8);
  unsigned char reply[RD_RTNL_MAX_REPLY];
  size_t offset = 0;
  if (link_request) {
    struct rd_route_link links[RD_RTNL_MAX_LINKS];
    int count = rd_route_collect_links(links, RD_RTNL_MAX_LINKS);
    if (count < 0) return -1;
    for (int i = 0; i < count; i++) {
      if (offset + 20 >= sizeof reply) { errno = ENOSPC; return -1; }
      int n = rd_route_encode_link(&links[i], sequence, rd_rtnl_port_id(fd),
                                   reply + offset, sizeof reply - offset - 20);
      if (n < 0) return -1;
      offset += align4((size_t)n);
    }
  } else {
    struct rd_route_address addresses[RD_RTNL_MAX_ADDRESSES];
    int count = rd_route_collect_addresses(addresses, RD_RTNL_MAX_ADDRESSES, req[16]);
    if (count < 0) return -1;
    for (int i = 0; i < count; i++) {
      if (offset + 20 >= sizeof reply) { errno = ENOSPC; return -1; }
      int n = rd_route_encode_address(&addresses[i], sequence, rd_rtnl_port_id(fd),
                                      reply + offset, sizeof reply - offset - 20);
      if (n < 0) return -1;
      offset += align4((size_t)n);
    }
  }
  if (offset + 20 > sizeof reply) { errno = ENOSPC; return -1; }
  put32(reply + offset, 20);
  put16(reply + offset + 4, 3 /*NLMSG_DONE*/);
  put16(reply + offset + 6, 2 /*NLM_F_MULTI*/);
  put32(reply + offset + 8, sequence);
  put32(reply + offset + 12, rd_rtnl_port_id(fd));
  put32(reply + offset + 16, 0); // NLMSG_DONE status (zero = complete)
  offset += 20;
  pthread_mutex_lock(&route_lock);
  int slot = slot_locked(fd);
  int result = slot < 0 ? -1 : (int)send(sockets[slot].peer, reply, offset, 0);
  int saved = slot < 0 ? EBADF : errno;
  pthread_mutex_unlock(&route_lock);
  if (result != (int)offset) { errno = result >= 0 ? EIO : saved; return -1; }
  return (int)length;
}
