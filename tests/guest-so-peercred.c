/* ARM64 raw ABI: SO_PEERCRED on an AF_UNIX SOCK_STREAM socket must report the
 * peer's {pid,uid,gid}. The peer is a FORKED child (no exec), so under RDUID0 it
 * never publishes a guest credential record; SO_PEERCRED must still succeed with
 * uid=gid=0 rather than ENOPROTOOPT (which broke journald's client identification).
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

#define SYS_close       57
#define SYS_write       64
#define SYS_exit        93
#define SYS_socket      198
#define SYS_bind        200
#define SYS_listen      201
#define SYS_accept      202
#define SYS_connect     203
#define SYS_getsockopt  209
#define SYS_clone       220

#define AF_UNIX         1
#define SOCK_STREAM     1
#define SOL_SOCKET      1
#define SO_PEERCRED     17
#define SIGCHLD         17

struct sockaddr_un_linux { unsigned short sun_family; char sun_path[108]; };
struct ucred { int pid, uid, gid; };

static long sl(const char* s) { long n = 0; while (s[n]) n++; return n; }

static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "SO_PEERCRED_OK\n";
  static const char fail[] = "SO_PEERCRED_FAIL\n";
  const char* m = code ? fail : ok;
  unsigned long n = code ? (sizeof fail - 1) : (sizeof ok - 1);
  sc6(SYS_write, 1, (long)m, n, 0, 0, 0);
  sc6(SYS_exit, code, 0, 0, 0, 0, 0);
  for (;;) {}
}

static long fork_child(void) {
  register long x0 __asm__("x0") = SIGCHLD;
  register long x1 __asm__("x1") = 0;
  register long x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
  register long x4 __asm__("x4") = 0, x8 __asm__("x8") = SYS_clone;
  __asm__ volatile("svc #0" : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
                   : "memory");
  return x0;
}

static __attribute__((noreturn)) void child_exit(int code) {
  sc6(SYS_exit, code, 0, 0, 0, 0, 0);
  for (;;) {}
}

static void child_connect(void) {
  long cfd = sc6(SYS_socket, AF_UNIX, SOCK_STREAM, 0, 0, 0, 0);
  if (cfd < 0) child_exit(2);

  struct sockaddr_un_linux addr;
  long i;
  const char* path = "/tmp/so-peercred.sock";
  addr.sun_family = AF_UNIX;
  for (i = 0; i < 108; i++) addr.sun_path[i] = 0;
  for (i = 0; i <= sl(path); i++) addr.sun_path[i] = path[i];

  if (sc6(SYS_connect, cfd, (long)&addr, 2 + sl(path) + 1, 0, 0, 0) != 0)
    child_exit(3);
  /* keep the peer end open long enough for the parent to read our creds, then exit */
  for (volatile long z = 0; z < 400000000L; z++) {}
  sc6(SYS_exit, 0, 0, 0, 0, 0, 0);
  for (;;) {}
}

void _start(void) {
  long i;
  const char* path = "/tmp/so-peercred.sock";

  long lfd = sc6(SYS_socket, AF_UNIX, SOCK_STREAM, 0, 0, 0, 0);
  if (lfd < 0) done(4);

  struct sockaddr_un_linux addr;
  addr.sun_family = AF_UNIX;
  for (i = 0; i < 108; i++) addr.sun_path[i] = 0;
  for (i = 0; i <= sl(path); i++) addr.sun_path[i] = path[i];
  long alen = 2 + sl(path) + 1;
  if (sc6(SYS_bind, lfd, (long)&addr, alen, 0, 0, 0) != 0) done(5);
  if (sc6(SYS_listen, lfd, 1, 0, 0, 0, 0) != 0) done(6);

  long child = fork_child();
  if (child < 0) done(7);
  if (child == 0) child_connect();   /* never returns */

  long afd = sc6(SYS_accept, lfd, 0, 0, 0, 0, 0);
  if (afd < 0) done(8);

  struct ucred uc;
  for (i = 0; i < (long)sizeof uc; i++) ((char*)&uc)[i] = 0;
  long olen = sizeof uc;
  long r = sc6(SYS_getsockopt, afd, SOL_SOCKET, SO_PEERCRED, (long)&uc, (long)&olen, 0);
  if (r != 0) done(9);
  if (uc.pid != (int)child) done(10);
  if (uc.uid != 0 || uc.gid != 0) done(11);

  sc6(SYS_close, afd, 0, 0, 0, 0, 0);
  sc6(SYS_close, lfd, 0, 0, 0, 0, 0);
  done(0);
}
