/* ARM64 raw ABI: a guest TCP listen socket must not hold a host TCP port.
 * Bind 127.0.0.1:PORT, listen, report success, then exit. The Python driver
 * holds the same host port and runs this guest twice; both binds must succeed
 * because the shim relocates the guest listen port and releases it on exit.
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

#define SYS_close   57
#define SYS_write   64
#define SYS_exit    93
#define SYS_socket  198
#define SYS_bind    200
#define SYS_listen  201

#define AF_INET     2
#define SOCK_STREAM 1

#define PORT 9000

static long sl(const char* s) { long n = 0; while (s[n]) n++; return n; }

static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "SOCKFIX_TCP_BOUND\n";
  if (!code) sc6(SYS_write, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(SYS_exit, code, 0, 0, 0, 0, 0);
  for (;;) {}
}

void _start(void) {
  unsigned char addr[16];
  long i;
  for (i = 0; i < 16; i++) addr[i] = 0;
  addr[0] = AF_INET & 0xff; addr[1] = (AF_INET >> 8) & 0xff;  /* sin_family */
  addr[2] = (PORT >> 8) & 0xff; addr[3] = PORT & 0xff;        /* sin_port (net order) */
  addr[4] = 127; addr[5] = 0; addr[6] = 0; addr[7] = 1;       /* 127.0.0.1 */

  long fd = sc6(SYS_socket, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
  if (fd < 0) done(1);
  if (sc6(SYS_bind, fd, (long)&addr, 16, 0, 0, 0) != 0) done(2);
  if (sc6(SYS_listen, fd, 4, 0, 0, 0, 0) != 0) done(3);
  sc6(SYS_close, fd, 0, 0, 0, 0, 0);
  done(0);
}
