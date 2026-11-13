// test_net.c — standalone exercise of net.c through the raw Linux syscall ABI.
//
//   clang -arch arm64 -O2 -Wall -o /Users/dperry/linux-shim/tests/test_net \
//         /Users/dperry/linux-shim/src/test_net.c /Users/dperry/linux-shim/src/net.c
//
// Everything goes through net_syscall() with LINUX syscall numbers, LINUX struct layouts and
// LINUX constant values, so the translation layer is what is actually under test. Scratch files
// live under /Users/dperry/linux-shim/tests (never /tmp, which gets purged).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include "net.h"

#define SCRATCH "/Users/dperry/linux-shim/tests/net-scratch"
#define PREFIX  SCRATCH "/prefix"

// ---- Linux syscall numbers (aarch64) ----
#define N_eventfd2      19
#define N_epoll_create1 20
#define N_epoll_ctl     21
#define N_epoll_pwait   22
#define N_pipe2         59
#define N_pselect6      72
#define N_ppoll         73
#define N_socket        198
#define N_socketpair    199
#define N_bind          200
#define N_listen        201
#define N_accept        202
#define N_connect       203
#define N_getsockname   204
#define N_getpeername   205
#define N_sendto        206
#define N_recvfrom      207
#define N_setsockopt    208
#define N_getsockopt    209
#define N_shutdown      210
#define N_sendmsg       211
#define N_recvmsg       212
#define N_accept4       242

// ---- Linux constants ----
#define LAF_UNIX  1
#define LAF_INET  2
#define LAF_INET6 10
#define LSOCK_STREAM 1
#define LSOCK_DGRAM  2
#define LSOCK_NONBLOCK 0x00800
#define LSOCK_CLOEXEC  0x80000
#define LO_CLOEXEC     0x80000
#define LEFD_NONBLOCK  0x00800
#define LEAGAIN 11
#define LENOENT 2
#define LEEXIST 17
#define LSOL_SOCKET 1
#define LSO_REUSEADDR 2
#define LSO_ERROR 4
#define LSO_PASSCRED 16
#define LEPOLL_CTL_ADD 1
#define LEPOLL_CTL_DEL 2
#define LEPOLLIN  0x001u
#define LEPOLLOUT 0x004u
#define LPOLLIN 0x0001
#define LSCM_RIGHTS 1

// ---- Linux struct layouts (aarch64) ----
struct lin_sockaddr_un { uint16_t sun_family; char sun_path[108]; };
struct lin_epoll_event { uint32_t events; uint32_t __pad; uint64_t data; };  // NOT packed on arm64
struct lin_msghdr {
  uint64_t msg_name; uint32_t msg_namelen; uint32_t __pad1;
  uint64_t msg_iov;  uint64_t msg_iovlen;
  uint64_t msg_control; uint64_t msg_controllen;
  int32_t msg_flags; uint32_t __pad2;
};
struct lin_cmsghdr { uint64_t cmsg_len; int32_t cmsg_level; int32_t cmsg_type; };
struct lin_timespec { int64_t tv_sec; int64_t tv_nsec; };

static int g_fail, g_pass;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("    FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
  } while (0)
#define SECTION(name) printf("\n== %s ==\n", name)
#define OK(fmt, ...) printf("    ok   " fmt "\n", ##__VA_ARGS__)

static long sc(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
               unsigned long a3, unsigned long a4, unsigned long a5) {
  return net_syscall(nr, a0, a1, a2, a3, a4, a5);
}
#define SC0(n)                   sc(n,0,0,0,0,0,0)
#define SC1(n,a)                 sc(n,(unsigned long)(a),0,0,0,0,0)
#define SC2(n,a,b)               sc(n,(unsigned long)(a),(unsigned long)(b),0,0,0,0)
#define SC3(n,a,b,c)             sc(n,(unsigned long)(a),(unsigned long)(b),(unsigned long)(c),0,0,0)
#define SC4(n,a,b,c,d)           sc(n,(unsigned long)(a),(unsigned long)(b),(unsigned long)(c),(unsigned long)(d),0,0)
#define SC5(n,a,b,c,d,e)         sc(n,(unsigned long)(a),(unsigned long)(b),(unsigned long)(c),(unsigned long)(d),(unsigned long)(e),0)
#define SC6(n,a,b,c,d,e,f)       sc(n,(unsigned long)(a),(unsigned long)(b),(unsigned long)(c),(unsigned long)(d),(unsigned long)(e),(unsigned long)(f))

