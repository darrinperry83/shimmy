// net.c — Linux sockets / poll / epoll / pipe / eventfd -> macOS (Apple Silicon).
//
// This is how a guest X11/Wayland client reaches our in-process server over a unix socket, so it
// is routed BEFORE linux_syscall() in the trap handler (see net_owns_syscall).
//
// Everything here returns the raw Linux ABI: >= 0 on success, NEGATIVE LINUX errno on failure.
// Host (Darwin) errnos are translated with the local table below — deliberately a private copy so
// this file links standalone (syscalls.c exports the same mapping as linux_errno_from_host).
//
// The hard parts, and why the obvious memcpy is wrong:
//   * struct sockaddr: Linux has {u16 family; ...}, Darwin has {u8 len; u8 family; ...}. On
//     little-endian the family byte lands in a different slot, so EVERY address is rebuilt field
//     by field, never copied.
//   * AF_INET6 is 10 on Linux and 30 on Darwin.
//   * SOCK_NONBLOCK/SOCK_CLOEXEC (and the EFD_*/O_* twins) are OR-ed into the type/flags argument
//     on Linux; Darwin has no such bits — they are masked off and re-applied with fcntl().
//   * Socket option NAMES differ (SO_REUSEADDR 2 vs 0x0004, SO_ERROR 4 vs 0x1007, ...) and
//     SOL_SOCKET itself is 1 on Linux, 0xffff on Darwin.
//   * struct msghdr is 56 bytes on Linux and 48 on Darwin (msg_iovlen/msg_controllen are size_t
//     vs int/socklen_t), and struct cmsghdr is 16 bytes/8-aligned vs 12 bytes/4-aligned. Both are
//     rebuilt, including SCM_RIGHTS fd passing.
//   * Linux abstract unix sockets (sun_path[0]==0) do not exist on macOS at all, so their name is
//     hex-encoded into a real file under the relocation prefix.
//   * epoll is emulated on kqueue. The aarch64 struct epoll_event is NOT packed (unlike x86_64):
//     { u32 events; u32 pad; u64 data; } == 16 bytes with data at offset 8.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include "net.h"
#include "snd_alsa.h"

// ---------------------------------------------------------------------------
// Linux aarch64 syscall numbers we own
// ---------------------------------------------------------------------------
enum {
  LSYS_eventfd2      = 19,
  LSYS_epoll_create1 = 20,
  LSYS_epoll_ctl     = 21,
  LSYS_epoll_pwait   = 22,
  LSYS_pipe2         = 59,
  LSYS_pselect6      = 72,
  LSYS_ppoll         = 73,
  LSYS_socket        = 198,
  LSYS_socketpair    = 199,
  LSYS_bind          = 200,
  LSYS_listen        = 201,
  LSYS_accept        = 202,
  LSYS_connect       = 203,
  LSYS_getsockname   = 204,
  LSYS_getpeername   = 205,
  LSYS_sendto        = 206,
  LSYS_recvfrom      = 207,
  LSYS_setsockopt    = 208,
  LSYS_getsockopt    = 209,
  LSYS_shutdown      = 210,
  LSYS_sendmsg       = 211,
  LSYS_recvmmsg      = 243,
  LSYS_sendmmsg      = 269,
  LSYS_recvmsg       = 212,
  LSYS_accept4       = 242,
};

// ---------------------------------------------------------------------------
// Linux constants (values as seen by the guest)
// ---------------------------------------------------------------------------
#define L_EPERM 1
#define L_ENOENT 2
#define L_EINTR 4
#define L_EBADF 9
#define L_EAGAIN 11
#define L_ENOMEM 12
#define L_EACCES 13
#define L_EFAULT 14
#define L_EEXIST 17
#define L_EINVAL 22
#define L_EMFILE 24
#define L_ENAMETOOLONG 36
#define L_ENOSYS 38
#define L_ENOTSOCK 88
#define L_ENOPROTOOPT 92
#define L_EAFNOSUPPORT 97
#define L_EOPNOTSUPP 95
#define L_ERANGE 34

#define L_SOCK_NONBLOCK 0x0800
#define L_SOCK_CLOEXEC  0x80000
#define L_O_NONBLOCK    0x0800
#define L_O_CLOEXEC     0x80000
#define L_O_DIRECT      0x4000   // pipe2 packet mode — ignored
#define L_EFD_SEMAPHORE 0x0001
#define L_EFD_NONBLOCK  0x0800
#define L_EFD_CLOEXEC   0x80000
#define L_EPOLL_CLOEXEC 0x80000

#define L_AF_UNSPEC 0
#define L_AF_UNIX   1
#define L_AF_INET   2
#define L_AF_INET6  10
#define L_AF_NETLINK 16
#define L_AF_PACKET 17

#define L_SOL_SOCKET 1
#define L_SOL_IP     0
#define L_SOL_TCP    6
#define L_SOL_IPV6   41

#define L_EPOLL_CTL_ADD 1
#define L_EPOLL_CTL_DEL 2
#define L_EPOLL_CTL_MOD 3

#define L_EPOLLIN        0x00000001u
#define L_EPOLLPRI       0x00000002u
#define L_EPOLLOUT       0x00000004u
#define L_EPOLLERR       0x00000008u
#define L_EPOLLHUP       0x00000010u
#define L_EPOLLRDNORM    0x00000040u
#define L_EPOLLRDBAND    0x00000080u
#define L_EPOLLWRNORM    0x00000100u
#define L_EPOLLWRBAND    0x00000200u
#define L_EPOLLRDHUP     0x00002000u
#define L_EPOLLEXCLUSIVE 0x10000000u
#define L_EPOLLWAKEUP    0x20000000u
#define L_EPOLLONESHOT   0x40000000u
#define L_EPOLLET        0x80000000u

#define L_POLLIN     0x0001
#define L_POLLPRI    0x0002
#define L_POLLOUT    0x0004
#define L_POLLERR    0x0008
#define L_POLLHUP    0x0010
#define L_POLLNVAL   0x0020
#define L_POLLRDNORM 0x0040
#define L_POLLRDBAND 0x0080
#define L_POLLWRNORM 0x0100
#define L_POLLWRBAND 0x0200

#define L_MSG_OOB          0x00000001
#define L_MSG_PEEK         0x00000002
#define L_MSG_DONTROUTE    0x00000004
#define L_MSG_CTRUNC       0x00000008
#define L_MSG_TRUNC        0x00000020
#define L_MSG_DONTWAIT     0x00000040
#define L_MSG_EOR          0x00000080
#define L_MSG_WAITALL      0x00000100
#define L_MSG_NOSIGNAL     0x00004000
#define L_MSG_MORE         0x00008000
#define L_MSG_WAITFORONE   0x00010000   // recvmmsg only: after the first message, do not block
#define L_MSG_CMSG_CLOEXEC 0x40000000

#define L_SCM_RIGHTS      1
#define L_SCM_CREDENTIALS 2

// Guest-side structures. Layouts are the aarch64 Linux ones, NOT the host ones.
struct lin_sockaddr    { uint16_t sa_family; char sa_data[126]; };
struct lin_sockaddr_un { uint16_t sun_family; char sun_path[108]; };

// aarch64 does NOT use __attribute__((packed)) for epoll_event (x86_64 does): 16 bytes, data at 8.
struct lin_epoll_event { uint32_t events; uint32_t __pad; uint64_t data; };
_Static_assert(sizeof(struct lin_epoll_event) == 16, "aarch64 epoll_event must be 16 bytes");
_Static_assert(offsetof(struct lin_epoll_event, data) == 8, "aarch64 epoll_event.data must be @8");
_Static_assert(offsetof(struct lin_epoll_event, events) == 0, "epoll_event.events must be @0");

struct lin_msghdr {
  uint64_t msg_name;        // void*
  uint32_t msg_namelen;     // socklen_t
  uint32_t __pad1;
  uint64_t msg_iov;         // struct iovec* (identical layout on both sides)
  uint64_t msg_iovlen;      // size_t on Linux, int on Darwin
  uint64_t msg_control;     // void*
  uint64_t msg_controllen;  // size_t on Linux, socklen_t on Darwin
  int32_t  msg_flags;
  uint32_t __pad2;
};
_Static_assert(sizeof(struct lin_msghdr) == 56, "linux msghdr is 56 bytes on LP64");

struct lin_cmsghdr { uint64_t cmsg_len; int32_t cmsg_level; int32_t cmsg_type; };
_Static_assert(sizeof(struct lin_cmsghdr) == 16, "linux cmsghdr is 16 bytes on LP64");
#define LCMSG_ALIGN(n) (((size_t)(n) + 7u) & ~(size_t)7u)
#define LCMSG_HDRLEN   (sizeof(struct lin_cmsghdr))

struct lin_timespec { int64_t tv_sec; int64_t tv_nsec; };  // identical to Darwin's on arm64

// ---------------------------------------------------------------------------
// errno: Darwin -> Linux
// ---------------------------------------------------------------------------
// 1..34 are identical EXCEPT that 11 and 35 are swapped (Darwin 11=EDEADLK/35=EAGAIN,
// Linux 11=EAGAIN/35=EDEADLK). Everything above that diverges completely.
static int lin_errno(int he) {
  if (he <= 0) return 0;
  switch (he) {
    case 11:  return 35;   // EDEADLK
    case 35:  return 11;   // EAGAIN / EWOULDBLOCK
    case 36:  return 115;  // EINPROGRESS
    case 37:  return 114;  // EALREADY
    case 38:  return 88;   // ENOTSOCK
    case 39:  return 89;   // EDESTADDRREQ
    case 40:  return 90;   // EMSGSIZE
    case 41:  return 91;   // EPROTOTYPE
    case 42:  return 92;   // ENOPROTOOPT
    case 43:  return 93;   // EPROTONOSUPPORT
    case 44:  return 94;   // ESOCKTNOSUPPORT
    case 45:  return 95;   // ENOTSUP -> EOPNOTSUPP
    case 46:  return 96;   // EPFNOSUPPORT
    case 47:  return 97;   // EAFNOSUPPORT
    case 48:  return 98;   // EADDRINUSE
    case 49:  return 99;   // EADDRNOTAVAIL
    case 50:  return 100;  // ENETDOWN
    case 51:  return 101;  // ENETUNREACH
    case 52:  return 102;  // ENETRESET
    case 53:  return 103;  // ECONNABORTED
    case 54:  return 104;  // ECONNRESET
    case 55:  return 105;  // ENOBUFS
    case 56:  return 106;  // EISCONN
    case 57:  return 107;  // ENOTCONN
    case 58:  return 108;  // ESHUTDOWN
    case 59:  return 109;  // ETOOMANYREFS
    case 60:  return 110;  // ETIMEDOUT
    case 61:  return 111;  // ECONNREFUSED
    case 62:  return 40;   // ELOOP
    case 63:  return 36;   // ENAMETOOLONG
    case 64:  return 112;  // EHOSTDOWN
    case 65:  return 113;  // EHOSTUNREACH
    case 66:  return 39;   // ENOTEMPTY
    case 67:  return 11;   // EPROCLIM -> EAGAIN
    case 68:  return 87;   // EUSERS
    case 69:  return 122;  // EDQUOT
    case 70:  return 116;  // ESTALE
    case 71:  return 66;   // EREMOTE
    case 72: case 73: case 74: case 75: case 76: return 22;  // E*RPC* -> EINVAL
    case 77:  return 37;   // ENOLCK
    case 78:  return 38;   // ENOSYS
    case 79:  return 22;   // EFTYPE
    case 80: case 81: return 1;   // EAUTH/ENEEDAUTH -> EPERM
    case 82: case 83: return 5;   // EPWROFF/EDEVERR -> EIO
    case 84:  return 75;   // EOVERFLOW
    case 85: case 86: case 88: return 8;  // EBADEXEC/EBADARCH/EBADMACHO -> ENOEXEC
    case 87:  return 22;   // ESHLIBVERS
    case 89:  return 125;  // ECANCELED
    case 90:  return 43;   // EIDRM
    case 91:  return 42;   // ENOMSG
    case 92:  return 84;   // EILSEQ
    case 93:  return 61;   // ENOATTR -> ENODATA
    case 94:  return 74;   // EBADMSG
    case 95:  return 72;   // EMULTIHOP
    case 96:  return 61;   // ENODATA
    case 97:  return 67;   // ENOLINK
    case 98:  return 63;   // ENOSR
    case 99:  return 60;   // ENOSTR
    case 100: return 71;   // EPROTO
    case 101: return 62;   // ETIME
    case 102: return 95;   // EOPNOTSUPP
    case 103: return 1;    // ENOPOLICY -> EPERM
    case 104: return 131;  // ENOTRECOVERABLE
    case 105: return 130;  // EOWNERDEAD
    case 106: return 105;  // EQFULL -> ENOBUFS
    default: break;
  }
  if (he <= 34) return he;
  return L_EINVAL;
}
static long lerr(void) { return -(long)lin_errno(errno); }
static long lret(long r) { return r < 0 ? lerr() : r; }

