#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include "netlink_route_transport.h"
static int reserved, unreserved;
int shim_fd_reserve(int fd) { __sync_fetch_and_add(&reserved, 1); return fd; }
void shim_fd_unreserve(int fd) { (void)fd; __sync_fetch_and_add(&unreserved, 1); }

static void put16(unsigned char* p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(unsigned char* p, uint32_t v) { memcpy(p, &v, 4); }
static uint16_t get16(const unsigned char* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t get32(const unsigned char* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void request(unsigned char p[32], uint32_t seq) {
  memset(p, 0, 32);
  put32(p, 32); put16(p + 4, 18); put16(p + 6, 0x301); put32(p + 8, seq);
}
static void extended_request(unsigned char p[40], uint32_t seq, uint32_t mask) {
  memset(p, 0, 40);
  put32(p, 40); put16(p + 4, 18); put16(p + 6, 0x301); put32(p + 8, seq);
  p[16] = 17; // AF_PACKET
  put16(p + 32, 8); put16(p + 34, 29); put32(p + 36, mask); // IFLA_EXT_MASK
}
static void address_request(unsigned char p[24], uint32_t seq, unsigned char family) {
  memset(p, 0, 24);
  put32(p, 24); put16(p + 4, 22); put16(p + 6, 0x301); put32(p + 8, seq);
  p[16] = family;
}
static void check_dump(int fd, uint32_t seq, uint16_t entry_type, unsigned char family) {
  unsigned char buf[32768];
  ssize_t n = recv(fd, buf, sizeof buf, 0);
  assert(n > 32);
  size_t pos = 0; int links = 0, done = 0;
  while (pos + 16 <= (size_t)n) {
    size_t length = get32(buf + pos);
    assert(length >= 16 && pos + length <= (size_t)n);
    assert(get32(buf + pos + 8) == seq);
    assert(get32(buf + pos + 12) == rd_rtnl_port_id(fd));
    if (get16(buf + pos + 4) == entry_type) {
      if (family) assert(buf[pos + 16] == family);
      links++;
    }
    else { assert(get16(buf + pos + 4) == 3 && length == 20);
           assert(get32(buf + pos + 16) == 0); done++; }
    pos += (length + 3) & ~3u;
  }
  assert(pos == (size_t)n && links > 0 && done == 1);
}
static void* concurrent_client(void* argument) {
  uint32_t base = *(uint32_t*)argument;
  int fd = rd_rtnl_open(SOCK_RAW, 0, 1, base);
  assert(fd >= 0);
  for (uint32_t i = 0; i < 8; i++) {
    unsigned char req[32]; request(req, base + i);
    assert(rd_rtnl_send(fd, req, sizeof req) == 32);
    check_dump(fd, base + i, 16, 0);
  }
  rd_rtnl_close(fd); close(fd);
  return NULL;
}
int main(void) {
  errno = 0;
  assert(rd_rtnl_open(SOCK_DGRAM, 0, 0, 111) == -1 && errno == EOPNOTSUPP);
  int first = rd_rtnl_open(SOCK_DGRAM, 0, 1, 111), second = rd_rtnl_open(SOCK_RAW, 0, 1, 222);
  assert(first >= 0 && second >= 0);
  assert(rd_rtnl_get_pktinfo(first) == 0);
  assert(rd_rtnl_set_pktinfo(first, 1) == 0);
  assert(rd_rtnl_get_pktinfo(first) == 1);
  assert(rd_rtnl_get_pktinfo(second) == 0);
  assert(first >= 0 && second >= 0 && first != second);
  assert(rd_rtnl_is_fd(first) && rd_rtnl_is_fd(second));
  unsigned char a[32], b[32]; request(a, 101); request(b, 202);
  errno = 0;
  assert(rd_rtnl_send(first, a, 31) == -1 && errno == EINVAL);
  a[4] = 26; // unsupported RTM_GETROUTE must not enqueue a reply
  errno = 0;
  assert(rd_rtnl_send(first, a, 32) == -1 && errno == EOPNOTSUPP);
  a[4] = 22; // malformed GETADDR: link-size payload
  errno = 0;
  assert(rd_rtnl_send(first, a, 32) == -1 && errno == EOPNOTSUPP);
  unsigned char empty[32];
  errno = 0;
  assert(recv(first, empty, sizeof empty, MSG_DONTWAIT) == -1 &&
         (errno == EAGAIN || errno == EWOULDBLOCK));
  request(a, 101);
  assert(rd_rtnl_send(first, a, 32) == 32);
  assert(rd_rtnl_send(second, b, 32) == 32);
  check_dump(second, 202, 16, 0); check_dump(first, 101, 16, 0);
  unsigned char extended[40]; extended_request(extended, 303, 9);
  assert(rd_rtnl_send(first, extended, sizeof extended) == 40);
  check_dump(first, 303, 16, 0);
  extended[16] = 0; put32(extended + 8, 305); // ip addr: all link families
  assert(rd_rtnl_send(first, extended, sizeof extended) == 40);
  check_dump(first, 305, 16, 0);
  request(a, 306); a[16] = 2; // ip -4 addr link prelude
  assert(rd_rtnl_send(first, a, sizeof a) == 32);
  check_dump(first, 306, 16, 0);
  request(a, 307); a[16] = 10; // ip -6 addr link prelude
  assert(rd_rtnl_send(first, a, sizeof a) == 32);
  check_dump(first, 307, 16, 0);
  extended_request(extended, 304, 1); // requested stats instead of skip-stats
  errno = 0;
  assert(rd_rtnl_send(first, extended, sizeof extended) == -1 && errno == EOPNOTSUPP);
  errno = 0;
  assert(recv(first, empty, sizeof empty, MSG_DONTWAIT) == -1 &&
         (errno == EAGAIN || errno == EWOULDBLOCK));
  unsigned char addr_req[24];
  address_request(addr_req, 501, 2);
  assert(rd_rtnl_send(first, addr_req, sizeof addr_req) == 24);
  check_dump(first, 501, 20, 2);
  address_request(addr_req, 502, 10);
  assert(rd_rtnl_send(second, addr_req, sizeof addr_req) == 24);
  check_dump(second, 502, 20, 10);
  address_request(addr_req, 503, 0);
  assert(rd_rtnl_send(first, addr_req, sizeof addr_req) == 24);
  check_dump(first, 503, 20, 0);
  unsigned char padded_addr[152] = {0};
  address_request(padded_addr, 506, 0);
  assert(rd_rtnl_send(first, padded_addr, sizeof padded_addr) == 152);
  check_dump(first, 506, 20, 0);
  padded_addr[100] = 1;
  errno = 0;
  assert(rd_rtnl_send(first, padded_addr, sizeof padded_addr) == -1 && errno == EINVAL);
  address_request(addr_req, 504, 99);
  errno = 0;
  assert(rd_rtnl_send(first, addr_req, sizeof addr_req) == -1 && errno == EOPNOTSUPP);
  address_request(addr_req, 505, 2); addr_req[20] = 1; // unsupported index filter
  errno = 0;
  assert(rd_rtnl_send(first, addr_req, sizeof addr_req) == -1 && errno == EOPNOTSUPP);
  assert(dup2(first, second) == second); // closes prior destination implicitly
  rd_rtnl_dup(first, second);
  assert(rd_rtnl_get_pktinfo(second) == 1);
  request(a, 101);
  assert(rd_rtnl_send(second, a, 32) == 32);
  check_dump(second, 101, 16, 0);
  int duplicate = dup(first); assert(duplicate >= 0);
  rd_rtnl_dup(first, duplicate);
  assert(rd_rtnl_get_pktinfo(duplicate) == 1);
  assert(rd_rtnl_set_pktinfo(duplicate, 0) == 0);
  assert(rd_rtnl_get_pktinfo(first) == 0);
  rd_rtnl_close(first); close(first);
  assert(rd_rtnl_send(duplicate, a, 32) == 32);
  check_dump(duplicate, 101, 16, 0);
  rd_rtnl_close(duplicate); close(duplicate);
  rd_rtnl_close(second); close(second);
  pthread_t workers[4]; uint32_t bases[4] = {1000, 2000, 3000, 4000};
  for (int i = 0; i < 4; i++) assert(pthread_create(&workers[i], NULL, concurrent_client, &bases[i]) == 0);
  for (int i = 0; i < 4; i++) assert(pthread_join(workers[i], NULL) == 0);
  assert(reserved == unreserved);
  puts("PASS bounded GETLINK multipart, malformed/unsupported requests, sequences, sockets and dup");
}