static uint32_t un_len(const struct lin_sockaddr_un* u, size_t pathbytes) {
  (void)u; return (uint32_t)(offsetof(struct lin_sockaddr_un, sun_path) + pathbytes);
}
static void un_path(struct lin_sockaddr_un* u, const char* p) {
  memset(u, 0, sizeof *u);
  u->sun_family = LAF_UNIX;
  snprintf(u->sun_path, sizeof u->sun_path, "%s", p);
}
static void un_abstract(struct lin_sockaddr_un* u, const char* name) {
  memset(u, 0, sizeof *u);
  u->sun_family = LAF_UNIX;
  u->sun_path[0] = 0;                       // the abstract marker
  memcpy(u->sun_path + 1, name, strlen(name));
}
static void hexify(const char* in, char* out) {
  static const char h[] = "0123456789abcdef";
  size_t n = strlen(in);
  for (size_t i = 0; i < n; i++) { out[2*i] = h[(unsigned char)in[i] >> 4]; out[2*i+1] = h[(unsigned char)in[i] & 15]; }
  out[2*n] = 0;
}

// ---------------------------------------------------------------------------
static void t_socketpair(void) {
  SECTION("(a) socketpair + sendto/recvfrom round-trip");
  int sv[2] = { -1, -1 };
  long r = SC4(N_socketpair, LAF_UNIX, LSOCK_STREAM, 0, sv);
  CHECK(r == 0, "socketpair -> %ld", r);
  CHECK(sv[0] >= 0 && sv[1] >= 0, "bad fds %d %d", sv[0], sv[1]);

  const char* msg = "hello-from-the-guest";
  long n = SC6(N_sendto, sv[0], msg, strlen(msg), 0, 0, 0);
  CHECK(n == (long)strlen(msg), "sendto -> %ld", n);

  char buf[64] = {0};
  n = SC6(N_recvfrom, sv[1], buf, sizeof buf, 0, 0, 0);
  CHECK(n == (long)strlen(msg), "recvfrom -> %ld", n);
  CHECK(!memcmp(buf, msg, strlen(msg)), "payload mismatch: '%s'", buf);
  OK("round-tripped %ld bytes: '%s'", n, buf);

  // reverse direction, exercising shutdown + EOF
  n = SC6(N_sendto, sv[1], "pong", 4, 0, 0, 0);
  CHECK(n == 4, "sendto back -> %ld", n);
  memset(buf, 0, sizeof buf);
  n = SC6(N_recvfrom, sv[0], buf, sizeof buf, 0, 0, 0);
  CHECK(n == 4 && !memcmp(buf, "pong", 4), "recvfrom back -> %ld '%s'", n, buf);
  CHECK(SC2(N_shutdown, sv[0], 2) == 0, "shutdown failed");
  close(sv[0]); close(sv[1]);
}

