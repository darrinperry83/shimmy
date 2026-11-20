/* ARM64 raw ABI: multi-fd systemd socket activation. A service that receives
 * TWO listening sockets (e.g. systemd-journald: journald.socket + dev-log.socket)
 * must inherit BOTH as fds 3 and 4, with LISTEN_FDS=2 and LISTEN_FDNAMES carrying
 * both names, while a third socket marked SOCK_CLOEXEC must NOT leak.
 *
 * Reproduces systemd's exact shape: socket()+bind()+listen() two AF_UNIX sockets,
 * then clone(CLONE_VM|CLONE_VFORK) + execve() a second program with
 * LISTEN_FDS=2 / LISTEN_PID=<vfork child pid> / LISTEN_FDNAMES=<two names>, and
 * asserts in the child that fds 3 and 4 are both valid listening sockets and that
 * LISTEN_PID still matches getpid() after the shim's vfork->exec re-spawn.
 *
 * Emits "SOCKACT2_OK" on success.
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

#define SYS_fcntl       25
#define SYS_write       64
#define SYS_fstat       80
#define SYS_exit_group  94
#define SYS_getpid      172
#define SYS_socket      198
#define SYS_bind        200
#define SYS_listen      201
#define SYS_getsockname 204
#define SYS_clone       220
#define SYS_execve      221
#define SYS_wait4       260

#define AF_UNIX        1
#define SOCK_STREAM    1
#define SOCK_CLOEXEC   0x80000
#define F_GETFD        1
#define S_IFMT         0170000
#define S_IFSOCK       0140000
#define CLONE_VM       0x100
#define CLONE_VFORK    0x4000
#define SIGCHLD        17

struct sockaddr_un_linux { unsigned short sun_family; char sun_path[108]; };

struct linux_stat {
  unsigned long long st_dev, st_ino;
  unsigned int st_mode, st_nlink, st_uid, st_gid;
  unsigned long long st_rdev, __pad1;
  long long st_size;
  int st_blksize, __pad2;
  long long st_blocks;
  long long st_atime_sec, st_atime_nsec, st_mtime_sec, st_mtime_nsec,
            st_ctime_sec, st_ctime_nsec;
  unsigned int __unused4[2];
};

static long sl(const char* s) { long n = 0; while (s[n]) n++; return n; }

static long str_eq(const char* a, const char* b) {
  while (*a && *b && *a == *b) { a++; b++; }
  return *a == *b;
}

static long str_has(const char* hay, const char* needle) {
  /* does `needle` appear as a NUL/colon-delimited token in `hay`? */
  long i, j;
  for (i = 0; hay[i]; i++) {
    if (i != 0 && hay[i - 1] != ':') continue;
    for (j = 0; needle[j]; j++) if (hay[i + j] != needle[j]) break;
    if (needle[j] == 0 && (hay[i + j] == 0 || hay[i + j] == ':')) return 1;
  }
  return 0;
}

static __attribute__((noreturn)) void die(long code) {
  sc6(SYS_exit_group, code, 0, 0, 0, 0, 0);
  for (;;) {}
}