// ---------------------------------------------------------------------------
// RDPOLLLOG=1 — wait-accuracy diagnostic (off by default, one cached branch when off)
// ---------------------------------------------------------------------------
// Prints, for every ppoll/pselect6/epoll_pwait, the timeout the GUEST asked for (plus the raw
// timespec it came from) and the wall-clock time actually spent. It exists because "the app
// freezes" reads exactly like "our poll oversleeps", and it settles that in one run: if
// requested and actual agree, the wait layer is not the problem and the stall is the guest's
// own scheduling. See KNOWN_BUGS.md, "xeyes' ~755 ms stall".
#include <mach/mach_time.h>
static int g_polllog = -1;
static inline int polllog_on(void) {
  if (g_polllog < 0) { const char* s = getenv("RDPOLLLOG"); g_polllog = (s && *s && *s != '0'); }
  return g_polllog;
}
static inline double now_ms(void) {
  static mach_timebase_info_data_t tb;
  if (!tb.denom) mach_timebase_info(&tb);
  return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

// ---------------------------------------------------------------------------
// address family / socket type / protocol
// ---------------------------------------------------------------------------
static int af_l2h(int laf) {
  switch (laf) {
    case L_AF_UNSPEC: return AF_UNSPEC;
    case L_AF_UNIX:   return AF_UNIX;    // 1 == 1
    case L_AF_INET:   return AF_INET;    // 2 == 2
    case L_AF_INET6:  return AF_INET6;   // 10 -> 30
    default:          return -1;
  }
}
static int af_h2l(int haf) {
  switch (haf) {
    case AF_UNSPEC: return L_AF_UNSPEC;
    case AF_UNIX:   return L_AF_UNIX;
    case AF_INET:   return L_AF_INET;
    case AF_INET6:  return L_AF_INET6;   // 30 -> 10
    default:        return -1;
  }
}

// SOCK_STREAM/DGRAM/RAW/RDM/SEQPACKET are 1..5 on both sides; only the flag bits differ.
static int apply_fd_flags(int fd, int nonblock, int cloexec) {
  if (fd < 0) return fd;
  if (nonblock) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  }
  if (cloexec) {
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
  }
  return fd;
}
// Guest processes expect EPIPE, not a fatal SIGPIPE delivered to the host process.
static void kill_sigpipe(int fd) { int on = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on); }

// ---------------------------------------------------------------------------
// unix-socket path relocation
// ---------------------------------------------------------------------------
static char g_prefix[512];
static size_t g_prefix_len;
// Optional socket namespace shared by one app and its exec'd service processes.
// Display sockets stay per process because fork children restart their compositors.
static char g_group_prefix[512];
static size_t g_group_prefix_len;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void net_set_unix_prefix(const char* host_prefix) {
  if (!host_prefix) host_prefix = "";
  snprintf(g_prefix, sizeof g_prefix, "%s", host_prefix);
  size_t n = strlen(g_prefix);
  while (n > 1 && g_prefix[n - 1] == '/') g_prefix[--n] = 0;
  if (n == 1 && g_prefix[0] == '/') { g_prefix[0] = 0; n = 0; }  // "/" == no relocation
  g_prefix_len = n;
}

static int mkdir_p(const char* dir) {
  char tmp[PATH_MAX];
  if (!dir || !*dir) return 0;
  size_t n = strlen(dir);
  if (n >= sizeof tmp) { errno = ENAMETOOLONG; return -1; }
  memcpy(tmp, dir, n + 1);
  while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
  for (char* p = tmp + 1; *p; p++) {
    if (*p != '/') continue;
    *p = 0;
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) { *p = '/'; return -1; }
    *p = '/';
  }
  if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
  return 0;
}
static int mkdir_parent(const char* path) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof tmp, "%s", path);
  char* slash = strrchr(tmp, '/');
  if (!slash || slash == tmp) return 0;
  *slash = 0;
  return mkdir_p(tmp);
}

// Named sockets in a private group may not traverse dot components or a
// pre-existing symlinked directory.  In particular, mkdir_parent() must not
// create directories outside the group through such a link.  The group itself
// is a trusted, private directory supplied by the session launcher.
static int group_guest_path_safe(const char* path) {
  char prefix[PATH_MAX];
  size_t used = strlen(g_group_prefix);
  if (used >= sizeof prefix) return 0;
  memcpy(prefix, g_group_prefix, used + 1);
  const char* p = path;
  while (*p == '/') p++;
  while (*p) {
    const char* slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    if (!len || (len == 1 && p[0] == '.') ||
        (len == 2 && p[0] == '.' && p[1] == '.')) return 0;
    if (used + 1 + len >= sizeof prefix) return 0;
    prefix[used++] = '/'; memcpy(prefix + used, p, len); used += len;
    prefix[used] = 0;
    if (slash) {
      struct stat st;
      if (lstat(prefix, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) return 0;
      } else if (errno != ENOENT) return 0;
      p = slash + 1;
      if (!*p) return 0;
    } else break;
  }
  return 1;
}

// Where hex-encoded abstract names and the eventfd rendezvous sockets live.
static void private_dir(char* out, size_t osz, const char* leaf) {
  if (g_prefix_len) snprintf(out, osz, "%s/.rdx-%s", g_prefix, leaf);
  else {
    const char* t = getenv("TMPDIR"); if (!t || !*t) t = "/tmp";
    size_t n = strlen(t);
    snprintf(out, osz, "%s%s.rdx-%s", t, (n && t[n - 1] == '/') ? "" : "/", leaf);
  }
}