// ---------------------------------------------------------------------------
static void t_unix_prefix(void) {
  SECTION("(b) unix socket via net_set_unix_prefix -> relocated host path");
  const char* guest = "/tmp/.X11-unix/X0";
  const char* host  = PREFIX "/tmp/.X11-unix/X0";
  unlink(host);

  struct lin_sockaddr_un sa; un_path(&sa, guest);
  uint32_t salen = un_len(&sa, strlen(guest) + 1);

  long srv = SC3(N_socket, LAF_UNIX, LSOCK_STREAM, 0);
  CHECK(srv >= 0, "socket -> %ld", srv);
  long r = SC3(N_bind, srv, &sa, salen);
  CHECK(r == 0, "bind(%s) -> %ld", guest, r);

  struct stat st;
  CHECK(stat(host, &st) == 0 && S_ISSOCK(st.st_mode), "host socket not at %s", host);
  OK("guest %s -> host %s", guest, host);

  CHECK(SC2(N_listen, srv, 8) == 0, "listen failed");

  long cli = SC3(N_socket, LAF_UNIX, LSOCK_STREAM, 0);
  CHECK(cli >= 0, "client socket -> %ld", cli);
  r = SC3(N_connect, cli, &sa, salen);
  CHECK(r == 0, "connect -> %ld", r);

  struct lin_sockaddr_un peer; memset(&peer, 0xAA, sizeof peer);
  uint32_t plen = sizeof peer;
  long acc = SC3(N_accept, srv, &peer, &plen);
  CHECK(acc >= 0, "accept -> %ld", acc);

  const char* q = "X-request";
  CHECK(SC6(N_sendto, cli, q, strlen(q), 0, 0, 0) == (long)strlen(q), "client send failed");
  char buf[64] = {0};
  long n = SC6(N_recvfrom, acc, buf, sizeof buf, 0, 0, 0);
  CHECK(n == (long)strlen(q) && !memcmp(buf, q, n), "server recv -> %ld '%s'", n, buf);
  CHECK(SC6(N_sendto, acc, "X-reply", 7, 0, 0, 0) == 7, "server send failed");
  memset(buf, 0, sizeof buf);
  n = SC6(N_recvfrom, cli, buf, sizeof buf, 0, 0, 0);
  CHECK(n == 7 && !memcmp(buf, "X-reply", 7), "client recv -> %ld '%s'", n, buf);
  OK("bytes exchanged over the relocated socket both ways");

  // getsockname must hand the GUEST path back, not the host one
  struct lin_sockaddr_un self; memset(&self, 0xAA, sizeof self);
  uint32_t slen = sizeof self;
  r = SC3(N_getsockname, srv, &self, &slen);
  CHECK(r == 0, "getsockname -> %ld", r);
  CHECK(self.sun_family == LAF_UNIX, "family %u", self.sun_family);
  CHECK(!strcmp(self.sun_path, guest), "getsockname returned '%s', want '%s'", self.sun_path, guest);
  CHECK(slen == un_len(&self, strlen(guest) + 1), "addrlen %u", slen);
  OK("getsockname un-relocated to '%s' (len %u)", self.sun_path, slen);

  // accept4(SOCK_NONBLOCK) + getpeername on the client side
  long cli2 = SC3(N_socket, LAF_UNIX, LSOCK_STREAM, 0);
  CHECK(SC3(N_connect, cli2, &sa, salen) == 0, "second connect failed");
  long acc2 = SC4(N_accept4, srv, 0, 0, LSOCK_NONBLOCK);
  CHECK(acc2 >= 0, "accept4 -> %ld", acc2);
  CHECK((fcntl((int)acc2, F_GETFL, 0) & O_NONBLOCK) != 0, "accept4 SOCK_NONBLOCK not applied");
  char nb[4];
  CHECK(SC6(N_recvfrom, acc2, nb, sizeof nb, 0, 0, 0) == -LEAGAIN, "accept4 fd is not nonblocking");
  OK("accept4 applied SOCK_NONBLOCK to the new fd");

  struct lin_sockaddr_un pn; memset(&pn, 0xAA, sizeof pn);
  uint32_t pnlen = sizeof pn;
  CHECK(SC3(N_getpeername, cli2, &pn, &pnlen) == 0, "getpeername failed");
  CHECK(pn.sun_family == LAF_UNIX && !strcmp(pn.sun_path, guest),
        "getpeername returned '%s', want '%s'", pn.sun_path, guest);
  OK("getpeername un-relocated to '%s'", pn.sun_path);
  close((int)acc2); close((int)cli2);

  // connecting to a path that does not exist must be Linux ENOENT (-2)
  struct lin_sockaddr_un bad; un_path(&bad, "/tmp/.X11-unix/Xnope");
  long c2 = SC3(N_socket, LAF_UNIX, LSOCK_STREAM, 0);
  r = SC3(N_connect, c2, &bad, un_len(&bad, strlen(bad.sun_path) + 1));
  CHECK(r == -LENOENT, "connect to missing path -> %ld (want -2)", r);
  OK("missing path -> -ENOENT(-2), the LINUX value");
  close((int)c2); close((int)acc); close((int)cli); close((int)srv);
  unlink(host);
}

// ---------------------------------------------------------------------------
static void t_abstract(void) {
  SECTION("(c) abstract socket (sun_path[0]==0) via hex encoding");
  const char* name = "rdx-net-abstract";
  struct lin_sockaddr_un sa; un_abstract(&sa, name);
  uint32_t salen = un_len(&sa, 1 + strlen(name));   // NO trailing NUL for abstract names

  long srv = SC3(N_socket, LAF_UNIX, LSOCK_STREAM, 0);
  CHECK(srv >= 0, "socket -> %ld", srv);
  long r = SC3(N_bind, srv, &sa, salen);
  CHECK(r == 0, "bind(abstract) -> %ld", r);

  char hex[128], hostpath[512];
  hexify(name, hex);
  snprintf(hostpath, sizeof hostpath, "%s/.rdx-abstract/%s", PREFIX, hex);
  struct stat st;
  CHECK(stat(hostpath, &st) == 0 && S_ISSOCK(st.st_mode), "no hex-encoded socket at %s", hostpath);
  OK("abstract '\\0%s' -> %s", name, hostpath);

  CHECK(SC2(N_listen, srv, 4) == 0, "listen failed");
  long cli = SC3(N_socket, LAF_UNIX, LSOCK_STREAM, 0);
  r = SC3(N_connect, cli, &sa, salen);
  CHECK(r == 0, "connect(abstract) -> %ld", r);
  long acc = SC3(N_accept, srv, 0, 0);
  CHECK(acc >= 0, "accept -> %ld", acc);

  CHECK(SC6(N_sendto, cli, "abs-ping", 8, 0, 0, 0) == 8, "send failed");
  char buf[32] = {0};
  long n = SC6(N_recvfrom, acc, buf, sizeof buf, 0, 0, 0);
  CHECK(n == 8 && !memcmp(buf, "abs-ping", 8), "recv -> %ld '%s'", n, buf);
  OK("abstract round-trip carried %ld bytes", n);

  struct lin_sockaddr_un self; memset(&self, 0xAA, sizeof self);
  uint32_t slen = sizeof self;
  r = SC3(N_getsockname, srv, &self, &slen);
  CHECK(r == 0, "getsockname -> %ld", r);
  CHECK(self.sun_family == LAF_UNIX && self.sun_path[0] == 0, "not reported as abstract");
  CHECK(slen == un_len(&self, 1 + strlen(name)), "addrlen %u want %u", slen, un_len(&self, 1 + strlen(name)));
  CHECK(!memcmp(self.sun_path + 1, name, strlen(name)), "abstract name not recovered");
  // NB: exactly `slen` bytes are written and no more, so print with an explicit length.
  OK("getsockname decoded back to '\\0%.*s' (len %u)", (int)strlen(name), self.sun_path + 1, slen);

  // rebinding the same abstract name after close must work (no filesystem persistence on Linux)
  close((int)acc); close((int)cli); close((int)srv);
  long srv2 = SC3(N_socket, LAF_UNIX, LSOCK_STREAM, 0);
  r = SC3(N_bind, srv2, &sa, salen);
  CHECK(r == 0, "re-bind of a released abstract name -> %ld", r);
  OK("stale abstract name reclaimed (no bogus EADDRINUSE)");
  close((int)srv2);
  unlink(hostpath);
}