static void emit_ok(void) {
  static const char ok[] = "SOCKACT2_OK\n";
  sc6(SYS_write, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
}

/* Fill out with "LISTEN_PID=<decimal pid>", NUL-terminated. */
static void build_pid_env(char* out, long pid) {
  static const char pre[] = "LISTEN_PID=";
  char tmp[24];
  long i, nd = 0;
  for (i = 0; pre[i]; i++) out[i] = pre[i];
  if (pid == 0) { tmp[nd++] = '0'; }
  else { while (pid > 0) { tmp[nd++] = (char)('0' + (pid % 10)); pid /= 10; } }
  while (nd > 0) out[i++] = tmp[--nd];
  out[i] = 0;
}

static char* copy_str(char* dst, const char* src) {
  long i;
  for (i = 0; src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
  return dst;
}

static long env_index(char** envp, const char* key) {
  long i, j, k = sl(key);
  for (i = 0; envp[i]; i++) {
    for (j = 0; j < k; j++) if (envp[i][j] != key[j]) break;
    if (j == k) return i;
  }
  return -1;
}

static long parse_pid(const char* e) {
  const char* p = e + 11;
  long v = 0;
  if (*p < '0' || *p > '9') return -1;
  while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
  return *p == 0 ? v : -1;
}

/* The vfork child: set LISTEN_FDS/LISTEN_PID/LISTEN_FDNAMES and exec probe. */
static __attribute__((noreturn)) void do_spawn_child(void) {
  long pid = sc6(SYS_getpid, 0, 0, 0, 0, 0, 0);
  char env0[] = "LISTEN_FDS=2";
  char env1[32];
  char env2[] = "LISTEN_FDNAMES=alpha:beta";
  build_pid_env(env1, pid);
  char* envp[4];
  envp[0] = env0;
  envp[1] = env1;
  envp[2] = env2;
  envp[3] = 0;

  char bin[16], arg[8];
  char* argv[3];
  argv[0] = copy_str(bin, "/bin/probe");
  argv[1] = copy_str(arg, "child");
  argv[2] = 0;

  sc6(SYS_execve, (long)"/bin/probe", (long)argv, (long)envp, 0, 0, 0);
  die(8);
}

static long make_sock(const char* path, long expect_fd) {
  long sfd = sc6(SYS_socket, AF_UNIX, SOCK_STREAM, 0, 0, 0, 0);
  if (sfd != expect_fd) die(2 + expect_fd);
  struct sockaddr_un_linux addr;
  long i;
  addr.sun_family = AF_UNIX;
  for (i = 0; i < 108; i++) addr.sun_path[i] = 0;
  for (i = 0; i <= sl(path); i++) addr.sun_path[i] = path[i];
  long alen = 2 + sl(path) + 1;
  if (sc6(SYS_bind, sfd, (long)&addr, alen, 0, 0, 0) != 0) die(20);
  if (sc6(SYS_listen, sfd, 16, 0, 0, 0, 0) != 0) die(21);
  return sfd;
}

static void assert_socket(long fd, long diecode) {
  if (sc6(SYS_fcntl, fd, F_GETFD, 0, 0, 0, 0) < 0) die(diecode);
  struct linux_stat st;
  if (sc6(SYS_fstat, fd, (long)&st, 0, 0, 0, 0) != 0) die(diecode + 1);
  if ((st.st_mode & S_IFMT) != S_IFSOCK) die(diecode + 2);
  struct sockaddr_un_linux sa;
  unsigned long salen = sizeof sa;
  if (sc6(SYS_getsockname, fd, (long)&sa, (long)&salen, 0, 0, 0) != 0) die(diecode + 3);
  if (sa.sun_family != AF_UNIX) die(diecode + 4);
}

static __attribute__((noreturn)) void do_parent(void) {
  (void)make_sock("/tmp/sockact2a.sock", 3);
  (void)make_sock("/tmp/sockact2b.sock", 4);

  /* A third socket marked CLOEXEC: it must NOT be inherited. */
  long cfd = sc6(SYS_socket, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, 0, 0, 0);
  if (cfd != 5) die(5);
  if (sc6(SYS_fcntl, cfd, F_GETFD, 0, 0, 0, 0) != 1) die(6);

  long child = sc6(SYS_clone, CLONE_VM | CLONE_VFORK | SIGCHLD, 0, 0, 0, 0, 0);
  if (child == 0) do_spawn_child();
  if (child < 0) die(7);

  long status = 0;
  long r = sc6(SYS_wait4, child, (long)&status, 0, 0, 0, 0);
  if (r != child) die(9);
  if ((status & 0x7f) == 0) {
    long code = (status >> 8) & 0xff;
    if (code != 0) die(code);
  }
  die(0);
}

/* The grandchild checks the same invariants AFTER A SECOND exec, which is what
 * systemd's socket activation does under the shim: systemd execs the executor,
 * and the executor execs the service. The fds must survive BOTH re-execs. */
static void check_listen_env(char** envp, long diebase) {
  long fi = env_index(envp, "LISTEN_FDS=");
  if (fi < 0 || !str_eq(envp[fi] + 11, "2")) die(diebase);
  long pi = env_index(envp, "LISTEN_PID=");
  if (pi < 0) die(diebase + 1);
  if (parse_pid(envp[pi]) != sc6(SYS_getpid, 0, 0, 0, 0, 0, 0)) die(diebase + 2);
  long ni = env_index(envp, "LISTEN_FDNAMES=");
  if (ni < 0) die(diebase + 3);
  if (!str_has(envp[ni] + 15, "alpha")) die(diebase + 4);
  if (!str_has(envp[ni] + 15, "beta")) die(diebase + 5);
}

static __attribute__((noreturn)) void do_child(unsigned long* stack) {
  long argc = (long)stack[0];
  char** envp = (char**)(stack + 1 + argc + 1);

  assert_socket(3, 30);
  assert_socket(4, 40);
  if (sc6(SYS_fcntl, 5, F_GETFD, 0, 0, 0, 0) >= 0) die(15);
  check_listen_env(envp, 16);

  /* Second exec: the child becomes the "executor" and execs the "service". */
  char bin[16], arg[16];
  char* argv[3];
  argv[0] = copy_str(bin, "/bin/probe");
  argv[1] = copy_str(arg, "grandchild");
  argv[2] = 0;
  sc6(SYS_execve, (long)"/bin/probe", (long)argv, (long)envp, 0, 0, 0);
  die(60);
}

static __attribute__((noreturn)) void do_grandchild(unsigned long* stack) {
  long argc = (long)stack[0];
  char** envp = (char**)(stack + 1 + argc + 1);

  assert_socket(3, 70);
  assert_socket(4, 80);
  if (sc6(SYS_fcntl, 5, F_GETFD, 0, 0, 0, 0) >= 0) die(90);
  check_listen_env(envp, 100);

  emit_ok();
  die(0);
}

__attribute__((naked)) void _start(void) {
  __asm__ volatile("mov x0, sp\n\tb start_c");
}

__attribute__((noreturn)) void start_c(unsigned long* stack) {
  long argc = (long)stack[0];
  char** argv = (char**)(stack + 1);
  if (argc >= 2 && argv[1] && str_eq(argv[1], "child")) {
    do_child(stack);
  }
  if (argc >= 2 && argv[1] && str_eq(argv[1], "grandchild")) {
    do_grandchild(stack);
  }
  do_parent();
}
