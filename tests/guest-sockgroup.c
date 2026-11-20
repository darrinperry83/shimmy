/* ARM64 raw ABI: socket-group privacy regression.
 *
 * The shim relocates guest AF_UNIX sockets into RDSOCKETGROUP. mkdir_p() must
 * create the sub-directories a bind() needs WITHOUT widening the group root
 * (mode 0700) to 0755: pidfd_shim's socket_group() requires owner-only mode,
 * and an exec'd process whose pidfd_shim_target_init() then fails exits the
 * shim with status 1 before the guest program ever runs. That is exactly why
 * every systemd service (systemd-executor) reported "Main process exited,
 * status=1" during a native-session boot.
 *
 * Reproduces the failure: bind() a socket under /run/sockgroup (which forces
 * mkdir_p() on <group>/run/sockgroup), then clone(CLONE_VM|CLONE_VFORK) +
 * execve() a second program. With RDNATIVE_SESSION=1 the exec'd shim runs
 * pidfd_shim_target_init(); if the group root was widened to 0755 that init
 * fails and the child shim exits before emitting SOCKGROUP_OK.
 *
 * Emits "SOCKGROUP_OK" on success.
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

#define SYS_write       64
#define SYS_exit_group  94
#define SYS_socket      198
#define SYS_bind        200
#define SYS_clone       220
#define SYS_execve      221
#define SYS_wait4       260

#define AF_UNIX        1
#define SOCK_STREAM    1
#define CLONE_VM       0x100
#define CLONE_VFORK    0x4000
#define SIGCHLD        17

struct sockaddr_un_linux { unsigned short sun_family; char sun_path[108]; };

static long sl(const char* s) { long n = 0; while (s[n]) n++; return n; }

static long str_eq(const char* a, const char* b) {
  while (*a && *b && *a == *b) { a++; b++; }
  return *a == *b;
}

static __attribute__((noreturn)) void die(long code) {
  sc6(SYS_exit_group, code, 0, 0, 0, 0, 0);
  for (;;) {}
}

static void emit_ok(void) {
  static const char ok[] = "SOCKGROUP_OK\n";
  sc6(SYS_write, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
}

static char* copy_str(char* dst, const char* src) {
  long i;
  for (i = 0; src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
  return dst;
}

static __attribute__((noreturn)) void do_spawn_child(void) {
  char bin[16], arg[8];
  char* argv[3];
  argv[0] = copy_str(bin, "/bin/probe");
  argv[1] = copy_str(arg, "child");
  argv[2] = 0;
  sc6(SYS_execve, (long)"/bin/probe", (long)argv, 0, 0, 0, 0);
  die(3);
}

static __attribute__((noreturn)) void do_parent(void) {
  static const char path_lit[] = "/run/sockgroup/test.sock";

  long sfd = sc6(SYS_socket, AF_UNIX, SOCK_STREAM, 0, 0, 0, 0);
  if (sfd != 3) die(2);

  struct sockaddr_un_linux addr;
  long i;
  addr.sun_family = AF_UNIX;
  for (i = 0; i < 108; i++) addr.sun_path[i] = 0;
  for (i = 0; i <= sl(path_lit); i++) addr.sun_path[i] = path_lit[i];
  long alen = 2 + sl(path_lit) + 1;
  /* bind() forces the shim to mkdir_p() <group>/run/sockgroup, which must NOT
   * widen <group> itself. A failure here (before the fix it silently succeeded
   * but chmod'd the group) is not what we detect — the child's exec is. */
  if (sc6(SYS_bind, sfd, (long)&addr, alen, 0, 0, 0) != 0) die(4);

  long child = sc6(SYS_clone, CLONE_VM | CLONE_VFORK | SIGCHLD, 0, 0, 0, 0, 0);
  if (child == 0) do_spawn_child();
  if (child < 0) die(5);

  long status = 0;
  long r = sc6(SYS_wait4, child, (long)&status, 0, 0, 0, 0);
  if (r != child) die(6);
  if ((status & 0x7f) == 0) {
    long code = (status >> 8) & 0xff;
    if (code != 0) die(code);
  }
  die(0);
}

__attribute__((naked)) void _start(void) {
  __asm__ volatile("mov x0, sp\n\tb start_c");
}

__attribute__((noreturn)) void start_c(unsigned long* stack) {
  long argc = (long)stack[0];
  char** argv = (char**)(stack + 1);
  if (argc >= 2 && argv[1] && str_eq(argv[1], "child")) {
    emit_ok();
    die(0);
  }
  do_parent();
}