// ---------------------------------------------------------------------------
static void t_nonblock(void) {
  SECTION("(d) SOCK_NONBLOCK -> real O_NONBLOCK, EAGAIN as the LINUX value 11");
  int sv[2] = { -1, -1 };
  long r = SC4(N_socketpair, LAF_UNIX, LSOCK_STREAM | LSOCK_NONBLOCK, 0, sv);
  CHECK(r == 0, "socketpair(NONBLOCK) -> %ld", r);
  CHECK((fcntl(sv[0], F_GETFL, 0) & O_NONBLOCK) != 0, "fd0 is not O_NONBLOCK");
  CHECK((fcntl(sv[1], F_GETFL, 0) & O_NONBLOCK) != 0, "fd1 is not O_NONBLOCK");

  char buf[8];
  long n = SC6(N_recvfrom, sv[0], buf, sizeof buf, 0, 0, 0);
  CHECK(n == -LEAGAIN, "recvfrom on empty nonblocking socket -> %ld (want -11)", n);
  OK("host EAGAIN(35) translated to linux EAGAIN(-11)");

  // SOCK_CLOEXEC must not leak into the type argument either
  long fd = SC3(N_socket, LAF_UNIX, LSOCK_STREAM | LSOCK_CLOEXEC, 0);
  CHECK(fd >= 0, "socket(CLOEXEC) -> %ld", fd);
  CHECK((fcntl((int)fd, F_GETFD, 0) & FD_CLOEXEC) != 0, "SOCK_CLOEXEC not applied");
  OK("SOCK_CLOEXEC(0x80000) masked off the type and applied via FD_CLOEXEC");
  close((int)fd); close(sv[0]); close(sv[1]);
}

