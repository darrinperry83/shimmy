/* ARM64 raw ABI: sd_notify-style SCM_CREDENTIALS recovery on an UNCONNECTED
 * AF_UNIX datagram socket.
 *
 * systemd's /run/systemd/notify is a bound-but-never-connected SOCK_DGRAM socket
 * with SO_PASSCRED set. macOS cannot attach SCM_CREDENTIALS to a datagram, so the
 * shim must recover the sender's pid from the datagram's source pathname (the
 * sender autobinds to ".rdx-sender/<pid>.<seq>"). This test reproduces exactly that:
 *
 *   1. parent: socket(AF_UNIX, SOCK_DGRAM) -> bind("/tmp/scmcred.sock")
 *      -> setsockopt(SO_PASSCRED, 1)
 *   2. parent: fork()
 *   3. child:  socket(AF_UNIX, SOCK_DGRAM) -> sendmsg(to "/tmp/scmcred.sock",
 *              WITHOUT connect)  — forces the shim's autobind_sender() path
 *   4. parent: recvmsg() and assert an SCM_CREDENTIALS cmsg carrying the child's pid
 */
static long sc6(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                   : "memory");
  return x0;
}

#define SYS_close      57
#define SYS_write      64
#define SYS_exit       93
#define SYS_getpid     172
#define SYS_socket     198
#define SYS_bind       200
#define SYS_setsockopt 208
#define SYS_sendmsg    211
#define SYS_recvmsg    212
#define SYS_clone      220

#define AF_UNIX        1
#define SOCK_DGRAM     2
#define SOL_SOCKET     1
#define SO_PASSCRED    16
#define SCM_CREDENTIALS 2
#define SIGCHLD        17

struct sockaddr_un_linux {
  unsigned short sun_family;
  char sun_path[108];
};

struct iovec {
  void* iov_base;
  unsigned long iov_len;
};

/* Linux aarch64 struct msghdr (56 bytes; matches src/net.c lin_msghdr). */
struct msghdr {
  void* msg_name;
  unsigned int msg_namelen;
  unsigned int __pad1;
  void* msg_iov;
  unsigned long msg_iovlen;
  void* msg_control;
  unsigned long msg_controllen;
  int msg_flags;
  unsigned int __pad2;
};

/* Linux aarch64 struct cmsghdr (16 bytes) and struct ucred (12 bytes). */
struct cmsghdr {
  unsigned long cmsg_len;
  int cmsg_level;
  int cmsg_type;
};

struct ucred {
  int pid, uid, gid;
};

static long sl(const char* s) {
  long n = 0;
  while (s[n]) n++;
  return n;
}

static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "SDNOTIFY_SCM_OK\n";
  static const char fail[] = "SDNOTIFY_SCM_FAIL\n";
  const char* m = code ? fail : ok;
  unsigned long n = code ? (sizeof fail - 1) : (sizeof ok - 1);
  sc6(SYS_write, 1, (long)m, (long)n, 0, 0, 0);
  sc6(SYS_exit, code, 0, 0, 0, 0, 0);
  for (;;) {}
}

static long fork_child(void) {
  /* clone(SIGCHLD, NULL, ...) == fork(). No CLONE_VM, so the shim does a host
   * fork() and the child is a real separate guest process with its own pid. */
  register long x0 __asm__("x0") = SIGCHLD;
  register long x1 __asm__("x1") = 0;
  register long x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
  register long x4 __asm__("x4") = 0, x8 __asm__("x8") = SYS_clone;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
                   : "memory");
  return x0;  /* child: 0, parent: child pid */
}

/* The child must exit SILENTLY: the run harness greps the parent's stdout for the
 * marker, so the child printing it too would mask a missing/wrong credential. */
static __attribute__((noreturn)) void child_exit(int code) {
  sc6(SYS_exit, code, 0, 0, 0, 0, 0);
  for (;;) {}
}

static void child_send(void) {
  long sfd = sc6(SYS_socket, AF_UNIX, SOCK_DGRAM, 0, 0, 0, 0);
  if (sfd < 0) child_exit(2);

  struct sockaddr_un_linux dst;
  long i;
  const char* path = "/tmp/scmcred.sock";
  dst.sun_family = AF_UNIX;
  for (i = 0; i < 108; i++) dst.sun_path[i] = 0;
  for (i = 0; i <= sl(path); i++) dst.sun_path[i] = path[i];

  struct iovec iov;
  char byte = 'X';
  iov.iov_base = &byte;
  iov.iov_len = 1;

  struct msghdr mh;
  for (i = 0; i < (long)sizeof mh; i++) ((char*)&mh)[i] = 0;
  mh.msg_name = &dst;
  mh.msg_namelen = 2 + (unsigned)sl(path) + 1;
  mh.msg_iov = &iov;
  mh.msg_iovlen = 1;

  long n = sc6(SYS_sendmsg, sfd, (long)&mh, 0, 0, 0, 0);
  sc6(SYS_close, sfd, 0, 0, 0, 0, 0);
  child_exit(n < 0 ? 3 : 0);
}

void _start(void) {
  long i;
  const char* path = "/tmp/scmcred.sock";

  long rfd = sc6(SYS_socket, AF_UNIX, SOCK_DGRAM, 0, 0, 0, 0);
  if (rfd < 0) done(4);

  struct sockaddr_un_linux addr;
  addr.sun_family = AF_UNIX;
  for (i = 0; i < 108; i++) addr.sun_path[i] = 0;
  for (i = 0; i <= sl(path); i++) addr.sun_path[i] = path[i];
  long alen = 2 + sl(path) + 1;
  if (sc6(SYS_bind, rfd, (long)&addr, alen, 0, 0, 0) != 0) done(5);

  int one = 1;
  if (sc6(SYS_setsockopt, rfd, SOL_SOCKET, SO_PASSCRED, (long)&one, 4, 0) != 0) done(6);

  long child = fork_child();
  if (child < 0) done(7);
  if (child == 0) child_send();   /* never returns */

  char buf[16];
  for (i = 0; i < (long)sizeof buf; i++) buf[i] = 0;
  struct iovec riov;
  riov.iov_base = buf;
  riov.iov_len = sizeof buf;

  unsigned char cbuf[64];
  for (i = 0; i < (long)sizeof cbuf; i++) cbuf[i] = 0;

  struct msghdr rmh;
  for (i = 0; i < (long)sizeof rmh; i++) ((char*)&rmh)[i] = 0;
  rmh.msg_iov = &riov;
  rmh.msg_iovlen = 1;
  rmh.msg_control = cbuf;
  rmh.msg_controllen = sizeof cbuf;

  long n = sc6(SYS_recvmsg, rfd, (long)&rmh, 0, 0, 0, 0);
  if (n < 0) done(8);

  int found = 0;
  unsigned long left = rmh.msg_controllen;
  char* p = (char*)cbuf;
  while (left >= 16) {
    struct cmsghdr* c = (struct cmsghdr*)p;
    if (c->cmsg_len < 16 || c->cmsg_len > left) break;
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS) {
      struct ucred* uc = (struct ucred*)(p + 16);
      if (uc->pid == (int)child) found = 1;
    }
    unsigned long adv = (c->cmsg_len + 7) & ~7UL;
    p += adv;
    left -= adv;
  }

  sc6(SYS_close, rfd, 0, 0, 0, 0, 0);
  done(found ? 0 : 9);
}
