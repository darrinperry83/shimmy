/* ARM64 raw ABI: seccomp() must accept the filters systemd generates for a
 * hardened service and answer the kernel-support probe libseccomp issues before
 * loading one.
 *
 * Two regressions behind the D-Bus activation failures, both hit by
 * polkit.service/upower.service (SystemCallFilter=@system-service):
 *   * the compiled filter is 411 instructions; SHIM_SECCOMP_MAX_INSNS was 256,
 *     so seccomp(SECCOMP_SET_MODE_FILTER) failed with EINVAL and the service
 *     died at "step SECCOMP".
 *   * libseccomp probes seccomp(SECCOMP_GET_ACTION_AVAIL, SCMP_ACT_KILL_PROCESS)
 *     before loading; the shim answered EINVAL, so the filter never installed.
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
#define SYS_exit        93
#define SYS_prctl       167
#define SYS_seccomp     277

#define PR_SET_NO_NEW_PRIVS 38
#define SECCOMP_SET_MODE_FILTER 1
#define SECCOMP_GET_ACTION_AVAIL 2
#define SCMP_ACT_ALLOW        0x7fff0000u
#define SCMP_ACT_KILL_PROCESS 0x80000000u

struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog { unsigned short len; unsigned short _pad0; unsigned int _pad1; unsigned long filter; };

#define N_INSNS 300   /* > old 256 ceiling; @system-service is ~411 */

static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "SECCOMP_FILTER_OK\n";
  static const char fail[] = "SECCOMP_FILTER_FAIL\n";
  const char* m = code ? fail : ok;
  unsigned long n = code ? (sizeof fail - 1) : (sizeof ok - 1);
  sc6(SYS_write, 1, (long)m, n, 0, 0, 0);
  sc6(SYS_exit, code, 0, 0, 0, 0, 0);
  for (;;) {}
}

void _start(void) {
  /* A filter may only be installed once no_new_privs is set. */
  if (sc6(SYS_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0, 0) != 0) done(1);

  /* libseccomp probes KILL_PROCESS support before loading @system-service. */
  unsigned int action = SCMP_ACT_KILL_PROCESS;
  if (sc6(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, (long)&action, 0, 0, 0) != 0)
    done(2);

  /* A valid filter longer than the old 256-instruction ceiling. The verifier
   * requires the final instruction to be RET and non-jump jt/jf to be zero. */
  struct sock_filter insns[N_INSNS + 1];
  for (long i = 0; i < N_INSNS; i++) {
    insns[i].code = 0x00;  /* BPF_LD | BPF_IMM */
    insns[i].jt = 0;
    insns[i].jf = 0;
    insns[i].k = 0;
  }
  insns[N_INSNS].code = 0x06;  /* BPF_RET | BPF_K */
  insns[N_INSNS].jt = 0;
  insns[N_INSNS].jf = 0;
  insns[N_INSNS].k = SCMP_ACT_ALLOW;

  struct sock_fprog prog;
  prog.len = N_INSNS + 1;
  prog._pad0 = 0;
  prog._pad1 = 0;
  prog.filter = (unsigned long)insns;

  if (sc6(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, (long)&prog, 0, 0, 0) != 0)
    done(3);

  done(0);
}