// ---------------------------------------------------------------------------
static void t_epoll(void) {
  SECTION("(e) epoll_create1/ctl/pwait on kqueue + aarch64 epoll_event layout");
  printf("    layout: sizeof(struct epoll_event)=%zu offsetof(events)=%zu offsetof(data)=%zu\n",
         sizeof(struct lin_epoll_event), offsetof(struct lin_epoll_event, events),
         offsetof(struct lin_epoll_event, data));
  CHECK(sizeof(struct lin_epoll_event) == 16, "epoll_event must be 16 bytes on aarch64");
  CHECK(offsetof(struct lin_epoll_event, data) == 8, "epoll_event.data must be at offset 8");
  CHECK(offsetof(struct lin_epoll_event, events) == 0, "epoll_event.events must be at offset 0");

  long ep = SC1(N_epoll_create1, LO_CLOEXEC);
  CHECK(ep >= 0, "epoll_create1 -> %ld", ep);
  CHECK((fcntl((int)ep, F_GETFD, 0) & FD_CLOEXEC) != 0, "EPOLL_CLOEXEC not applied");

  int sv[2];
  CHECK(SC4(N_socketpair, LAF_UNIX, LSOCK_STREAM, 0, sv) == 0, "socketpair failed");

  const uint64_t MAGIC = 0xDEADBEEFCAFEBABEull;
  struct lin_epoll_event ev = { LEPOLLIN, 0, MAGIC };
  long r = SC4(N_epoll_ctl, ep, LEPOLL_CTL_ADD, sv[1], &ev);
  CHECK(r == 0, "epoll_ctl ADD -> %ld", r);
  r = SC4(N_epoll_ctl, ep, LEPOLL_CTL_ADD, sv[1], &ev);
  CHECK(r == -LEEXIST, "duplicate ADD -> %ld (want -17 EEXIST)", r);

  struct lin_epoll_event out[4];
  memset(out, 0xAA, sizeof out);
  r = SC4(N_epoll_pwait, ep, out, 4, 0);           // nothing pending yet
  CHECK(r == 0, "epoll_pwait with no events -> %ld (want 0)", r);

  CHECK(SC6(N_sendto, sv[0], "wake", 4, 0, 0, 0) == 4, "send failed");
  memset(out, 0xAA, sizeof out);
  r = SC4(N_epoll_pwait, ep, out, 4, 1000);
  CHECK(r == 1, "epoll_pwait -> %ld (want 1)", r);
  CHECK((out[0].events & LEPOLLIN) != 0, "events=%#x has no EPOLLIN", out[0].events);
  CHECK(out[0].data == MAGIC, "data=%#llx want %#llx",
        (unsigned long long)out[0].data, (unsigned long long)MAGIC);
  CHECK(out[0].__pad == 0, "pad word not cleared (%#x)", out[0].__pad);

  // byte-level proof that data really sits at +8 and events at +0
  const uint8_t* raw = (const uint8_t*)&out[0];
  uint32_t ev0; uint64_t data0;
  memcpy(&ev0, raw + 0, 4);
  memcpy(&data0, raw + 8, 8);
  CHECK(ev0 == out[0].events && data0 == MAGIC, "raw bytes disagree with the struct");
  OK("event: events=%#x @+0, pad=0 @+4, data=%#llx @+8", ev0, (unsigned long long)data0);

  // both directions on one fd must coalesce into ONE epoll_event
  struct lin_epoll_event evrw = { LEPOLLIN | LEPOLLOUT, 0, MAGIC };
  r = SC4(N_epoll_ctl, ep, 3 /*MOD*/, sv[1], &evrw);
  CHECK(r == 0, "epoll_ctl MOD -> %ld", r);
  memset(out, 0, sizeof out);
  r = SC4(N_epoll_pwait, ep, out, 4, 1000);
  CHECK(r == 1, "coalesced pwait -> %ld (want 1 event for 2 kqueue filters)", r);
  CHECK((out[0].events & LEPOLLIN) && (out[0].events & LEPOLLOUT),
        "coalesced events=%#x, want IN|OUT", out[0].events);
  OK("EVFILT_READ + EVFILT_WRITE coalesced into one event (events=%#x)", out[0].events);

  r = SC4(N_epoll_ctl, ep, LEPOLL_CTL_DEL, sv[1], 0);
  CHECK(r == 0, "epoll_ctl DEL -> %ld", r);
  r = SC4(N_epoll_ctl, ep, LEPOLL_CTL_DEL, sv[1], 0);
  CHECK(r == -LENOENT, "DEL of an unregistered fd -> %ld (want -2)", r);
  r = SC4(N_epoll_pwait, ep, out, 4, 0);
  CHECK(r == 0, "after DEL, pwait -> %ld (want 0)", r);
  OK("ADD/MOD/DEL bookkeeping matches epoll error semantics");
  close((int)ep); close(sv[0]); close(sv[1]);
}

// ---------------------------------------------------------------------------
static void t_ppoll(void) {
  SECTION("(f) ppoll / pselect6");
  int sv[2];
  CHECK(SC4(N_socketpair, LAF_UNIX, LSOCK_STREAM, 0, sv) == 0, "socketpair failed");

  struct pollfd pf = { sv[1], LPOLLIN, 0 };
  struct lin_timespec ts = { 0, 0 };
  long r = SC5(N_ppoll, &pf, 1, &ts, 0, 0);
  CHECK(r == 0, "ppoll with nothing pending -> %ld (want 0)", r);

  CHECK(SC6(N_sendto, sv[0], "z", 1, 0, 0, 0) == 1, "send failed");
  pf.revents = 0;
  ts.tv_sec = 1;
  r = SC5(N_ppoll, &pf, 1, &ts, 0, 0);
  CHECK(r == 1, "ppoll -> %ld (want 1)", r);
  CHECK((pf.revents & LPOLLIN) != 0, "revents=%#x has no POLLIN", pf.revents);
  OK("ppoll reported POLLIN (revents=%#x) with a 1s timespec", pf.revents);

  // NULL timespec == wait forever; data is already queued so it returns at once
  pf.revents = 0;
  r = SC5(N_ppoll, &pf, 1, 0, 0, 0);
  CHECK(r == 1 && (pf.revents & LPOLLIN), "ppoll(NULL timeout) -> %ld revents=%#x", r, pf.revents);

  fd_set rfds;
  FD_ZERO(&rfds); FD_SET(sv[1], &rfds);
  struct lin_timespec z = { 1, 0 };
  r = SC6(N_pselect6, sv[1] + 1, &rfds, 0, 0, &z, 0);
  CHECK(r == 1, "pselect6 -> %ld (want 1)", r);
  CHECK(FD_ISSET(sv[1], &rfds), "pselect6 did not mark the fd readable");
  OK("pselect6 reported 1 readable fd");
  close(sv[0]); close(sv[1]);
}

