/* ARM64 raw ABI: install a minimal seccomp filter and prove it is enforced.
 *
 * The filter denies getppid (aarch64 nr 173) with ERRNO(EPERM) and ALLOWs
 * everything else, so getpid (nr 172) must keep working. The program is the
 * two-branch classic BPF shape from the Linux seccomp man page. */
struct sock_filter { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog  { unsigned short len; unsigned short pad; unsigned int pad2; unsigned long filter; };

static long sc6(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e, x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}

static __attribute__((noreturn)) void done(int code) {
  static const char ok[] = "SECCOMP_GUEST_OK\n";
  if (!code) sc6(64, 1, (long)ok, sizeof ok - 1, 0, 0, 0);
  sc6(93, code, 0, 0, 0, 0, 0);
  for (;;) {}
}

void _start(void) {
  /* PR_SET_NO_NEW_PRIVS = 38: mandatory before a filter is accepted. */
  if (sc6(167, 38, 1, 0, 0, 0, 0) != 0) done(1);

  /* Deny nr == 173 (getppid) with EPERM; ALLOW everything else. */
  struct sock_filter insns[4] = {
    { 0x20, 0, 0, 0 },                /* BPF_LD|BPF_W|BPF_ABS, k=0 -> seccomp_data.nr */
    { 0x15, 0, 1, 173 },              /* BPF_JMP|BPF_JEQ|BPF_K: nr==173 -> jt=0 (next), else jf=1 (skip) */
    { 0x06, 0, 0, 0x00050000u | 1u }, /* BPF_RET|BPF_K: SECCOMP_RET_ERRNO | EPERM */
    { 0x06, 0, 0, 0x7fff0000u },      /* BPF_RET|BPF_K: SECCOMP_RET_ALLOW */
  };
  struct sock_fprog fp = { 4, 0, 0, (unsigned long)insns };

  /* seccomp(277, SECCOMP_SET_MODE_FILTER=1, flags=0, &fp). */
  if (sc6(277, 1, 0, (long)&fp, 0, 0, 0) != 0) done(2);

  /* getppid (173) must be denied with -EPERM. */
  if (sc6(173, 0, 0, 0, 0, 0, 0) != -1) done(3);

  /* getpid (172) must still succeed. */
  if (sc6(172, 0, 0, 0, 0, 0, 0) <= 0) done(4);

  done(0);
}
