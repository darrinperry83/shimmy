#include "../src/seccomp_bpf.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#define I(code, jt, jf, k) {code, jt, jf, k}
#define ALLOW SHIM_SECCOMP_RET_ALLOW
#define ERRNO(e) (SHIM_SECCOMP_RET_ERRNO | (e))

int main(void) {
  const struct shim_sock_filter p[] = {
    I(0x20, 0, 0, offsetof(struct shim_seccomp_data, arch)),
    I(0x15, 0, 4, SHIM_SECCOMP_ARCH_AARCH64),
    I(0x20, 0, 0, offsetof(struct shim_seccomp_data, nr)),
    I(0x15, 0, 1, 64),
    I(0x06, 0, 0, ERRNO(13)),
    I(0x06, 0, 0, ALLOW),
    I(0x06, 0, 0, ERRNO(1)),
  };
  struct shim_seccomp_data data = {.nr = 64, .arch = SHIM_SECCOMP_ARCH_AARCH64};
  assert(shim_seccomp_bpf_verify(p, 7) == 0);
  assert(shim_seccomp_bpf_eval(p, 7, &data) == ERRNO(13));
  data.nr = 172;
  assert(shim_seccomp_bpf_eval(p, 7, &data) == ALLOW);
  data.arch = 0;
  assert(shim_seccomp_bpf_eval(p, 7, &data) == ERRNO(1));

  const struct shim_sock_filter arg[] = {
    I(0x20, 0, 0, offsetof(struct shim_seccomp_data, args)),
    I(0x54, 0, 0, 1),
    I(0x45, 1, 0, 1),
    I(0x06, 0, 0, ALLOW),
    I(0x06, 0, 0, ERRNO(22)),
  };
  data.arch = SHIM_SECCOMP_ARCH_AARCH64;
  data.args[0] = 0;
  assert(shim_seccomp_bpf_verify(arg, 5) == 0);
  assert(shim_seccomp_bpf_eval(arg, 5, &data) == ALLOW);
  data.args[0] = 1;
  assert(shim_seccomp_bpf_eval(arg, 5, &data) == ERRNO(22));

  struct shim_sock_filter bad[] = {
    I(0x20, 0, 0, 63), I(0x06, 0, 0, ALLOW),
  };
  assert(shim_seccomp_bpf_verify(bad, 2) != 0); /* unaligned/OOB load */
  bad[0] = (struct shim_sock_filter)I(0x05, 0, 0, 2);
  assert(shim_seccomp_bpf_verify(bad, 2) != 0); /* escaping jump */
  bad[0] = (struct shim_sock_filter)I(0x06, 0, 0, 0x7fc00000u);
  assert(shim_seccomp_bpf_verify(bad, 2) != 0); /* unsupported USER_NOTIF */
  bad[0] = (struct shim_sock_filter)I(0x15, 0, 0, 1);
  assert(shim_seccomp_bpf_verify(bad, 2) == 0);
  bad[0].jf = 1;
  assert(shim_seccomp_bpf_verify(bad, 2) != 0);
  assert(shim_seccomp_bpf_verify(p, 0) != 0);
  assert(shim_seccomp_bpf_eval(bad, 2, &data) == ERRNO(1));
  puts("SECCOMP_BPF_SUBSET_OK");
}