// ---------------------------------------------------------------------------
// The sibling of t_eventfd, and it exists because the suite asserted pollability for
// eventfd and NEVER for pipe2 -- while a self-pipe wakeup is the fallback half of
// GLib/NSPR's event loops, so a pipe that could not be polled would present as an
// application spinning forever rather than as a syscall returning wrong.
//
// THE NEGATIVE CONTROL IS THE TEST. Poll BEFORE writing and require NOT-readable:
// without it, a ppoll that always answered "ready" would sail through a
// write-then-poll check. Found while chasing a Firefox event loop that turned out
// NOT to be this -- the property holds -- so this is a regression guard, not a fix.
static void t_pipe_pollable(void) {
  SECTION("(g2) pipe2 — a written pipe must be reported readable by ppoll");
  int fds[2] = { -1, -1 };
  long r = SC2(N_pipe2, fds, 0);
  CHECK(r == 0 && fds[0] >= 0 && fds[1] >= 0, "pipe2 -> %ld (fds %d,%d)", r, fds[0], fds[1]);

  fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
  fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);

  // CONTROL: nothing written yet, so this MUST time out.
  struct pollfd pf = { fds[0], LPOLLIN, 0 };
  struct lin_timespec zero = { 0, 50 * 1000 * 1000 };      // 50 ms
  r = SC5(N_ppoll, &pf, 1, &zero, 0, 0);
  CHECK(r == 0, "CONTROL FAILED: empty pipe reported readable (r=%ld revents=%#x) -- "
                "a ppoll that always says ready would pass the rest of this test", r, pf.revents);
  OK("control: empty pipe is not readable");

  CHECK(write(fds[1], "x", 1) == 1, "write(pipe) failed: %s", strerror(errno));

  pf.revents = 0;
  struct lin_timespec ts = { 1, 0 };
  r = SC5(N_ppoll, &pf, 1, &ts, 0, 0);
  CHECK(r == 1 && (pf.revents & LPOLLIN), "written pipe not pollable: r=%ld revents=%#x — "
        "every self-pipe wakeup would spin forever", r, pf.revents);
  OK("written pipe is pollable (revents=%#x)", pf.revents);

  char b = 0;
  CHECK(read(fds[0], &b, 1) == 1 && b == 'x', "byte did not round-trip (got %#x)", (unsigned)b);
  OK("byte round-tripped through the pipe");
  close(fds[0]); close(fds[1]);
}

// ---------------------------------------------------------------------------
static void t_eventfd(void) {
  SECTION("(g) eventfd2 — write/read round-trip and pollability");
  long efd = SC2(N_eventfd2, 0, 0);
  CHECK(efd >= 0, "eventfd2 -> %ld", efd);

  uint64_t v = 42;
  // The guest's read()/write() go to syscalls.c, so the plain host calls are the real path.
  CHECK(write((int)efd, &v, sizeof v) == (ssize_t)sizeof v, "write(eventfd) failed: %s", strerror(errno));

  struct pollfd pf = { (int)efd, LPOLLIN, 0 };
  struct lin_timespec ts = { 1, 0 };
  long r = SC5(N_ppoll, &pf, 1, &ts, 0, 0);
  CHECK(r == 1 && (pf.revents & LPOLLIN), "eventfd not pollable: r=%ld revents=%#x", r, pf.revents);
  OK("eventfd is pollable (revents=%#x)", pf.revents);

  uint64_t got = 0;
  CHECK(read((int)efd, &got, sizeof got) == (ssize_t)sizeof got, "read(eventfd) failed: %s", strerror(errno));
  CHECK(got == 42, "eventfd read back %llu, want 42", (unsigned long long)got);
  OK("counter round-tripped: wrote 42, read %llu", (unsigned long long)got);

  // drained: a nonblocking eventfd must now report EAGAIN
  long efd2 = SC2(N_eventfd2, 7, LEFD_NONBLOCK);
  CHECK(efd2 >= 0, "eventfd2(initval=7, EFD_NONBLOCK) -> %ld", efd2);
  CHECK((fcntl((int)efd2, F_GETFL, 0) & O_NONBLOCK) != 0, "EFD_NONBLOCK not applied");
  got = 0;
  CHECK(read((int)efd2, &got, sizeof got) == (ssize_t)sizeof got && got == 7,
        "initval not readable, got %llu", (unsigned long long)got);
  errno = 0;
  ssize_t n = read((int)efd2, &got, sizeof got);
  CHECK(n < 0 && errno == EAGAIN, "drained nonblocking eventfd -> %zd errno=%d", n, errno);
  OK("initval delivered, then EAGAIN once drained");
  close((int)efd); close((int)efd2);
}

