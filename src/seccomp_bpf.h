#ifndef SHIMMY_SECCOMP_BPF_H
#define SHIMMY_SECCOMP_BPF_H

#include <stddef.h>
#include <stdint.h>

/* Linux UAPI layouts. This module is a verifier/evaluator, not an installer. */
struct shim_sock_filter {
  uint16_t code;
  uint8_t jt, jf;
  uint32_t k;
};

struct shim_seccomp_data {
  int32_t nr;
  uint32_t arch;
  uint64_t instruction_pointer;
  uint64_t args[6];
};

#define SHIM_SECCOMP_ARCH_AARCH64 0xc00000b7u
#define SHIM_SECCOMP_RET_ALLOW   0x7fff0000u
#define SHIM_SECCOMP_RET_ERRNO   0x00050000u
/* The standard libseccomp wrong-architecture fallback. Our evaluator always
 * reports arch == AARCH64, so these branches are unreachable; they are accepted
 * so real libseccomp-generated filters verify, and fail closed if ever taken. */
#define SHIM_SECCOMP_RET_KILL_THREAD  0x00000000u
#define SHIM_SECCOMP_RET_KILL_PROCESS 0x80000000u

/* 0 success, -1 for an unsupported or unsafe program. ALLOW, ERRNO and the
 * wrong-architecture KILL fallbacks are accepted. The caller must snapshot the
 * instructions before use. */
int shim_seccomp_bpf_verify(const struct shim_sock_filter *insns, size_t count);

/* Only call after verify. Invalid input fails closed with ERRNO(EPERM). */
uint32_t shim_seccomp_bpf_eval(const struct shim_sock_filter *insns, size_t count,
                              const struct shim_seccomp_data *data);

/* For a private immutable copy that already passed verify at construction. */
uint32_t shim_seccomp_bpf_eval_verified(const struct shim_sock_filter *insns,
                                       size_t count,
                                       const struct shim_seccomp_data *data);

#endif