static const char k_hex[] = "0123456789abcdef";
static void hex_encode(const uint8_t* in, size_t n, char* out) {
  for (size_t i = 0; i < n; i++) { out[2 * i] = k_hex[in[i] >> 4]; out[2 * i + 1] = k_hex[in[i] & 15]; }
  out[2 * n] = 0;
}
static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static long hex_decode(const char* in, uint8_t* out, size_t ocap) {
  size_t n = strlen(in);
  if (n & 1) return -1;
  n /= 2;
  if (n > ocap) return -1;
  for (size_t i = 0; i < n; i++) {
    int hi = hex_val(in[2 * i]), lo = hex_val(in[2 * i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return (long)n;
}

// A guest sun_path -> a host filesystem path.
//   ""                 (autobind)        -> <prefix>/.rdx-abstract/<hex of a generated name>
//   "\0name"           (abstract)        -> <prefix>/.rdx-abstract/<hex of name>
//   "/tmp/.X11-unix/X0"(filesystem path) -> <prefix>/tmp/.X11-unix/X0
// *abstract is set when the result lives in the emulated abstract namespace (which has no
// persistence on Linux, so a stale host file there must be removed before bind).
static long unix_path_to_host(const char* sp, size_t splen, int for_bind,
                              char* out, size_t osz, int* abstract) {
  *abstract = 0;
  if (splen == 0 || (splen >= 1 && sp[0] == 0)) {
    uint8_t name[128];
    size_t nlen;
    if (splen == 0) {                        // autobind: Linux picks a free 5-hex-digit name
      static unsigned counter;
      pthread_mutex_lock(&g_lock);
      unsigned c = ++counter;
      pthread_mutex_unlock(&g_lock);
      nlen = (size_t)snprintf((char*)name, sizeof name, "%05x", (unsigned)((getpid() << 8) ^ c) & 0xfffff);
    } else {                                 // abstract: bytes after the leading NUL
      nlen = splen - 1;
      while (nlen > 0 && sp[nlen] == 0) nlen--;   // tolerate callers that pass sizeof(sockaddr_un)
      if (nlen > sizeof name) return -L_ENAMETOOLONG;
      memcpy(name, sp + 1, nlen);
    }
    char dir[PATH_MAX]; private_dir(dir, sizeof dir, "abstract");
    if (mkdir_p(dir) < 0 && errno != EEXIST) return lerr();
    char hex[2 * 128 + 1];
    hex_encode(name, nlen, hex);
    int n = snprintf(out, osz, "%s/%s", dir, hex);
    if (n < 0 || (size_t)n >= osz) return -L_ENAMETOOLONG;
    *abstract = 1;
    return 0;
  }
  size_t plen = strnlen(sp, splen);
  char path[PATH_MAX];
  if (plen >= sizeof path) return -L_ENAMETOOLONG;
  memcpy(path, sp, plen); path[plen] = 0;
  int n;
  int display_path = !strcmp(path, "/tmp/wayland-0") ||
                     !strcmp(path, "/tmp/.X11-unix/X0");
  if (g_group_prefix_len && path[0] == '/' && !display_path) {
    if (!group_guest_path_safe(path)) return -L_EACCES;
    n = snprintf(out, osz, "%s%s", g_group_prefix, path);
  }
  else if (g_prefix_len && path[0] == '/') n = snprintf(out, osz, "%s%s", g_prefix, path);
  else n = snprintf(out, osz, "%s", path);
  if (n < 0 || (size_t)n >= osz) return -L_ENAMETOOLONG;
  if (for_bind) mkdir_parent(out);
  return 0;
}

// The inverse, for getsockname/getpeername/accept/recvfrom.
// Returns the number of bytes of sun_path content (with the leading NUL for abstract names).
static size_t unix_path_to_guest(const char* hp, char* out, size_t osz) {
  char adir[PATH_MAX]; private_dir(adir, sizeof adir, "abstract");
  size_t alen = strlen(adir);
  if (!strncmp(hp, adir, alen) && hp[alen] == '/') {
    uint8_t name[128];
    long n = hex_decode(hp + alen + 1, name, sizeof name);
    if (n >= 0 && (size_t)n + 1 <= osz) { out[0] = 0; memcpy(out + 1, name, (size_t)n); return (size_t)n + 1; }
  }
  const char* g = hp;
  if (g_group_prefix_len && !strncmp(hp, g_group_prefix, g_group_prefix_len) &&
      hp[g_group_prefix_len] == '/') g = hp + g_group_prefix_len;
  if (g_prefix_len && !strncmp(hp, g_prefix, g_prefix_len) && hp[g_prefix_len] == '/') g = hp + g_prefix_len;
  size_t n = strlen(g);
  if (n + 1 > osz) n = osz - 1;
  memcpy(out, g, n); out[n] = 0;
  return n + 1;   // include the terminating NUL, as Linux does for pathname sockets
}

// ---------------------------------------------------------------------------
// sockaddr: guest (Linux) <-> host (Darwin)
// ---------------------------------------------------------------------------
static long sa_to_host(const void* lsa, uint32_t llen, int for_bind,
                       struct sockaddr_storage* hs, socklen_t* hlen) {
  if (!lsa || llen < 2) return -L_EINVAL;
  uint16_t lfam;
  memcpy(&lfam, lsa, 2);
  int hfam = af_l2h((int)lfam);
  if (hfam < 0) return -L_EAFNOSUPPORT;
  memset(hs, 0, sizeof *hs);

  if (hfam == AF_UNIX) {
    const struct lin_sockaddr_un* lu = (const struct lin_sockaddr_un*)lsa;
    size_t splen = (llen > 2) ? (size_t)llen - 2 : 0;
    if (splen > sizeof lu->sun_path) splen = sizeof lu->sun_path;
    char host[PATH_MAX];
    int abstract = 0;
    long r = unix_path_to_host(lu->sun_path, splen, for_bind, host, sizeof host, &abstract);
    if (r < 0) return r;
    struct sockaddr_un* hu = (struct sockaddr_un*)hs;
    size_t n = strlen(host);
    if (n >= sizeof hu->sun_path) return -L_ENAMETOOLONG;   // Darwin sun_path is 104, Linux 108
    hu->sun_family = AF_UNIX;
    memcpy(hu->sun_path, host, n + 1);
    hu->sun_len = (unsigned char)(offsetof(struct sockaddr_un, sun_path) + n + 1);
    *hlen = (socklen_t)hu->sun_len;
    // Linux's abstract namespace has no persistence: a stale host file would wrongly EADDRINUSE.
    if (for_bind && abstract) unlink(host);
    return 0;
  }
  if (hfam == AF_INET) {
    if (llen < 16) return -L_EINVAL;
    struct sockaddr_in* hi = (struct sockaddr_in*)hs;
    const uint8_t* p = (const uint8_t*)lsa;
    hi->sin_len = sizeof *hi;
    hi->sin_family = AF_INET;
    memcpy(&hi->sin_port, p + 2, 2);        // already network byte order
    memcpy(&hi->sin_addr, p + 4, 4);
    *hlen = sizeof *hi;
    return 0;
  }
  if (hfam == AF_INET6) {
    if (llen < 24) return -L_EINVAL;
    struct sockaddr_in6* h6 = (struct sockaddr_in6*)hs;
    const uint8_t* p = (const uint8_t*)lsa;
    h6->sin6_len = sizeof *h6;
    h6->sin6_family = AF_INET6;
    memcpy(&h6->sin6_port, p + 2, 2);
    memcpy(&h6->sin6_flowinfo, p + 4, 4);
    memcpy(&h6->sin6_addr, p + 8, 16);
    if (llen >= 28) memcpy(&h6->sin6_scope_id, p + 24, 4);
    *hlen = sizeof *h6;
    return 0;
  }
  if (hfam == AF_UNSPEC) { hs->ss_family = AF_UNSPEC; hs->ss_len = 2; *hlen = 2; return 0; }
  return -L_EAFNOSUPPORT;
}

// Writes the Linux-layout address into the guest buffer (truncating as Linux does), and reports
// the FULL length it would have needed via *needed.
static long sa_to_guest(const struct sockaddr* hs, socklen_t hslen, void* out, uint32_t cap,
                        uint32_t* needed) {
  uint8_t buf[160];
  size_t len;
  if (!hs || hslen < (socklen_t)sizeof(sa_family_t)) { *needed = 0; return 0; }
  memset(buf, 0, sizeof buf);
  int lfam = af_h2l(hs->sa_family);
  if (lfam < 0) return -L_EAFNOSUPPORT;
  uint16_t f = (uint16_t)lfam;
  memcpy(buf, &f, 2);

  if (hs->sa_family == AF_UNIX) {
    const struct sockaddr_un* hu = (const struct sockaddr_un*)hs;
    size_t plen = 0;
    if (hslen > (socklen_t)offsetof(struct sockaddr_un, sun_path)) {
      char hp[PATH_MAX];
      size_t avail = (size_t)hslen - offsetof(struct sockaddr_un, sun_path);
      if (avail > sizeof hp - 1) avail = sizeof hp - 1;
      size_t n = strnlen(hu->sun_path, avail);
      memcpy(hp, hu->sun_path, n); hp[n] = 0;
      if (n) plen = unix_path_to_guest(hp, (char*)buf + 2, sizeof buf - 2);
    }
    len = 2 + plen;
  } else if (hs->sa_family == AF_INET) {
    const struct sockaddr_in* hi = (const struct sockaddr_in*)hs;
    memcpy(buf + 2, &hi->sin_port, 2);
    memcpy(buf + 4, &hi->sin_addr, 4);
    len = 16;
  } else if (hs->sa_family == AF_INET6) {
    const struct sockaddr_in6* h6 = (const struct sockaddr_in6*)hs;
    memcpy(buf + 2, &h6->sin6_port, 2);
    memcpy(buf + 4, &h6->sin6_flowinfo, 4);
    memcpy(buf + 8, &h6->sin6_addr, 16);
    memcpy(buf + 24, &h6->sin6_scope_id, 4);
    len = 28;
  } else {
    len = 2;
  }
  *needed = (uint32_t)len;
  if (out && cap) memcpy(out, buf, len < cap ? len : cap);
  return 0;
}

// getsockname/getpeername/accept/recvfrom share this out-parameter dance.
static long store_addr(const struct sockaddr* hs, socklen_t hslen,
                       unsigned long uaddr, unsigned long ulen) {
  if (!uaddr || !ulen) return 0;
  uint32_t* plen = (uint32_t*)ulen;
  uint32_t cap = *plen, needed = 0;
  long r = sa_to_guest(hs, hslen, (void*)uaddr, cap, &needed);
  if (r < 0) return r;
  *plen = needed;
  return 0;
}

// ---------------------------------------------------------------------------
// socket options
// ---------------------------------------------------------------------------
#define OPT_NOOP  (-1)   // silently accepted, reads back as zero
#define OPT_BAD   (-2)   // -ENOPROTOOPT

struct optmap { int lname; int hname; };

static const struct optmap k_sol_socket[] = {
  {  1, SO_DEBUG       },  // SO_DEBUG        0x0001
  {  2, SO_REUSEADDR   },  // SO_REUSEADDR    0x0004
  {  3, SO_TYPE        },  // SO_TYPE         0x1008
  {  4, SO_ERROR       },  // SO_ERROR        0x1007  (value itself is translated)
  {  5, SO_DONTROUTE   },  // SO_DONTROUTE    0x0010
  {  6, SO_BROADCAST   },  // SO_BROADCAST    0x0020
  {  7, SO_SNDBUF      },  // SO_SNDBUF       0x1001
  {  8, SO_RCVBUF      },  // SO_RCVBUF       0x1002
  {  9, SO_KEEPALIVE   },  // SO_KEEPALIVE    0x0008
  { 10, SO_OOBINLINE   },  // SO_OOBINLINE    0x0100
  { 11, OPT_NOOP       },  // SO_NO_CHECK
  { 12, OPT_NOOP       },  // SO_PRIORITY
  { 13, SO_LINGER      },  // SO_LINGER       0x0080 (struct linger matches)
  { 14, OPT_NOOP       },  // SO_BSDCOMPAT
  { 15, SO_REUSEPORT   },  // SO_REUSEPORT    0x0200
  { 16, OPT_NOOP       },  // SO_PASSCRED     — no macOS analog
  { 17, OPT_BAD        },  // SO_PEERCRED     — LOCAL_PEERCRED has a different struct
  { 18, SO_RCVLOWAT    },  // SO_RCVLOWAT     0x1004
  { 19, SO_SNDLOWAT    },  // SO_SNDLOWAT     0x1003
  { 20, SO_RCVTIMEO    },  // SO_RCVTIMEO     0x1006 (timeval is converted)
  { 21, SO_SNDTIMEO    },  // SO_SNDTIMEO     0x1005
  { 29, SO_TIMESTAMP   },  // SO_TIMESTAMP    0x0400
  { 30, SO_ACCEPTCONN  },  // SO_ACCEPTCONN   0x0002
  { 31, OPT_BAD        },  // SO_PEERSEC
  { 32, SO_SNDBUF      },  // SO_SNDBUFFORCE
  { 33, SO_RCVBUF      },  // SO_RCVBUFFORCE
  // SO_PASSSEC is the SELinux twin of SO_PASSCRED, and OPT_NOOP is FAITHFUL here where it was not
  // there: on a kernel with no LSM, Linux itself attaches no SCM_SECURITY, so "accepted, and
  // nothing arrives" is exactly what the caller would see on Linux. Keep it, but keep it for that
  // reason and not because it looks harmless. (OPT_NOOP sweep, W2.)
  { 34, OPT_NOOP       },  // SO_PASSSEC     — faithful: no LSM, so Linux sends no label either
  { 35, OPT_NOOP       },  // SO_TIMESTAMPNS — emulated, intercepted in do_setsockopt()
  // SO_PROTOCOL / SO_DOMAIN / SO_PEERSEC stay OPT_BAD deliberately. Refusing an option is HONEST:
  // the caller sees ENOPROTOOPT and takes its fallback. That is the whole lesson of this sweep --
  // the dangerous answer is not "no", it is "yes" followed by nothing.
  { 38, OPT_BAD        },  // SO_PROTOCOL
  { 39, OPT_BAD        },  // SO_DOMAIN
};
// ---- OPT_NOOP audit (W2) -------------------------------------------------------------------
// The rule this table now follows, after SO_PASSCRED cost a day:
//
//   An option that sets a MODE or a HINT may be a no-op -- the caller cannot observe the
//   difference, or Linux on this configuration would not produce one either.
//   An option by which the caller ASKS FOR DATA may NOT be a no-op. Accepting it promises that
//   data will arrive; when it does not, the caller fails somewhere else entirely, with a message
//   that blames the wrong component. SO_PASSCRED failed three layers away inside WebKit, in an
//   error string that accused the child process of crashing.
//
// Audited, SOL_SOCKET: 11 SO_NO_CHECK, 12 SO_PRIORITY, 14 SO_BSDCOMPAT are hints and stay NOOP
// (SO_BSDCOMPAT is a no-op on Linux too). 16 SO_PASSCRED and 29/35 SO_TIMESTAMP(NS) are
// data-requests and are now emulated. 34 SO_PASSSEC is a data-request but faithful, see above.
// 17 SO_PEERCRED, 31 SO_PEERSEC, 38 SO_PROTOCOL, 39 SO_DOMAIN are getsockopt-only and answer or
// refuse honestly. SOL_IP 11 IP_RECVERR is a hint, and its NOOP is load-bearing (see below).
// SOL_TCP 3 TCP_CORK was a hint with a real analogue and is now mapped to TCP_NOPUSH.
static const struct optmap k_sol_ip[] = {
  {  1, IP_TOS }, {  2, IP_TTL }, {  3, IP_HDRINCL }, {  4, IP_OPTIONS },
  // 11 = IP_RECVERR. Darwin has no equivalent, and NOOP (not BAD) is load-bearing: glibc's
  // resolv/res_send.c reopen() treats setsockopt(IP_RECVERR) failing as FATAL -- it calls
  // __res_iclose() and returns 0, so the socket is closed before any bind/connect/sendto and
  // EVERY name lookup fails. musl never sets it, which is the only reason Alpine never hit this.
  // IP_RECVERR only asks for extended error reporting, so silently succeeding is correct: the
  // resolver just falls back to ordinary timeouts. (Found by B2 on Debian glibc 2.36.)
  { 11, OPT_NOOP },
  { 32, IP_MULTICAST_IF }, { 33, IP_MULTICAST_TTL }, { 34, IP_MULTICAST_LOOP },
  { 35, IP_ADD_MEMBERSHIP }, { 36, IP_DROP_MEMBERSHIP },
};
static const struct optmap k_sol_tcp[] = {
  // TCP_CORK had no analogue "because macOS has none". It does: TCP_NOPUSH is the BSD spelling of
  // exactly this -- hold partial segments until the option is cleared, then flush. A caller that
  // corks, writes a header and a body, and uncorks is relying on the coalescing; as OPT_NOOP it
  // got two segments instead of one. Not a correctness bug, but it is another promise the shim
  // was making and not keeping, and the analogue was one line away. (OPT_NOOP sweep, W2.)
  { 1, TCP_NODELAY }, { 2, TCP_MAXSEG }, { 3, TCP_NOPUSH /*TCP_CORK*/ },
  { 4, TCP_KEEPALIVE /*TCP_KEEPIDLE*/ }, { 5, TCP_KEEPINTVL }, { 6, TCP_KEEPCNT },
};
static const struct optmap k_sol_ipv6[] = {
  { 16, IPV6_UNICAST_HOPS }, { 17, IPV6_MULTICAST_IF }, { 18, IPV6_MULTICAST_HOPS },
  { 19, IPV6_MULTICAST_LOOP }, { 20, IPV6_JOIN_GROUP }, { 21, IPV6_LEAVE_GROUP },
  { 26, IPV6_V6ONLY },
};

static int lookup_opt(const struct optmap* t, size_t n, int lname) {
  for (size_t i = 0; i < n; i++) if (t[i].lname == lname) return t[i].hname;
  return OPT_BAD;
}
// Returns 0 and fills *hlevel/*hname, or a negative Linux errno, or 1 for "pretend success".
static long opt_l2h(int llevel, int lname, int* hlevel, int* hname) {
  const struct optmap* t; size_t n;
  switch (llevel) {
    case L_SOL_SOCKET: *hlevel = SOL_SOCKET;   t = k_sol_socket; n = sizeof k_sol_socket / sizeof *k_sol_socket; break;
    case L_SOL_IP:     *hlevel = IPPROTO_IP;   t = k_sol_ip;     n = sizeof k_sol_ip     / sizeof *k_sol_ip;     break;
    case L_SOL_TCP:    *hlevel = IPPROTO_TCP;  t = k_sol_tcp;    n = sizeof k_sol_tcp    / sizeof *k_sol_tcp;    break;
    case L_SOL_IPV6:   *hlevel = IPPROTO_IPV6; t = k_sol_ipv6;   n = sizeof k_sol_ipv6   / sizeof *k_sol_ipv6;   break;
    default: return -L_ENOPROTOOPT;
  }
  int h = lookup_opt(t, n, lname);
  if (h == OPT_NOOP) return 1;
  if (h == OPT_BAD)  return -L_ENOPROTOOPT;
  *hname = h;
  return 0;
}

// --- SO_PASSCRED / SCM_CREDENTIALS -----------------------------------------------------------
// Linux delivers the sender's {pid,uid,gid} as an SCM_CREDENTIALS control message once the
// RECEIVER has asked for it with SO_PASSCRED. Darwin has no such thing, so the option was mapped
// OPT_NOOP -- accepted, and then no credentials ever arrive.
//
// That silent success is not harmless. WebKitGTK's process launcher learns the auxiliary
// process's pid exactly this way: setsockopt(SO_PASSCRED) then recvmsg, and readPIDFromPeer()
// calls g_error() -- which aborts -- when the credentials are not there. So MiniBrowser's UI
// process killed itself before the first paint, and the message it printed blamed the child
// ("This usually means the auxiliary process crashed immediately"), which was false: the child
// was alive and still initialising JavaScriptCore.
//
// Darwin CAN answer the question, just not through the ancillary-data mechanism: a connected
// AF_UNIX socket knows its peer, via getsockopt(SOL_LOCAL, LOCAL_PEERPID) and getpeereid(). So
// remember which fds asked for SO_PASSCRED and synthesise the cmsg on the way out.
#define PASSCRED_MAX 64
static int g_passcred[PASSCRED_MAX];
static int g_passcred_n;
static void passcred_set(int fd, int on) {
  for (int i = 0; i < g_passcred_n; i++)
    if (g_passcred[i] == fd) { if (!on) g_passcred[i] = g_passcred[--g_passcred_n]; return; }
  if (on && g_passcred_n < PASSCRED_MAX) g_passcred[g_passcred_n++] = fd;
}
static int passcred_get(int fd) {
  for (int i = 0; i < g_passcred_n; i++) if (g_passcred[i] == fd) return 1;
  return 0;
}
// socketpair() bookkeeping. macOS will not tell us who holds the other end of a socketpair --
// getsockopt(SOL_LOCAL, LOCAL_PEERPID) is ENOTCONN, MEASURED, for both SOCK_STREAM and SOCK_DGRAM
// -- and there is no working sender-stamped alternative either: Darwin defines the SCM_CREDS
// constant but guards `struct cmsgcred` with #ifndef __APPLE__ and does not implement it.
//
// The shim does not need to ask, though. It CREATED the pair, and it SPAWNS the process that
// inherits one end, so it can simply remember. When a child is spawned, whichever end is not
// FD_CLOEXEC goes to that child, which makes the child the peer of the OTHER end.
#define SPAIR_MAX 128
struct spair { int a, b; int peer_of_a, peer_of_b; };   // peer_of_X = pid holding the far side of X
static struct spair g_spair[SPAIR_MAX];
static int g_spair_n;
static void spair_add(int a, int b) {
  if (g_spair_n < SPAIR_MAX) { g_spair[g_spair_n].a = a; g_spair[g_spair_n].b = b;
                               g_spair[g_spair_n].peer_of_a = 0; g_spair[g_spair_n].peer_of_b = 0;
                               g_spair_n++; }
}
static int spair_peer_pid(int fd) {
  for (int i = 0; i < g_spair_n; i++) {
    if (g_spair[i].a == fd) return g_spair[i].peer_of_a;
    if (g_spair[i].b == fd) return g_spair[i].peer_of_b;
  }
  return 0;
}
void net_note_spawned_child(int pid) {
  if (pid <= 0) return;
  for (int i = 0; i < g_spair_n; i++) {
    // An fd WITHOUT FD_CLOEXEC survives the exec, so the child now holds that end; this process
    // keeps the other one, whose peer is therefore the child.
    int fa = fcntl(g_spair[i].a, F_GETFD, 0), fb = fcntl(g_spair[i].b, F_GETFD, 0);
    if (fa >= 0 && !(fa & FD_CLOEXEC)) g_spair[i].peer_of_b = pid;
    if (fb >= 0 && !(fb & FD_CLOEXEC)) g_spair[i].peer_of_a = pid;
  }
  if (getenv("RDCREDLOG")) fprintf(stderr, "[cred] spawned child pid=%d\n", pid);
}
struct lin_ucred { int32_t pid; uint32_t uid; uint32_t gid; };
// Fills *out with the peer's credentials. Returns 1 on success, 0 if this fd cannot answer.
static int peer_ucred(int fd, struct lin_ucred* out) {
  pid_t ppid = 0; socklen_t pl = sizeof ppid;
  int rc = getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &ppid, &pl);
  if (rc != 0 || ppid <= 0) ppid = (pid_t)spair_peer_pid(fd);   // socketpair: use what we recorded
  if (getenv("RDCREDLOG"))
    fprintf(stderr, "[cred] peer fd=%d getsockopt_rc=%d -> pid=%d\n", fd, rc, (int)ppid);
  if (ppid <= 0) return 0;
  uid_t uid = 0; gid_t gid = 0;
  if (getpeereid(fd, &uid, &gid) != 0) { uid = getuid(); gid = getgid(); }
  // The host getpeereid() returns the HOST uid (e.g. 501), but the guest believes it is uid 0
  // (RDUID0=1, see syscalls.c case 174). D-Bus' EXTERNAL auth has the client send getuid() and
  // the server verify it against SO_PEERCRED; a host/guest mismatch makes dbus-daemon reject every
  // connection ("Transport endpoint is not connected"). Report the guest's OWN view so both sides
  // of the socket agree.
  if (getenv("RDUID0")) { uid = 0; gid = 0; }
  out->pid = (int32_t)ppid; out->uid = (uint32_t)uid; out->gid = (uint32_t)gid;
  return 1;
}

// ---------------------------------------------------------------------------
// SO_TIMESTAMP / SO_TIMESTAMPNS -- the same shape of bug as SO_PASSCRED, found by the OPT_NOOP
// sweep. These two are the receiver ASKING FOR DATA, not setting a mode, so accepting them and
// delivering nothing is a broken promise, not a harmless approximation:
//
//   * SO_TIMESTAMPNS (35) was a flat OPT_NOOP -- accepted, and no timestamp ever arrives.
//   * SO_TIMESTAMP (29) looked correct because the table maps it to Darwin's SO_TIMESTAMP, but
//     cmsg_h2l() then DROPPED the resulting control message on the `else if (cmsg_level ==
//     SOL_SOCKET) continue;` line, whose comment read "no Linux analog (SCM_TIMESTAMP, ...)".
//     There is an analog; the two differ only in the cmsg TYPE number and the struct layout.
//     So the option worked all the way to the kernel and the answer was thrown away at the door.
//
// Both are now recorded per-fd, both set Darwin SO_TIMESTAMP underneath, and cmsg_h2l() converts
// Darwin's SCM_TIMESTAMP into whichever spelling this fd asked for.
#define L_SCM_TIMESTAMP   29                     // == SO_TIMESTAMP on Linux
#define L_SCM_TIMESTAMPNS 35                     // == SO_TIMESTAMPNS on Linux
#define TSMODE_MAX 64
static struct { int fd; int ns; } g_tsmode[TSMODE_MAX];
static int g_tsmode_n;
static void tsmode_set(int fd, int on, int ns) {
  for (int i = 0; i < g_tsmode_n; i++)
    if (g_tsmode[i].fd == fd) {
      if (!on) g_tsmode[i] = g_tsmode[--g_tsmode_n]; else g_tsmode[i].ns = ns;
      return;
    }
  if (on && g_tsmode_n < TSMODE_MAX) { g_tsmode[g_tsmode_n].fd = fd; g_tsmode[g_tsmode_n].ns = ns;
                                       g_tsmode_n++; }
}
// -1 = not requested, 0 = SO_TIMESTAMP (timeval), 1 = SO_TIMESTAMPNS (timespec)
static int tsmode_get(int fd) {
  for (int i = 0; i < g_tsmode_n; i++) if (g_tsmode[i].fd == fd) return g_tsmode[i].ns;
  return -1;
}

struct lin_timeval { int64_t tv_sec; int64_t tv_usec; };   // Linux suseconds_t is 64-bit
struct lin_timespec64 { int64_t tv_sec; int64_t tv_nsec; };
static long do_setsockopt(int fd, int llevel, int lname, const void* val, uint32_t len) {
  int hlevel = 0, hname = 0;
  // SO_PASSCRED is still a no-op AT THE HOST -- Darwin has no such option -- but it is no longer
  // a no-op to US: it is the receiver announcing that it wants SCM_CREDENTIALS, and recvmsg
  // synthesises them. Recorded before the generic OPT_NOOP path swallows it.
  if (llevel == L_SOL_SOCKET && lname == 16) {
    int on = 1;
    if (val && len >= sizeof(int)) { int v; memcpy(&v, val, sizeof v); on = (v != 0); }
    passcred_set(fd, on);
    return 0;
  }
  // SO_TIMESTAMP (29) / SO_TIMESTAMPNS (35): record which spelling was asked for, then set
  // Darwin's SO_TIMESTAMP underneath. cmsg_h2l() converts the reply. See the block above.
  if (llevel == L_SOL_SOCKET && (lname == L_SCM_TIMESTAMP || lname == L_SCM_TIMESTAMPNS)) {
    int on = 1;
    if (val && len >= sizeof(int)) { int v; memcpy(&v, val, sizeof v); on = (v != 0); }
    tsmode_set(fd, on, lname == L_SCM_TIMESTAMPNS);
    int hv = on;
    return lret(setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &hv, sizeof hv));
  }
  long m = opt_l2h(llevel, lname, &hlevel, &hname);
  if (m < 0) return m;
  if (m == 1) return 0;                     // accepted and ignored (SO_PASSCRED & friends)
  if (hlevel == SOL_SOCKET && (hname == SO_RCVTIMEO || hname == SO_SNDTIMEO)) {
    if (!val || len < sizeof(struct lin_timeval)) return -L_EINVAL;
    const struct lin_timeval* lt = (const struct lin_timeval*)val;
    struct timeval tv = { (time_t)lt->tv_sec, (suseconds_t)lt->tv_usec };
    return lret(setsockopt(fd, hlevel, hname, &tv, sizeof tv));
  }
  return lret(setsockopt(fd, hlevel, hname, val, (socklen_t)len));
}
static long do_getsockopt(int fd, int llevel, int lname, void* val, uint32_t* len) {
  int hlevel = 0, hname = 0;
  // SO_PEERCRED. Darwin's LOCAL_PEERCRED returns a struct xucred, which is NOT Linux's ucred, so
  // the option was refused outright. The three fields Linux wants are all obtainable, though:
  // LOCAL_PEERPID for the pid and getpeereid() for uid/gid.
  if (llevel == L_SOL_SOCKET && lname == 17) {
    struct lin_ucred uc;
    if (!val || !len || *len < sizeof uc) return -L_EINVAL;
    if (!peer_ucred(fd, &uc)) return -L_ENOPROTOOPT;
    memcpy(val, &uc, sizeof uc);
    *len = (uint32_t)sizeof uc;
    return 0;
  }
  if (llevel == L_SOL_SOCKET && lname == 16) {     // SO_PASSCRED: report what we recorded
    if (val && len && *len >= 4) { uint32_t v = (uint32_t)passcred_get(fd); memcpy(val, &v, 4); *len = 4; }
    else if (len) *len = 0;
    return 0;
  }
  long m = opt_l2h(llevel, lname, &hlevel, &hname);
  if (m < 0) return m;
  if (m == 1) {                             // no analog: report zero
    if (val && len && *len >= 4) { uint32_t z = 0; memcpy(val, &z, 4); *len = 4; }
    else if (len) *len = 0;
    return 0;
  }
  if (hlevel == SOL_SOCKET && hname == SO_ERROR) {   // the value is a host errno — translate it
    int v = 0; socklen_t sl = sizeof v;
    if (getsockopt(fd, hlevel, hname, &v, &sl) < 0) return lerr();
    int lv = v ? lin_errno(v) : 0;
    if (!val || !len || *len < 4) return -L_EINVAL;
    memcpy(val, &lv, 4); *len = 4;
    return 0;
  }
  if (hlevel == SOL_SOCKET && (hname == SO_RCVTIMEO || hname == SO_SNDTIMEO)) {
    struct timeval tv; socklen_t sl = sizeof tv;
    if (getsockopt(fd, hlevel, hname, &tv, &sl) < 0) return lerr();
    if (!val || !len || *len < sizeof(struct lin_timeval)) return -L_EINVAL;
    struct lin_timeval lt = { (int64_t)tv.tv_sec, (int64_t)tv.tv_usec };
    memcpy(val, &lt, sizeof lt); *len = (uint32_t)sizeof lt;
    return 0;
  }
  socklen_t sl = len ? (socklen_t)*len : 0;
  if (getsockopt(fd, hlevel, hname, val, &sl) < 0) return lerr();
  if (len) *len = (uint32_t)sl;
  return 0;
}

// ---------------------------------------------------------------------------
// msg flags
// ---------------------------------------------------------------------------
static int msgflags_l2h(int lf) {
  int h = 0;
  if (lf & L_MSG_OOB)       h |= MSG_OOB;
  if (lf & L_MSG_PEEK)      h |= MSG_PEEK;
  if (lf & L_MSG_DONTROUTE) h |= MSG_DONTROUTE;
  if (lf & L_MSG_EOR)       h |= MSG_EOR;        // 0x80 -> 0x8
  if (lf & L_MSG_DONTWAIT)  h |= MSG_DONTWAIT;   // 0x40 -> 0x80
  if (lf & L_MSG_WAITALL)   h |= MSG_WAITALL;    // 0x100 -> 0x40
#ifdef MSG_NOSIGNAL
  if (lf & L_MSG_NOSIGNAL)  h |= MSG_NOSIGNAL;   // 0x4000 -> 0x80000
#endif
  // L_MSG_TRUNC (recv "give me the real length") and L_MSG_MORE have no Darwin input analog.
  return h;
}
static int msgflags_h2l(int hf) {
  int l = 0;
  if (hf & MSG_OOB)    l |= L_MSG_OOB;
  if (hf & MSG_EOR)    l |= L_MSG_EOR;      // 0x8 -> 0x80
  if (hf & MSG_TRUNC)  l |= L_MSG_TRUNC;    // 0x10 -> 0x20
  if (hf & MSG_CTRUNC) l |= L_MSG_CTRUNC;   // 0x20 -> 0x8
  return l;
}

// ---------------------------------------------------------------------------
// control messages (SCM_RIGHTS)
// ---------------------------------------------------------------------------
// Linux cmsghdr: {u64 len; i32 level; i32 type} = 16 bytes, 8-aligned.
// Darwin cmsghdr: {u32 len; i32 level; i32 type} = 12 bytes, 4-aligned.
static long cmsg_l2h(const uint8_t* lc, size_t llen, uint8_t** out, size_t* outlen) {
  *out = NULL; *outlen = 0;
  if (!lc || llen < LCMSG_HDRLEN) return 0;

  size_t total = 0;
  const uint8_t* p = lc; size_t left = llen;
  while (left >= LCMSG_HDRLEN) {                       // pass 1: size the host buffer
    struct lin_cmsghdr h; memcpy(&h, p, sizeof h);
    if (h.cmsg_len < LCMSG_HDRLEN || h.cmsg_len > left) break;
    size_t dlen = (size_t)h.cmsg_len - LCMSG_HDRLEN;
    if (h.cmsg_level == L_SOL_SOCKET && h.cmsg_type == L_SCM_RIGHTS) total += CMSG_SPACE(dlen);
    size_t adv = LCMSG_ALIGN(h.cmsg_len);
    if (adv >= left) break;
    p += adv; left -= adv;
  }
  if (!total) return 0;

  uint8_t* hb = (uint8_t*)calloc(1, total);
  if (!hb) return -L_ENOMEM;
  struct msghdr shim; memset(&shim, 0, sizeof shim);
  shim.msg_control = hb; shim.msg_controllen = (socklen_t)total;
  struct cmsghdr* hc = CMSG_FIRSTHDR(&shim);

  p = lc; left = llen;
  while (left >= LCMSG_HDRLEN && hc) {                 // pass 2: fill it
    struct lin_cmsghdr h; memcpy(&h, p, sizeof h);
    if (h.cmsg_len < LCMSG_HDRLEN || h.cmsg_len > left) break;
    size_t dlen = (size_t)h.cmsg_len - LCMSG_HDRLEN;
    if (h.cmsg_level == L_SOL_SOCKET && h.cmsg_type == L_SCM_RIGHTS) {
      hc->cmsg_len = CMSG_LEN(dlen);
      hc->cmsg_level = SOL_SOCKET;                     // 1 -> 0xffff
      hc->cmsg_type = SCM_RIGHTS;
      memcpy(CMSG_DATA(hc), p + LCMSG_HDRLEN, dlen);   // guest fds ARE host fds here
      hc = CMSG_NXTHDR(&shim, hc);
    }
    // SCM_CREDENTIALS and anything else is dropped: no Darwin equivalent.
    size_t adv = LCMSG_ALIGN(h.cmsg_len);
    if (adv >= left) break;
    p += adv; left -= adv;
  }
  *out = hb; *outlen = total;
  return 0;
}

// Host control buffer -> guest (Linux layout). Returns bytes written; sets *truncated.
static size_t cmsg_h2l(int fd, struct msghdr* hm, uint8_t* lc, size_t lcap, int cmsg_cloexec,
                       int* truncated, int* gave_creds) {
  size_t used = 0;
  *truncated = 0;
  *gave_creds = 0;
  for (struct cmsghdr* hc = CMSG_FIRSTHDR(hm); hc; hc = CMSG_NXTHDR(hm, hc)) {
    if (hc->cmsg_len < sizeof(struct cmsghdr)) break;
    size_t dlen = (size_t)hc->cmsg_len - CMSG_LEN(0);
    int llevel = (hc->cmsg_level == SOL_SOCKET) ? L_SOL_SOCKET : hc->cmsg_level;
    int ltype = hc->cmsg_type;
    if (hc->cmsg_level == SOL_SOCKET && hc->cmsg_type == SCM_RIGHTS) {
      ltype = L_SCM_RIGHTS;
      if (cmsg_cloexec) {                       // MSG_CMSG_CLOEXEC
        int nfd = (int)(dlen / sizeof(int));
        for (int i = 0; i < nfd; i++) {
          int fd; memcpy(&fd, CMSG_DATA(hc) + i * sizeof(int), sizeof fd);
          int fl = fcntl(fd, F_GETFD, 0);
          if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
        }
      }
    } else if (hc->cmsg_level == SOL_SOCKET && hc->cmsg_type == SCM_TIMESTAMP
               && tsmode_get(fd) >= 0 && dlen >= sizeof(struct timeval)) {
      // Darwin stamps a `struct timeval` whose tv_usec is 32-bit; Linux's is 64-bit, and
      // SO_TIMESTAMPNS wants a `struct timespec` in NANOseconds under a different cmsg type.
      // Rebuild rather than memcpy -- the payloads are the same SIZE (16 bytes) but not the same
      // LAYOUT, so a straight copy would deliver a plausible-looking wrong microsecond field.
      struct timeval htv; memcpy(&htv, CMSG_DATA(hc), sizeof htv);
      int ns = tsmode_get(fd);
      unsigned char pay[16];
      if (ns) { struct lin_timespec64 t = { (int64_t)htv.tv_sec, (int64_t)htv.tv_usec * 1000 };
                memcpy(pay, &t, sizeof t); }
      else    { struct lin_timeval t = { (int64_t)htv.tv_sec, (int64_t)htv.tv_usec };
                memcpy(pay, &t, sizeof t); }
      if (used + LCMSG_HDRLEN + sizeof pay > lcap) { *truncated = 1; break; }
      struct lin_cmsghdr lh;
      lh.cmsg_len = (uint64_t)(LCMSG_HDRLEN + sizeof pay);
      lh.cmsg_level = L_SOL_SOCKET;
      lh.cmsg_type = ns ? L_SCM_TIMESTAMPNS : L_SCM_TIMESTAMP;
      memcpy(lc + used, &lh, sizeof lh);
      memcpy(lc + used + LCMSG_HDRLEN, pay, sizeof pay);
      used += LCMSG_ALIGN(LCMSG_HDRLEN + sizeof pay);
      if (used > lcap) used = lcap;
      continue;
    } else if (hc->cmsg_level == SOL_SOCKET) {
      continue;                                 // genuinely no Linux analog (SCM_CREDS, ...)
    }
    if (used + LCMSG_HDRLEN + dlen > lcap) { *truncated = 1; break; }
    struct lin_cmsghdr lh;
    lh.cmsg_len = (uint64_t)(LCMSG_HDRLEN + dlen);
    lh.cmsg_level = llevel;
    lh.cmsg_type = ltype;
    memcpy(lc + used, &lh, sizeof lh);
    memcpy(lc + used + LCMSG_HDRLEN, CMSG_DATA(hc), dlen);
    used += LCMSG_ALIGN(LCMSG_HDRLEN + dlen);          // pad to the next Linux cmsg boundary
    if (used > lcap) used = lcap;
  }
  return used;
}

// ---------------------------------------------------------------------------
// epoll on kqueue
// ---------------------------------------------------------------------------
struct epent { int fd; uint32_t events; uint64_t data; };
struct epinst {
  struct epinst* next;
  int kq;
  struct epent* v;
  int n, cap;
};
static struct epinst* g_ep;

static struct epinst* ep_find(int kq) {
  for (struct epinst* e = g_ep; e; e = e->next) if (e->kq == kq) return e;
  return NULL;
}
static struct epinst* ep_get(int kq) {          // lazily adopt unknown (but plausible) epfds
  struct epinst* e = ep_find(kq);
  if (e) return e;
  e = (struct epinst*)calloc(1, sizeof *e);
  if (!e) return NULL;
  e->kq = kq; e->next = g_ep; g_ep = e;
  return e;
}
static void ep_drop(int kq) {                   // a recycled fd number must start clean
  struct epinst** pp = &g_ep;
  while (*pp) {
    if ((*pp)->kq == kq) { struct epinst* d = *pp; *pp = d->next; free(d->v); free(d); return; }
    pp = &(*pp)->next;
  }
}
static struct epent* ep_lookup(struct epinst* e, int fd) {
  for (int i = 0; i < e->n; i++) if (e->v[i].fd == fd) return &e->v[i];
  return NULL;
}
static struct epent* ep_add(struct epinst* e, int fd) {
  if (e->n == e->cap) {
    int nc = e->cap ? e->cap * 2 : 16;
    struct epent* nv = (struct epent*)realloc(e->v, (size_t)nc * sizeof *nv);
    if (!nv) return NULL;
    e->v = nv; e->cap = nc;
  }
  struct epent* p = &e->v[e->n++];
  p->fd = fd; p->events = 0; p->data = 0;
  return p;
}
static void ep_del(struct epinst* e, int fd) {
  for (int i = 0; i < e->n; i++)
    if (e->v[i].fd == fd) { e->v[i] = e->v[e->n - 1]; e->n--; return; }
}

#define EP_WANT_READ  (L_EPOLLIN | L_EPOLLPRI | L_EPOLLRDNORM | L_EPOLLRDBAND | L_EPOLLRDHUP)
#define EP_WANT_WRITE (L_EPOLLOUT | L_EPOLLWRNORM | L_EPOLLWRBAND)

static long ep_apply(int kq, int fd, uint32_t events) {
  struct kevent chg[2];
  int nadd = 0;
  unsigned short base = EV_ADD;
  if (events & L_EPOLLET) base |= EV_CLEAR;            // epoll is level-triggered by default
  if (events & L_EPOLLONESHOT) base |= EV_ONESHOT;

  if (events & EP_WANT_READ) EV_SET(&chg[nadd++], (uintptr_t)fd, EVFILT_READ, base, 0, 0, NULL);
  else { struct kevent d; EV_SET(&d, (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
         kevent(kq, &d, 1, NULL, 0, NULL); }           // ENOENT is fine
  if (events & EP_WANT_WRITE) EV_SET(&chg[nadd++], (uintptr_t)fd, EVFILT_WRITE, base, 0, 0, NULL);
  else { struct kevent d; EV_SET(&d, (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
         kevent(kq, &d, 1, NULL, 0, NULL); }

  if (nadd && kevent(kq, chg, nadd, NULL, 0, NULL) < 0) return lerr();
  return 0;
}
static void ep_unapply(int kq, int fd) {
  struct kevent d[2];
  EV_SET(&d[0], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
  EV_SET(&d[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
  kevent(kq, &d[0], 1, NULL, 0, NULL);
  kevent(kq, &d[1], 1, NULL, 0, NULL);
}

static long sys_epoll_create1(int flags) {
  int kq = kqueue();
  if (kq < 0) return lerr();
  apply_fd_flags(kq, 0, (flags & L_EPOLL_CLOEXEC) != 0);
  pthread_mutex_lock(&g_lock);
  ep_drop(kq);
  ep_get(kq);
  pthread_mutex_unlock(&g_lock);
  return kq;
}

static long sys_epoll_ctl(int epfd, int op, int fd, struct lin_epoll_event* uev) {
  if (epfd < 0 || fd < 0) return -L_EBADF;
  if (epfd == fd) return -L_EINVAL;
  struct lin_epoll_event ev = { 0, 0, 0 };
  if (op != L_EPOLL_CTL_DEL) {
    if (!uev) return -L_EFAULT;
    ev = *uev;
  }
  long r = 0;
  pthread_mutex_lock(&g_lock);
  struct epinst* e = ep_get(epfd);
  if (!e) { pthread_mutex_unlock(&g_lock); return -L_ENOMEM; }
  struct epent* p = ep_lookup(e, fd);
  switch (op) {
    case L_EPOLL_CTL_ADD:
      if (p) { r = -L_EEXIST; break; }
      p = ep_add(e, fd);
      if (!p) { r = -L_ENOMEM; break; }
      r = ep_apply(epfd, fd, ev.events);
      if (r < 0) { ep_del(e, fd); break; }
      p->events = ev.events; p->data = ev.data;
      break;
    case L_EPOLL_CTL_MOD:
      if (!p) { r = -L_ENOENT; break; }
      r = ep_apply(epfd, fd, ev.events);
      if (r < 0) break;
      p->events = ev.events; p->data = ev.data;
      break;
    case L_EPOLL_CTL_DEL:
      if (!p) { r = -L_ENOENT; break; }
      ep_unapply(epfd, fd);
      ep_del(e, fd);
      break;
    default:
      r = -L_EINVAL;
  }
  pthread_mutex_unlock(&g_lock);
  return r;
}

static long sys_epoll_pwait(int epfd, struct lin_epoll_event* uev, int maxevents, int timeout_ms) {
  if (maxevents <= 0) return -L_EINVAL;
  if (!uev) return -L_EFAULT;
  if (maxevents > 4096) maxevents = 4096;
  struct kevent* kv = (struct kevent*)calloc((size_t)maxevents, sizeof *kv);
  if (!kv) return -L_ENOMEM;
  struct timespec ts, *tsp = NULL;
  if (timeout_ms >= 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    tsp = &ts;
  }
  int* idents = (int*)calloc((size_t)maxevents, sizeof *idents);   // for coalescing by fd
  if (!idents) { free(kv); return -L_ENOMEM; }

  double t0 = polllog_on() ? now_ms() : 0;
  int n = kevent(epfd, NULL, 0, kv, maxevents, tsp);
  if (polllog_on()) fprintf(stderr, "[polllog] t=%.2f epoll_pwait req=%dms act=%.2fms ret=%d\n",
                            now_ms(), timeout_ms, now_ms() - t0, n);
  if (n < 0) { long e = lerr(); free(kv); free(idents); return e; }

  int out = 0;
  for (int i = 0; i < n; i++) {
    int fd = (int)kv[i].ident;
    uint32_t bits = 0;
    if (kv[i].filter == EVFILT_READ) {
      bits |= L_EPOLLIN | L_EPOLLRDNORM;
      if (kv[i].flags & EV_EOF) bits |= L_EPOLLRDHUP;
    } else if (kv[i].filter == EVFILT_WRITE) {
      bits |= L_EPOLLOUT | L_EPOLLWRNORM;
      if (kv[i].flags & EV_EOF) bits |= L_EPOLLHUP;
    }
    if ((kv[i].flags & EV_ERROR) && kv[i].data) bits |= L_EPOLLERR;
    if ((kv[i].flags & EV_EOF) && kv[i].fflags) bits |= L_EPOLLERR;

    int slot = -1;                              // coalesce both filters of one fd into one event
    for (int j = 0; j < out; j++) if (idents[j] == fd) { slot = j; break; }
    if (slot < 0) {
      if (out >= maxevents) continue;
      slot = out++;
      idents[slot] = fd;
      uev[slot].events = 0;
      uev[slot].__pad = 0;                      // the aarch64 pad word must be zero
      uev[slot].data = 0;
      pthread_mutex_lock(&g_lock);
      struct epinst* e = ep_find(epfd);
      struct epent* p = e ? ep_lookup(e, fd) : NULL;
      if (p) uev[slot].data = p->data;
      pthread_mutex_unlock(&g_lock);
    }
    uev[slot].events |= bits;
  }
  free(kv);
  free(idents);
  return out;
}

// ---------------------------------------------------------------------------
// poll / select
// ---------------------------------------------------------------------------
static short pollev_l2h(short le) {
  short h = 0;
  if (le & L_POLLIN)     h |= POLLIN;
  if (le & L_POLLPRI)    h |= POLLPRI;
  if (le & L_POLLOUT)    h |= POLLOUT;
  if (le & L_POLLRDNORM) h |= POLLRDNORM;
  if (le & L_POLLRDBAND) h |= POLLRDBAND;
  if (le & L_POLLWRNORM) h |= POLLWRNORM;   // 0x100 -> 0x004 (== POLLOUT on Darwin)
  if (le & L_POLLWRBAND) h |= POLLWRBAND;   // 0x200 -> 0x100
  return h;
}
static short pollev_h2l(short hr, short lwant) {
  short l = 0;
  if (hr & POLLIN)     l |= L_POLLIN;
  if (hr & POLLPRI)    l |= L_POLLPRI;
  if (hr & POLLOUT)    l |= L_POLLOUT;
  if (hr & POLLERR)    l |= L_POLLERR;
  if (hr & POLLHUP)    l |= L_POLLHUP;
  if (hr & POLLNVAL)   l |= L_POLLNVAL;
  if (hr & POLLRDNORM) l |= L_POLLRDNORM;
  if (hr & POLLRDBAND) l |= L_POLLRDBAND;
  if ((hr & POLLOUT) && (lwant & L_POLLWRNORM)) l |= L_POLLWRNORM;
  if ((hr & POLLWRBAND) && (lwant & L_POLLWRBAND)) l |= L_POLLWRBAND;
  return l;
}

static int poll_has_shim_eventfd(const struct pollfd* fds, unsigned long count) {
  for (unsigned long i = 0; i < count; i++) {
    struct sockaddr_un peer = {0};
    socklen_t len = sizeof peer;
    if (fds[i].fd >= 0 && getpeername(fds[i].fd, (struct sockaddr*)&peer, &len) == 0 &&
        peer.sun_family == AF_UNIX && strstr(peer.sun_path, "/.rdx-eventfd/")) return 1;
  }
  return 0;
}

static long sys_ppoll(struct pollfd* ufds, unsigned long nfds, const struct lin_timespec* uts) {
  if (nfds > 4096) return -L_EINVAL;
  int timeout = -1;                                    // NULL timespec == block forever
  if (uts) {
    int64_t ms = uts->tv_sec * 1000 + (uts->tv_nsec + 999999) / 1000000;
    if (ms < 0) ms = 0;
    if (ms > INT_MAX) ms = INT_MAX;
    timeout = (int)ms;
  }
  if (nfds == 0) {
    double t0 = polllog_on() ? now_ms() : 0;
    long rr = lret(poll(NULL, 0, timeout));
    if (polllog_on()) fprintf(stderr, "[polllog] t=%.2f ppoll n=0 req=%dms act=%.2fms ret=%ld\n",
                              now_ms(), timeout, now_ms() - t0, rr);
    return rr;
  }

  struct pollfd* hf = (struct pollfd*)calloc(nfds, sizeof *hf);
  if (!hf) return -L_ENOMEM;
  for (unsigned long i = 0; i < nfds; i++) {
    hf[i].fd = ufds[i].fd;
    hf[i].events = pollev_l2h(ufds[i].events);
    hf[i].revents = 0;
  }
  // The synthetic ALSA PCM fd is not a real file host poll() can block on: it is a /dev/null
  // fd whose "writable" state is the ring-buffer fill level, advanced by the CoreAudio render
  // thread (host side). Merge it here: check the snd fds ourselves and host-poll only the rest,
  // slicing the wait so a newly-freed buffer wakes us within a few ms.
  int has_snd = 0;
  for (unsigned long i = 0; i < nfds; i++)
    if (snd_owns_fd(ufds[i].fd)) { has_snd = 1; hf[i].fd = -1; }
  if (has_snd) {
    const int slice_ms = 5;
    double deadline = timeout >= 0 ? now_ms() + timeout : 0;
    int total = 0;
    for (;;) {
      for (unsigned long i = 0; i < nfds; i++) ufds[i].revents = 0;
      for (unsigned long i = 0; i < nfds; i++) hf[i].revents = 0;
      int ready = 0;
      for (unsigned long i = 0; i < nfds; i++) {
        if (snd_owns_fd(ufds[i].fd)) {
          short rev = snd_poll_revents(ufds[i].fd, ufds[i].events);
          if (rev) { ufds[i].revents = rev; ready++; }
        }
      }
      int w = timeout < 0 ? slice_ms : (timeout > slice_ms ? slice_ms : timeout);
      int r = poll(hf, (nfds_t)nfds, w);
      if (r < 0 && errno != EINTR) { free(hf); return lerr(); }
      if (r > 0) {
        for (unsigned long i = 0; i < nfds; i++)
          if (hf[i].fd >= 0 && hf[i].revents) {
            ufds[i].revents = pollev_h2l(hf[i].revents, ufds[i].events);
            ready++;
          }
      }
      if (ready) { total = ready; break; }
      if (timeout == 0) break;
      if (timeout >= 0 && now_ms() >= deadline) break;
    }
    if (polllog_on())
      fprintf(stderr, "[polllog] t=%.2f ppoll(snd) n=%lu req=%dms ret=%d\n",
              now_ms(), nfds, timeout, total);
    free(hf);
    return total;
  }
  double t0 = polllog_on() ? now_ms() : 0;
  // Diagnostic opt-in for an observed Darwin poll wake loss on the shim's
  // self-connected datagram eventfd. Restrict it to small, read-only sets
  // containing that eventfd; all other ppoll calls keep their original path.
  int slice = 0;
  const char* slice_opt = getenv("IDLE_REPOLL_EVENTFD_MS");
  if (slice_opt && nfds <= 24) {
    char* end = NULL;
    long requested = strtol(slice_opt, &end, 10);
    if (end && *end == 0 && requested >= 10 && requested <= 1000) {
      int eligible = 1;
      for (unsigned long i = 0; i < nfds; i++)
        if (hf[i].fd < 0 || hf[i].events != POLLIN) eligible = 0;
      if (eligible && poll_has_shim_eventfd(hf, nfds)) slice = (int)requested;
    }
  }
  double deadline = slice && timeout >= 0 ? now_ms() + timeout : 0;
  unsigned internal_wakes = 0;
  int r;
  for (;;) {
    int wait_ms = timeout;
    if (slice && (timeout < 0 || timeout > slice)) {
      if (timeout < 0) wait_ms = slice;
      else {
        double remain = deadline - now_ms();
        // Avoid a zero-length host poll while the guest deadline is fractional.
        wait_ms = remain <= 0 ? 0 : (remain < slice ? (int)remain + 1 : slice);
      }
    }
    for (unsigned long i = 0; i < nfds; i++) hf[i].revents = 0;
    r = poll(hf, (nfds_t)nfds, wait_ms);
    if (r != 0 || !slice || timeout == 0) break;
    if (timeout >= 0 && now_ms() >= deadline) break;
    internal_wakes++;
  }
  if (slice && getenv("IDLE_REPOLL_TRACE") && internal_wakes)
    fprintf(stderr, "[idle-repoll] pid=%d n=%lu slice=%d internal=%u out=%d\n",
            getpid(), nfds, slice, internal_wakes, r);
  if (polllog_on()) {
    fprintf(stderr, "[polllog] t=%.2f ppoll n=%lu req=%dms", now_ms(), nfds, timeout);
    if (uts) fprintf(stderr, " (ts=%lld.%09lld)", (long long)uts->tv_sec, (long long)uts->tv_nsec);
    fprintf(stderr, " act=%.2fms ret=%d errno=%d\n", now_ms() - t0, r, r < 0 ? errno : 0);
  }
  if (r < 0) { long e = lerr(); free(hf); return e; }
  for (unsigned long i = 0; i < nfds; i++) ufds[i].revents = pollev_h2l(hf[i].revents, ufds[i].events);
  if (polllog_on() && getenv("RDPOLLDETAIL") && nfds <= 8) {
    for (unsigned long i = 0; i < nfds; i++) {
      fprintf(stderr, "[polldetail] fd=%d events=%#hx revents=%#hx\n",
              ufds[i].fd, ufds[i].events, ufds[i].revents);
      if (ufds[i].revents & POLLIN) {
        uint32_t header[2] = {0};
        ssize_t got = recv(ufds[i].fd, header, sizeof(header), MSG_PEEK | MSG_DONTWAIT);
        if (got == (ssize_t)sizeof(header))
          fprintf(stderr, "[polldetail] wayland-peek fd=%d object=%u opcode=%u size=%u\n",
                  ufds[i].fd, header[0], header[1] & 0xffffu, header[1] >> 16);
      }
    }
  }
  free(hf);
  return r;
}

static long sys_pselect6(int nfds, fd_set* r, fd_set* w, fd_set* e, const struct lin_timespec* uts) {
  // Linux fd_set is a 1024-bit array of longs; Darwin's is a 1024-bit array of int32. Same bytes.
  if (nfds < 0) return -L_EINVAL;
  if (nfds > FD_SETSIZE) nfds = FD_SETSIZE;
  struct timespec ts, *tsp = NULL;
  if (uts) { ts.tv_sec = (time_t)uts->tv_sec; ts.tv_nsec = (long)uts->tv_nsec; tsp = &ts; }
  double t0 = polllog_on() ? now_ms() : 0;
  long rr = lret(pselect(nfds, r, w, e, tsp, NULL));   // sigmask deliberately ignored
  if (polllog_on()) fprintf(stderr, "[polllog] t=%.2f pselect6 nfds=%d req=%.2fms act=%.2fms ret=%ld\n",
                            now_ms(), nfds,
                            tsp ? (double)ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6 : -1.0,
                            now_ms() - t0, rr);
  return rr;
}

// ---------------------------------------------------------------------------
// eventfd — a self-connected AF_UNIX datagram socket
// ---------------------------------------------------------------------------
// Must be ONE fd that is writable, readable and pollable, because the guest's read()/write() go
// straight to syscalls.c and never come back through here. An 8-byte write becomes one datagram;
// an 8-byte read takes it back. That is faithful enough for libc/toolkit wakeup use.
static long sys_eventfd2(unsigned int initval, int flags) {
  char dir[PATH_MAX];
  private_dir(dir, sizeof dir, "eventfd");
  if (mkdir_p(dir) < 0 && errno != EEXIST) return lerr();

  static unsigned seq;
  pthread_mutex_lock(&g_lock);
  unsigned s = ++seq;
  pthread_mutex_unlock(&g_lock);

  struct sockaddr_un sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  int n = snprintf(sa.sun_path, sizeof sa.sun_path, "%s/e%d-%u", dir, (int)getpid(), s);
  if (n < 0 || (size_t)n >= sizeof sa.sun_path) return -L_ENAMETOOLONG;
  sa.sun_len = (unsigned char)(offsetof(struct sockaddr_un, sun_path) + n + 1);

  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return lerr();
  kill_sigpipe(fd);
  unlink(sa.sun_path);
  if (bind(fd, (struct sockaddr*)&sa, (socklen_t)sa.sun_len) < 0) { long e = lerr(); close(fd); return e; }
  if (connect(fd, (struct sockaddr*)&sa, (socklen_t)sa.sun_len) < 0) {
    long e = lerr(); unlink(sa.sun_path); close(fd); return e;
  }
  unlink(sa.sun_path);                       // both ends are the same fd, so the name is done

  if (initval) { uint64_t v = initval; if (send(fd, &v, sizeof v, 0) < 0) { /* best effort */ } }
  apply_fd_flags(fd, (flags & L_EFD_NONBLOCK) != 0, (flags & L_EFD_CLOEXEC) != 0);
  return fd;
}

// ---------------------------------------------------------------------------
// pipe2
// ---------------------------------------------------------------------------
static long sys_pipe2(int* ufd, int flags) {
  if (!ufd) return -L_EFAULT;
  if (flags & ~(L_O_NONBLOCK | L_O_CLOEXEC | L_O_DIRECT)) return -L_EINVAL;
  int fds[2];
  if (pipe(fds) < 0) return lerr();
  apply_fd_flags(fds[0], (flags & L_O_NONBLOCK) != 0, (flags & L_O_CLOEXEC) != 0);
  apply_fd_flags(fds[1], (flags & L_O_NONBLOCK) != 0, (flags & L_O_CLOEXEC) != 0);
  ufd[0] = fds[0]; ufd[1] = fds[1];
  return 0;
}

// ---------------------------------------------------------------------------
// sockets
// ---------------------------------------------------------------------------
static long sys_socket(int ldomain, int ltype, int protocol) {
  // AF_NETLINK does not exist on macOS. libudev only needs a netlink socket to (a) open and
  // (b) bind so its hotplug monitor "exists"; the guest never receives actual hotplug events,
  // so a quiet datagram socket is faithful ("subscribed, nothing arrives") — and it is the
  // difference between KWin's GpuManager booting (udev_monitor_new_from_netlink returns non-NULL)
  // and a null deref. See the GpuManager crash note in the KDE integration docs.
  if (ldomain == L_AF_NETLINK) {
    int nonblock = (ltype & L_SOCK_NONBLOCK) != 0, cloexec = (ltype & L_SOCK_CLOEXEC) != 0;
    // A host AF_UNIX datagram socket: never connected, never written to, so poll()/epoll never
    // reports it readable and recv() returns EAGAIN — exactly a netlink socket with no events.
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return lerr();
    kill_sigpipe(fd);
    apply_fd_flags(fd, nonblock, cloexec);
    return fd;
  }
  int hdom = af_l2h(ldomain);
  if (hdom < 0) return -L_EAFNOSUPPORT;
  int nonblock = (ltype & L_SOCK_NONBLOCK) != 0, cloexec = (ltype & L_SOCK_CLOEXEC) != 0;
  int htype = ltype & ~(L_SOCK_NONBLOCK | L_SOCK_CLOEXEC);
  // See sys_socketpair() below for why SOCK_SEQPACKET becomes SOCK_DGRAM on AF_UNIX.
  int fd = socket(hdom, (hdom == AF_UNIX && htype == SOCK_SEQPACKET) ? SOCK_DGRAM : htype,
                  protocol);
  if (fd < 0) return lerr();
  kill_sigpipe(fd);
  apply_fd_flags(fd, nonblock, cloexec);
  return fd;
}

static long sys_socketpair(int ldomain, int ltype, int protocol, int* usv) {
  if (!usv) return -L_EFAULT;
  int hdom = af_l2h(ldomain);
  if (hdom < 0) return -L_EAFNOSUPPORT;
  int nonblock = (ltype & L_SOCK_NONBLOCK) != 0, cloexec = (ltype & L_SOCK_CLOEXEC) != 0;
  int htype = ltype & ~(L_SOCK_NONBLOCK | L_SOCK_CLOEXEC);
  int sv[2];
  // SOCK_SEQPACKET over AF_UNIX exists on Linux and DOES NOT EXIST on macOS: the number (5) is
  // the same, `socketpair()` just refuses it, and the guest sees EPROTONOSUPPORT.
  //
  // This is not a corner case. WebKitGTK's IPC::Connection opens its UI<->WebProcess channel with
  // socketpair(AF_UNIX, SOCK_SEQPACKET|SOCK_CLOEXEC, 0) and calls CRASH() when it fails, so the
  // whole browser died with rc=134 and NO message, before any window, on every run. Observed:
  //     [sc] nr=199(0x1,0x80005,0) -> -93        <- socketpair(AF_UNIX, SEQPACKET|CLOEXEC)
  //     [sc] nr=172(...) -> 89115 ; nr=131(0x15c1b,0x1,0x6)   <- getpid then tgkill(self,SIGABRT)
  //
  // Fall back to SOCK_DGRAM, which is the closest thing macOS has: on a socketpair both ends are
  // already connected, and AF_UNIX datagrams preserve message boundaries, which is the property
  // SEQPACKET is chosen for. Only used AFTER the real type is refused, so a host that does
  // support SEQPACKET keeps exact semantics and this path is dead.
  if (socketpair(hdom, htype, protocol, sv) < 0) {
    int e = errno;
    if (hdom == AF_UNIX && htype == SOCK_SEQPACKET &&
        (e == EPROTONOSUPPORT || e == EPROTOTYPE || e == ESOCKTNOSUPPORT || e == EOPNOTSUPP)) {
      // RDSEQPACKET=stream|dgram picks the substitute, so the choice can be A/B'd rather than
      // assumed: DGRAM keeps message boundaries, STREAM keeps byte-count semantics, and only one
      // of them is right for a given consumer.
      const char* sub = getenv("RDSEQPACKET");
      int htype2 = (sub && !strcmp(sub, "stream")) ? SOCK_STREAM : SOCK_DGRAM;
      if (socketpair(hdom, htype2, protocol, sv) < 0) return lerr();
      // A Darwin AF_UNIX datagram socket is created with net.local.dgram.maxdgram = 2048 bytes of
      // send space, which is far below what a Linux SEQPACKET socket gives (208 KB) and far below
      // WebKit's IPC messages: the guest got "Error sending IPC message: No buffer space
      // available" (ENOBUFS) and the web process died. That 2048 is only the DEFAULT buffer, not
      // a hard cap, so raise it -- largest first, stepping down, since the ceiling is
      // kern.ipc.maxsockbuf and we must not fail the socketpair over this.
      for (int i = 0; i < 2; i++) {
        static const int kWant[] = { 1 << 20, 1 << 18, 1 << 16, 1 << 14 };
        for (size_t k = 0; k < sizeof kWant / sizeof *kWant; k++) {
          int v = kWant[k];
          if (setsockopt(sv[i], SOL_SOCKET, SO_SNDBUF, &v, sizeof v) == 0) break;
        }
        for (size_t k = 0; k < sizeof kWant / sizeof *kWant; k++) {
          int v = kWant[k];
          if (setsockopt(sv[i], SOL_SOCKET, SO_RCVBUF, &v, sizeof v) == 0) break;
        }
      }
      if (getenv("RDCREDLOG")) {
        int sb = 0, rb = 0; socklen_t sl = sizeof sb;
        getsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sb, &sl); sl = sizeof rb;
        getsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &rb, &sl);
        fprintf(stderr, "[cred] seqpacket->%s pair fd=%d/%d sndbuf=%d rcvbuf=%d\n",
                htype2 == SOCK_STREAM ? "stream" : "dgram", sv[0], sv[1], sb, rb);
      }
    } else {
      errno = e;
      return lerr();
    }
  }
  for (int i = 0; i < 2; i++) { kill_sigpipe(sv[i]); apply_fd_flags(sv[i], nonblock, cloexec); }
  spair_add(sv[0], sv[1]);
  usv[0] = sv[0]; usv[1] = sv[1];
  return 0;
}

static long sys_bind(int fd, const void* uaddr, uint32_t len) {
  // netlink "bind" carries a sockaddr_nl (pid/groups), not a filesystem path. The socket is the
  // emulated datagram above, so accept any netlink bind and record nothing: no events will ever
  // arrive, which is correct for a shim with no kernel uevent source.
  if (uaddr && len >= 2) {
    uint16_t fam; memcpy(&fam, uaddr, 2);
    if (fam == L_AF_NETLINK) return 0;
  }
  struct sockaddr_storage hs; socklen_t hlen = 0;
  long r = sa_to_host(uaddr, len, 1, &hs, &hlen);
  if (getenv("RDNETSOCK") && uaddr && len >= 2) {
    uint16_t fam; memcpy(&fam, uaddr, 2);
    if (fam == L_AF_UNIX) {
      const struct lin_sockaddr_un* lu = (const struct lin_sockaddr_un*)uaddr;
      fprintf(stderr, "[netsock] bind fd=%d guest='%s' host='%s' r=%ld\n", fd,
              lu->sun_path[0] ? lu->sun_path : "(abstract)",
              ((struct sockaddr_un*)&hs)->sun_path, r);
    }
  }
  if (r < 0) return r;
  long out = lret(bind(fd, (struct sockaddr*)&hs, hlen));
  if (getenv("RDNETSOCK") && out < 0) fprintf(stderr, "[netsock] bind result=%ld errno=%d\n", out, errno);
  return out;
}
static long sys_connect(int fd, const void* uaddr, uint32_t len) {
  struct sockaddr_storage hs; socklen_t hlen = 0;
  long r = sa_to_host(uaddr, len, 0, &hs, &hlen);
  if (getenv("RDNETSOCK") && uaddr && len >= 2) {
    uint16_t fam; memcpy(&fam, uaddr, 2);
    if (fam == L_AF_UNIX) {
      const struct lin_sockaddr_un* lu = (const struct lin_sockaddr_un*)uaddr;
      fprintf(stderr, "[netsock] connect fd=%d guest='%s' host='%s' r=%ld\n", fd,
              lu->sun_path[0] ? lu->sun_path : "(abstract)",
              ((struct sockaddr_un*)&hs)->sun_path, r);
    }
  }
  if (r < 0) return r;
  long out = lret(connect(fd, (struct sockaddr*)&hs, hlen));
  if (getenv("RDNETSOCK") && out < 0) fprintf(stderr, "[netsock] connect result=%ld errno=%d\n", out, errno);
  return out;
}
static long sys_accept4(int fd, unsigned long uaddr, unsigned long ulen, int flags) {
  struct sockaddr_storage hs; socklen_t hlen = sizeof hs;
  memset(&hs, 0, sizeof hs);
  int nfd = accept(fd, (struct sockaddr*)&hs, &hlen);
  if (nfd < 0) return lerr();
  kill_sigpipe(nfd);
  apply_fd_flags(nfd, (flags & L_SOCK_NONBLOCK) != 0, (flags & L_SOCK_CLOEXEC) != 0);
  if (uaddr && ulen) {
    long r = store_addr((struct sockaddr*)&hs, hlen, uaddr, ulen);
    if (r < 0) { close(nfd); return r; }
  }
  return nfd;
}
static long sys_getsockname(int fd, unsigned long uaddr, unsigned long ulen, int peer) {
  struct sockaddr_storage hs; socklen_t hlen = sizeof hs;
  memset(&hs, 0, sizeof hs);
  int r = peer ? getpeername(fd, (struct sockaddr*)&hs, &hlen)
               : getsockname(fd, (struct sockaddr*)&hs, &hlen);
  if (r < 0) return lerr();
  return store_addr((struct sockaddr*)&hs, hlen, uaddr, ulen);
}

static long sys_sendto(int fd, const void* buf, size_t len, int lflags,
                       const void* uaddr, uint32_t alen) {
  int hflags = msgflags_l2h(lflags);
  if (uaddr && alen) {
    struct sockaddr_storage hs; socklen_t hlen = 0;
    long r = sa_to_host(uaddr, alen, 0, &hs, &hlen);
    if (r < 0) return r;
    return lret(sendto(fd, buf, len, hflags, (struct sockaddr*)&hs, hlen));
  }
  return lret(sendto(fd, buf, len, hflags, NULL, 0));
}
static long sys_recvfrom(int fd, void* buf, size_t len, int lflags,
                         unsigned long uaddr, unsigned long ulen) {
  int hflags = msgflags_l2h(lflags);
  struct sockaddr_storage hs; socklen_t hlen = sizeof hs;
  memset(&hs, 0, sizeof hs);
  ssize_t n = recvfrom(fd, buf, len, hflags, uaddr ? (struct sockaddr*)&hs : NULL,
                       uaddr ? &hlen : NULL);
  if (n < 0) return lerr();
  if (uaddr && ulen) {
    long r = store_addr((struct sockaddr*)&hs, hlen, uaddr, ulen);
    if (r < 0) return r;
  }
  return (long)n;
}

static long sys_sendmsg(int fd, const struct lin_msghdr* lm, int lflags) {
  if (!lm) return -L_EFAULT;
  struct msghdr hm; memset(&hm, 0, sizeof hm);
  struct sockaddr_storage hs; socklen_t hslen = 0;
  if (lm->msg_name && lm->msg_namelen) {
    long r = sa_to_host((const void*)lm->msg_name, lm->msg_namelen, 0, &hs, &hslen);
    if (r < 0) return r;
    hm.msg_name = &hs; hm.msg_namelen = hslen;
  }
  hm.msg_iov = (struct iovec*)lm->msg_iov;          // identical layout on both sides
  if (lm->msg_iovlen > INT_MAX) return -L_EINVAL;
  hm.msg_iovlen = (int)lm->msg_iovlen;              // size_t on Linux, int on Darwin

  uint8_t* hctl = NULL; size_t hctllen = 0;
  if (lm->msg_control && lm->msg_controllen) {
    long r = cmsg_l2h((const uint8_t*)lm->msg_control, (size_t)lm->msg_controllen, &hctl, &hctllen);
    if (r < 0) return r;
    hm.msg_control = hctl; hm.msg_controllen = (socklen_t)hctllen;
  }
  ssize_t n = sendmsg(fd, &hm, msgflags_l2h(lflags));
  long out = (n < 0) ? lerr() : (long)n;
  free(hctl);
  return out;
}

// sendmmsg -- send several messages with one call. Darwin has no such syscall, but the Linux
// contract explicitly allows a short send: it returns the NUMBER of messages sent and only reports
// an error if the FIRST one fails. A loop over sendmsg is therefore a complete implementation, not
// an approximation.
//
// This is not an optional nicety. glibc's getaddrinfo(AF_UNSPEC) sends the A and AAAA queries in a
// single sendmmsg, and send_dg() in resolv/res_send.c treats a failure as fatal -- it closes the
// socket and gives up. With sendmmsg returning -ENOSYS, a Debian rootfs resolves NOTHING through
// the ordinary dual-stack path: getent ahostsv4 and ahostsv6 each work, plain `getent ahosts`
// exits 2, and every real program (which asks for AF_UNSPEC) reports "Host not found". musl issues
// two separate sendto calls and never hits it.
struct lin_mmsghdr { struct lin_msghdr msg_hdr; uint32_t msg_len; uint32_t __pad; };
_Static_assert(sizeof(struct lin_mmsghdr) == 64, "linux mmsghdr is 64 bytes on LP64");

static long sys_sendmmsg(int fd, struct lin_mmsghdr* lv, unsigned vlen, int lflags) {
  if (!lv) return -L_EFAULT;
  if (vlen > 1024) vlen = 1024;                 // Linux caps at UIO_MAXIOV
  unsigned sent = 0;
  for (; sent < vlen; sent++) {
    long r = sys_sendmsg(fd, &lv[sent].msg_hdr, lflags);
    if (r < 0) {
      if (sent == 0) return r;                  // only the first failure is reported
      break;
    }
    lv[sent].msg_len = (uint32_t)r;             // the caller reads back the per-message length
  }
  return (long)sent;
}

static long sys_recvmsg(int fd, struct lin_msghdr* lm, int lflags) {
  if (!lm) return -L_EFAULT;
  struct msghdr hm; memset(&hm, 0, sizeof hm);
  struct sockaddr_storage hs; memset(&hs, 0, sizeof hs);
  if (lm->msg_name && lm->msg_namelen) { hm.msg_name = &hs; hm.msg_namelen = sizeof hs; }
  hm.msg_iov = (struct iovec*)lm->msg_iov;
  if (lm->msg_iovlen > INT_MAX) return -L_EINVAL;
  hm.msg_iovlen = (int)lm->msg_iovlen;

  uint8_t* hctl = NULL;
  size_t lcap = (size_t)lm->msg_controllen;
  if (lm->msg_control && lcap) {
    size_t hcap = lcap;                             // the Darwin layout is never larger
    hctl = (uint8_t*)calloc(1, hcap);
    if (!hctl) return -L_ENOMEM;
    hm.msg_control = hctl; hm.msg_controllen = (socklen_t)hcap;
  }
  ssize_t n = recvmsg(fd, &hm, msgflags_l2h(lflags));
  // RDCREDLOG=1: WebKit's readPIDFromPeer has THREE failure messages and only one of them means
  // what it says. Log the actual byte count on a credential socket so "short read" is measured
  // rather than assumed -- 0 means the peer hung up, anything else means it did not.
  if (getenv("RDCREDLOG") && passcred_get(fd)) {
    size_t want = 0;
    for (size_t i = 0; i < (size_t)hm.msg_iovlen; i++) want += hm.msg_iov[i].iov_len;
    fprintf(stderr, "[cred] recvmsg fd=%d wanted=%zu got=%zd hctl=%u\n",
            fd, want, n, (unsigned)hm.msg_controllen);
  }
  if (n < 0) { long e = lerr(); free(hctl); return e; }

  int lflags_out = msgflags_h2l(hm.msg_flags);
  if (hm.msg_name && lm->msg_name) {
    uint32_t needed = 0;
    sa_to_guest((struct sockaddr*)&hs, hm.msg_namelen, (void*)lm->msg_name, lm->msg_namelen, &needed);
    lm->msg_namelen = needed;
  }
  if (hctl) {
    int trunc = 0;
    int gave = 0;
    size_t used = cmsg_h2l(fd, &hm, (uint8_t*)lm->msg_control, lcap,
                           (lflags & L_MSG_CMSG_CLOEXEC) != 0, &trunc, &gave);
    // SO_PASSCRED was asked for on this fd, so Linux would have appended SCM_CREDENTIALS here.
    // Darwin never will, so build it from the socket's own knowledge of its peer.
    if (!gave && passcred_get(fd)) {
      struct lin_ucred uc;
      size_t need = LCMSG_ALIGN(LCMSG_HDRLEN + sizeof uc);
      if (used + need <= lcap && peer_ucred(fd, &uc)) {
        uint8_t* p = (uint8_t*)lm->msg_control + used;
        struct lin_cmsghdr lh;
        lh.cmsg_len = (uint64_t)(LCMSG_HDRLEN + sizeof uc);
        lh.cmsg_level = L_SOL_SOCKET;
        lh.cmsg_type = L_SCM_CREDENTIALS;
        memcpy(p, &lh, sizeof lh);
        memcpy(p + LCMSG_HDRLEN, &uc, sizeof uc);
        used += need;
      }
    }
    lm->msg_controllen = used;
    if (trunc) lflags_out |= L_MSG_CTRUNC;
    free(hctl);
  } else {
    lm->msg_controllen = 0;
  }
  lm->msg_flags = lflags_out;
  return (long)n;
}

// recvmmsg -- receive several messages with one call. Same shape as sendmmsg above and the same
// reason to exist: it is the obvious next -ENOSYS to bite a glibc program, and an -ENOSYS here is
// not a graceful degradation, it is a hard failure for any caller that does not have a fallback.
//
// The loop mirrors Linux's own do_recvmmsg() rather than inventing semantics:
//   * only the FIRST failure is reported; a later one just ends the batch, and the count already
//     received is returned. That is why a short receive is contractual, not an approximation.
//   * MSG_WAITFORONE is stripped before each recvmsg and, once one message is in, converted into
//     MSG_DONTWAIT for the rest -- which is exactly what makes the call return promptly instead
//     of blocking for all vlen messages.
//   * the timeout is a DEADLINE checked between messages, not a per-message timeout. Linux checks
//     it after each datagram and stops; CLOCK_MONOTONIC so a clock step cannot extend it.
// A caller with vlen messages and no timeout and no MSG_WAITFORONE genuinely does block until it
// has them all. That is Linux's behaviour, so it is the right behaviour here.
static long sys_recvmmsg(int fd, struct lin_mmsghdr* lv, unsigned vlen, int lflags,
                         const struct lin_timespec* lts) {
  if (!lv) return -L_EFAULT;
  if (vlen > 1024) vlen = 1024;                   // Linux caps at UIO_MAXIOV
  int waitforone = (lflags & L_MSG_WAITFORONE) != 0;
  int f = lflags & ~L_MSG_WAITFORONE;

  struct timespec deadline;
  int have_deadline = 0;
  if (lts) {
    if (lts->tv_nsec < 0 || lts->tv_nsec >= 1000000000L || lts->tv_sec < 0) return -L_EINVAL;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec  += (time_t)lts->tv_sec;
    deadline.tv_nsec += (long)lts->tv_nsec;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    have_deadline = 1;
  }

  unsigned got = 0;
  for (; got < vlen; got++) {
    long r = sys_recvmsg(fd, &lv[got].msg_hdr, f);
    if (r < 0) {
      if (got == 0) return r;                     // only the first failure is an error
      break;
    }
    lv[got].msg_len = (uint32_t)r;                // the caller reads the length back out
    if (waitforone) f |= L_MSG_DONTWAIT;
    if (have_deadline) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec > deadline.tv_sec ||
          (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
        got++;
        break;
      }
    }
  }
  return (long)got;
}

// ---------------------------------------------------------------------------
// entry points
// ---------------------------------------------------------------------------
void net_init(void) {
  // A guest write to a closed socket must yield EPIPE, not kill the host process.
  signal(SIGPIPE, SIG_IGN);
  const char* group = getenv("RDSOCKETGROUP");
  if (group && group[0] == '/') {
    snprintf(g_group_prefix, sizeof g_group_prefix, "%s", group);
    g_group_prefix_len = strlen(g_group_prefix);
    while (g_group_prefix_len > 1 && g_group_prefix[g_group_prefix_len - 1] == '/')
      g_group_prefix[--g_group_prefix_len] = 0;
    mkdir_p(g_group_prefix);
  }
}

int net_owns_syscall(unsigned long nr) {
  switch (nr) {
    case LSYS_eventfd2:
    case LSYS_epoll_create1:
    case LSYS_epoll_ctl:
    case LSYS_epoll_pwait:
    case LSYS_pipe2:
    case LSYS_pselect6:
    case LSYS_ppoll:
    case LSYS_socket:
    case LSYS_socketpair:
    case LSYS_bind:
    case LSYS_listen:
    case LSYS_accept:
    case LSYS_connect:
    case LSYS_getsockname:
    case LSYS_getpeername:
    case LSYS_sendto:
    case LSYS_recvfrom:
    case LSYS_setsockopt:
    case LSYS_getsockopt:
    case LSYS_shutdown:
    case LSYS_sendmsg:
    case LSYS_recvmsg:
    case LSYS_sendmmsg:
    case LSYS_recvmmsg:
    case LSYS_accept4:
      return 1;
    default:
      return 0;
  }
}

long net_syscall(unsigned long nr, unsigned long a0, unsigned long a1, unsigned long a2,
                 unsigned long a3, unsigned long a4, unsigned long a5) {
  (void)a5;
  switch (nr) {
    case LSYS_eventfd2:
      return sys_eventfd2((unsigned int)a0, (int)(long)a1);

    case LSYS_epoll_create1:
      return sys_epoll_create1((int)(long)a0);
    case LSYS_epoll_ctl:
      return sys_epoll_ctl((int)(long)a0, (int)(long)a1, (int)(long)a2,
                           (struct lin_epoll_event*)a3);
    case LSYS_epoll_pwait:
      return sys_epoll_pwait((int)(long)a0, (struct lin_epoll_event*)a1,
                             (int)(long)a2, (int)(long)a3);   // a4/a5: sigmask — ignored

    case LSYS_pipe2:
      return sys_pipe2((int*)a0, (int)(long)a1);

    case LSYS_pselect6:
      return sys_pselect6((int)(long)a0, (fd_set*)a1, (fd_set*)a2, (fd_set*)a3,
                          (const struct lin_timespec*)a4);    // a5: sigmask — ignored
    case LSYS_ppoll:
      return sys_ppoll((struct pollfd*)a0, a1, (const struct lin_timespec*)a2);

    case LSYS_socket:
      return sys_socket((int)(long)a0, (int)(long)a1, (int)(long)a2);
    case LSYS_socketpair:
      return sys_socketpair((int)(long)a0, (int)(long)a1, (int)(long)a2, (int*)a3);
    case LSYS_bind:
      return sys_bind((int)(long)a0, (const void*)a1, (uint32_t)a2);
    case LSYS_listen:
      return lret(listen((int)(long)a0, (int)(long)a1));
    case LSYS_accept:
      return sys_accept4((int)(long)a0, a1, a2, 0);
    case LSYS_accept4:
      return sys_accept4((int)(long)a0, a1, a2, (int)(long)a3);
    case LSYS_connect:
      return sys_connect((int)(long)a0, (const void*)a1, (uint32_t)a2);
    case LSYS_getsockname:
      return sys_getsockname((int)(long)a0, a1, a2, 0);
    case LSYS_getpeername:
      return sys_getsockname((int)(long)a0, a1, a2, 1);
    case LSYS_sendto:
      return sys_sendto((int)(long)a0, (const void*)a1, (size_t)a2, (int)(long)a3,
                        (const void*)a4, (uint32_t)a5);
    case LSYS_recvfrom:
      return sys_recvfrom((int)(long)a0, (void*)a1, (size_t)a2, (int)(long)a3, a4, a5);
    case LSYS_setsockopt:
      return do_setsockopt((int)(long)a0, (int)(long)a1, (int)(long)a2,
                           (const void*)a3, (uint32_t)a4);
    case LSYS_getsockopt:
      return do_getsockopt((int)(long)a0, (int)(long)a1, (int)(long)a2,
                           (void*)a3, (uint32_t*)a4);
    case LSYS_shutdown:
      return lret(shutdown((int)(long)a0, (int)(long)a1));    // SHUT_RD/WR/RDWR are 0/1/2 on both
    case LSYS_sendmsg:
      return sys_sendmsg((int)(long)a0, (const struct lin_msghdr*)a1, (int)(long)a2);
    case LSYS_recvmsg:
      return sys_recvmsg((int)(long)a0, (struct lin_msghdr*)a1, (int)(long)a2);
    case LSYS_recvmmsg:
      return sys_recvmmsg((int)(long)a0, (struct lin_mmsghdr*)a1, (unsigned)a2,
                          (int)(long)a3, (const struct lin_timespec*)a4);
    case LSYS_sendmmsg:
      return sys_sendmmsg((int)(long)a0, (struct lin_mmsghdr*)a1, (unsigned)a2, (int)(long)a3);

    default:
      return -L_ENOSYS;
  }
}
