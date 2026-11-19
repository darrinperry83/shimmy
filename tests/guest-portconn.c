/* ARM64 raw ABI: full TCP loopback through the shim's per-boot port offset.
 *
 * Server (parent) binds 127.0.0.1:PORT and listens; client (forked child) connects
 * to 127.0.0.1:PORT. Data must flow in both directions, and the server's
 * getsockname() plus the client's getpeername() must report the ORIGINAL PORT,
 * never the host's offset port. The Python driver holds the real host PORT for
 * the whole run: the guest bind must still succeed (host-port isolation) and must
 * never steal the host's port.
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

#define SYS_read        63
#define SYS_write       64
#define SYS_close       57
#define SYS_exit        93
#define SYS_socket      198
#define SYS_bind        200
#define SYS_listen      201
#define SYS_accept      202
#define SYS_connect     203
#define SYS_getsockname 204
#define SYS_getpeername 205
#define SYS_clone       220

#define AF_INET         2
#define SOCK_STREAM     1
#define SIGCHLD         17

/* Small enough that PORT + g_port_offset stays inside the shim's mapped band
 * 0x8000..0xBFFF for the test's (RDSHIM_BOOT-unset) deterministic offset. */
#define PORT 2222

static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "PORTCONN_OK\n";
  static const char fail[] = "PORTCONN_FAIL\n";
  const char* m = code ? fail : ok;
  unsigned long n = code ? (sizeof fail - 1) : (sizeof ok - 1);
  sc6(SYS_write, 1, (long)m, n, 0, 0, 0);
  sc6(SYS_exit, code, 0, 0, 0, 0, 0);
  for (;;) {}
}

static void make_addr(unsigned char* a) {
  long i;
  for (i = 0; i < 16; i++) a[i] = 0;
  a[0] = AF_INET & 0xff; a[1] = (AF_INET >> 8) & 0xff;   /* sin_family */
  a[2] = (PORT >> 8) & 0xff; a[3] = PORT & 0xff;         /* sin_port (net order) */
  a[4] = 127; a[5] = 0; a[6] = 0; a[7] = 1;              /* 127.0.0.1 */
}

/* Loop over short reads until n bytes arrive; returns 0 on success, -1 on EOF/error. */
static long read_full(long fd, char* buf, long n) {
  long got = 0;
  while (got < n) {
    long r = sc6(SYS_read, fd, (long)(buf + got), n - got, 0, 0, 0);
    if (r <= 0) return -1;
    got += r;
  }
  return 0;
}

/* The reported port from a 16-byte Linux sockaddr_in. */
static unsigned int port_of(const unsigned char* a) {
  return ((unsigned int)a[2] << 8) | (unsigned int)a[3];
}

/* getsockname (peer=0) / getpeername (peer=1); returns 0xffff on syscall error. */
static unsigned int reported_port(long fd, int peer) {
  unsigned char a[16];
  unsigned int alen = sizeof a;
  long r = sc6(peer ? SYS_getpeername : SYS_getsockname,
               fd, (long)a, (long)&alen, 0, 0, 0);
  if (r != 0) return 0xffff;
  return port_of(a);
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

static __attribute__((noreturn)) void client(void) {
  long cfd = sc6(SYS_socket, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
  if (cfd < 0) child_exit(2);

  unsigned char addr[16];
  make_addr(addr);
  if (sc6(SYS_connect, cfd, (long)addr, 16, 0, 0, 0) != 0) child_exit(3);

  unsigned int s1 = (reported_port(cfd, 1) == PORT) ? 1 : 0;  /* peer == server port */

  char msg[6] = {'h', 'e', 'l', 'l', 'o', (char)s1};
  if (sc6(SYS_write, cfd, (long)msg, 6, 0, 0, 0) != 6) child_exit(4);

  char rbuf[6];
  if (read_full(cfd, rbuf, 6) != 0) child_exit(5);
  if (rbuf[0] != 'w' || rbuf[1] != 'o' || rbuf[2] != 'r' || rbuf[3] != 'l' || rbuf[4] != 'd')
    child_exit(6);
  unsigned int s2 = (rbuf[5] != 0) ? 1 : 0;                    /* server's getsockname ok */

  /* Confirm to the server that the echo and the server's port check both passed. */
  char ack = (s1 && s2) ? 'A' : 'N';
  if (sc6(SYS_write, cfd, (long)&ack, 1, 0, 0, 0) != 1) child_exit(7);

  sc6(SYS_close, cfd, 0, 0, 0, 0, 0);
  child_exit(s1 && s2 ? 0 : 8);
}

void _start(void) {
  long lfd = sc6(SYS_socket, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
  if (lfd < 0) done(9);

  unsigned char addr[16];
  make_addr(addr);
  if (sc6(SYS_bind, lfd, (long)addr, 16, 0, 0, 0) != 0) done(10);
  if (sc6(SYS_listen, lfd, 4, 0, 0, 0, 0) != 0) done(11);

  long child = fork_child();
  if (child < 0) done(12);
  if (child == 0) client();            /* never returns */

  long afd = sc6(SYS_accept, lfd, 0, 0, 0, 0, 0);
  if (afd < 0) done(13);

  char rbuf[6];
  if (read_full(afd, rbuf, 6) != 0) done(14);
  if (rbuf[0] != 'h' || rbuf[1] != 'e' || rbuf[2] != 'l' || rbuf[3] != 'l' || rbuf[4] != 'o')
    done(15);
  unsigned int s1 = (rbuf[5] != 0) ? 1 : 0;                    /* client's getpeername ok */

  unsigned int s2 = (reported_port(afd, 0) == PORT) ? 1 : 0;   /* own bound port == PORT */

  char msg[6] = {'w', 'o', 'r', 'l', 'd', (char)s2};
  if (sc6(SYS_write, afd, (long)msg, 6, 0, 0, 0) != 6) done(16);

  char ack = 0;
  if (read_full(afd, &ack, 1) != 0) done(17);
  if (ack != 'A') done(18);

  sc6(SYS_close, afd, 0, 0, 0, 0, 0);
  sc6(SYS_close, lfd, 0, 0, 0, 0, 0);
  done(s1 && s2 ? 0 : 19);
}