// ---------------------------------------------------------------------------
static void t_pipe2(void) {
  SECTION("(h) pipe2 flags");
  int fds[2] = { -1, -1 };
  long r = SC2(N_pipe2, fds, LO_CLOEXEC);
  CHECK(r == 0, "pipe2(O_CLOEXEC) -> %ld", r);
  CHECK((fcntl(fds[0], F_GETFD, 0) & FD_CLOEXEC) != 0, "read end lacks FD_CLOEXEC");
  CHECK((fcntl(fds[1], F_GETFD, 0) & FD_CLOEXEC) != 0, "write end lacks FD_CLOEXEC");
  OK("linux O_CLOEXEC(0x80000) -> FD_CLOEXEC on both ends");
  CHECK(write(fds[1], "p", 1) == 1, "pipe write failed");
  char c = 0;
  CHECK(read(fds[0], &c, 1) == 1 && c == 'p', "pipe read failed");
  close(fds[0]); close(fds[1]);

  int f2[2];
  r = SC2(N_pipe2, f2, 0x800 /*O_NONBLOCK*/);
  CHECK(r == 0, "pipe2(O_NONBLOCK) -> %ld", r);
  CHECK((fcntl(f2[0], F_GETFL, 0) & O_NONBLOCK) != 0, "read end not O_NONBLOCK");
  CHECK((fcntl(f2[0], F_GETFD, 0) & FD_CLOEXEC) == 0, "FD_CLOEXEC set when not asked for");
  OK("linux O_NONBLOCK(0x800) -> O_NONBLOCK, and only that");
  close(f2[0]); close(f2[1]);
}

// ---------------------------------------------------------------------------
static void t_sockopt_af(void) {
  SECTION("(i) socket options + AF_INET6 translation");
  long fd = SC3(N_socket, LAF_INET, LSOCK_STREAM, 0);
  CHECK(fd >= 0, "socket(AF_INET) -> %ld", fd);

  int on = 1;
  long r = SC5(N_setsockopt, fd, LSOL_SOCKET, LSO_REUSEADDR, &on, sizeof on);
  CHECK(r == 0, "setsockopt(linux SO_REUSEADDR=2 -> darwin 0x0004) -> %ld", r);
  int back = 0; uint32_t blen = sizeof back;
  r = SC5(N_getsockopt, fd, LSOL_SOCKET, LSO_REUSEADDR, &back, &blen);
  CHECK(r == 0 && back != 0, "getsockopt SO_REUSEADDR -> %ld val=%d", r, back);
  OK("SO_REUSEADDR 2 -> 0x0004 set and read back");

  int err = -1; uint32_t elen = sizeof err;
  r = SC5(N_getsockopt, fd, LSOL_SOCKET, LSO_ERROR, &err, &elen);
  CHECK(r == 0 && err == 0, "getsockopt(linux SO_ERROR=4 -> darwin 0x1007) -> %ld val=%d", r, err);
  OK("SO_ERROR 4 -> 0x1007, value 0 (errno-translated on the way out)");

  r = SC5(N_setsockopt, fd, LSOL_SOCKET, LSO_PASSCRED, &on, sizeof on);
  CHECK(r == 0, "SO_PASSCRED (no macOS analog) -> %ld (want 0, accepted+ignored)", r);
  OK("SO_PASSCRED accepted as a no-op");
  close((int)fd);

  long s6 = SC3(N_socket, LAF_INET6, LSOCK_DGRAM, 0);
  CHECK(s6 >= 0, "socket(linux AF_INET6=10 -> darwin 30) -> %ld", s6);
  uint8_t a6[28]; memset(a6, 0, sizeof a6);
  uint16_t fam = LAF_INET6; memcpy(a6, &fam, 2);          // port 0, addr ::
  a6[23] = 1;                                             // ::1
  r = SC3(N_bind, s6, a6, sizeof a6);
  CHECK(r == 0, "bind(::1) -> %ld", r);
  uint8_t self[64]; memset(self, 0xAA, sizeof self);
  uint32_t slen = sizeof self;
  r = SC3(N_getsockname, s6, self, &slen);
  uint16_t gotfam = 0; memcpy(&gotfam, self, 2);
  CHECK(r == 0 && gotfam == LAF_INET6, "getsockname family=%u want 10", gotfam);
  CHECK(slen == 28, "sockaddr_in6 linux length %u want 28", slen);
  OK("AF_INET6 reported back to the guest as 10, len 28");
  close((int)s6);

  long bad = SC3(N_socket, 16 /*AF_NETLINK*/, LSOCK_DGRAM, 0);
  CHECK(bad == -97, "AF_NETLINK -> %ld (want -97 EAFNOSUPPORT)", bad);
  OK("unmappable address family -> -EAFNOSUPPORT(-97)");
}

// ---------------------------------------------------------------------------
static void t_cmsg(void) {
  SECTION("(j) sendmsg/recvmsg with SCM_RIGHTS across the 16-vs-12 byte cmsghdr gap");
  int sv[2];
  CHECK(SC4(N_socketpair, LAF_UNIX, LSOCK_STREAM, 0, sv) == 0, "socketpair failed");
  int pfd[2];
  CHECK(SC2(N_pipe2, pfd, 0) == 0, "pipe2 failed");

  char iobuf[1] = { 'F' };
  struct iovec iov = { iobuf, 1 };
  uint8_t ctl[64]; memset(ctl, 0, sizeof ctl);
  struct lin_cmsghdr* lc = (struct lin_cmsghdr*)ctl;
  lc->cmsg_len = sizeof(struct lin_cmsghdr) + sizeof(int);   // LINUX CMSG_LEN: 16 + 4
  lc->cmsg_level = LSOL_SOCKET;                              // 1, not 0xffff
  lc->cmsg_type = LSCM_RIGHTS;
  memcpy(ctl + sizeof(struct lin_cmsghdr), &pfd[0], sizeof(int));

  struct lin_msghdr mh; memset(&mh, 0, sizeof mh);
  mh.msg_iov = (uint64_t)(uintptr_t)&iov;
  mh.msg_iovlen = 1;
  mh.msg_control = (uint64_t)(uintptr_t)ctl;
  mh.msg_controllen = lc->cmsg_len;
  long n = SC3(N_sendmsg, sv[0], &mh, 0);
  CHECK(n == 1, "sendmsg -> %ld", n);

  char rbuf[1] = { 0 };
  struct iovec riov = { rbuf, 1 };
  uint8_t rctl[128]; memset(rctl, 0, sizeof rctl);
  struct lin_msghdr rmh; memset(&rmh, 0, sizeof rmh);
  rmh.msg_iov = (uint64_t)(uintptr_t)&riov;
  rmh.msg_iovlen = 1;
  rmh.msg_control = (uint64_t)(uintptr_t)rctl;
  rmh.msg_controllen = sizeof rctl;
  n = SC3(N_recvmsg, sv[1], &rmh, 0);
  CHECK(n == 1 && rbuf[0] == 'F', "recvmsg -> %ld data='%c'", n, rbuf[0]);

  struct lin_cmsghdr* rc = (struct lin_cmsghdr*)rctl;
  CHECK(rmh.msg_controllen >= sizeof(struct lin_cmsghdr) + sizeof(int),
        "controllen=%llu too small", (unsigned long long)rmh.msg_controllen);
  CHECK(rc->cmsg_level == LSOL_SOCKET, "cmsg_level=%d want 1 (linux SOL_SOCKET)", rc->cmsg_level);
  CHECK(rc->cmsg_type == LSCM_RIGHTS, "cmsg_type=%d want 1", rc->cmsg_type);
  CHECK(rc->cmsg_len == sizeof(struct lin_cmsghdr) + sizeof(int),
        "cmsg_len=%llu want 20 (linux layout)", (unsigned long long)rc->cmsg_len);

  int gotfd = -1;
  memcpy(&gotfd, rctl + sizeof(struct lin_cmsghdr), sizeof gotfd);
  CHECK(gotfd >= 0, "no fd received");
  if (gotfd >= 0) {
    CHECK(write(pfd[1], "R", 1) == 1, "pipe write failed");
    char c = 0;
    CHECK(read(gotfd, &c, 1) == 1 && c == 'R', "the passed fd does not work");
    OK("fd %d passed through SCM_RIGHTS and reads from the original pipe", gotfd);
    close(gotfd);
  }
  close(sv[0]); close(sv[1]); close(pfd[0]); close(pfd[1]);
}

// ---------------------------------------------------------------------------
static void t_ownership(void) {
  SECTION("net_owns_syscall coverage");
  static const unsigned long owned[] = { 19,20,21,22,59,72,73,198,199,200,201,202,203,204,
                                         205,206,207,208,209,210,211,212,242 };
  for (size_t i = 0; i < sizeof owned / sizeof *owned; i++)
    CHECK(net_owns_syscall(owned[i]), "net_owns_syscall(%lu) should be true", owned[i]);
  // Anything else must fall through to linux_syscall().
  static const unsigned long foreign[] = { 0,17,18,23,24,25,56,57,62,63,64,66,78,93,94,
                                           129,213,241,243,260,435,441 };
  for (size_t i = 0; i < sizeof foreign / sizeof *foreign; i++)
    CHECK(!net_owns_syscall(foreign[i]), "net_owns_syscall(%lu) should be false", foreign[i]);
  OK("owns exactly 23 syscall numbers (incl. pipe2=59), rejects everything else");
}

int main(void) {
  mkdir("/Users/dperry/linux-shim/tests", 0755);
  mkdir(SCRATCH, 0755);
  mkdir(PREFIX, 0755);
  net_init();
  net_set_unix_prefix(PREFIX);
  printf("net.c test — prefix=%s\n", PREFIX);

  t_ownership();
  t_socketpair();
  t_unix_prefix();
  t_abstract();
  t_nonblock();
  t_epoll();
  t_ppoll();
  t_pipe_pollable();
  t_eventfd();
  t_pipe2();
  t_sockopt_af();
  t_cmsg();

  printf("\n================================\n");
  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  printf("%s\n", g_fail ? "FAILED" : "ALL GREEN");
  return g_fail ? 1 : 0;
}
