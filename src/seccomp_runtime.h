#ifndef SHIMMY_SECCOMP_RUNTIME_H
#define SHIMMY_SECCOMP_RUNTIME_H

#include "seccomp_bpf.h"
#include <stdint.h>

/* Upper bound on a program length, shared with the install path (seccomp(2) and
 * prctl(PR_SET_SECCOMP)). Must be large enough for systemd's generated filters:
 * SystemCallFilter=@system-service (polkit.service, upower.service, ...) compiles
 * to several hundred classic-BPF instructions, so 256 rejected the whole filter
 * with EINVAL and the service died at "step SECCOMP". Match the verifier's own
 * SHIM_BPF_MAX_INSNS ceiling (seccomp_bpf.c). */
#define SHIM_SECCOMP_MAX_INSNS 4096u

/* Immutable, process-private policy. Installed via guest_seccomp_install() and
 * enforced by shim_seccomp_state_check() before every guest syscall. */
struct shim_seccomp_state;

struct shim_seccomp_state *shim_seccomp_state_new(
    const struct shim_sock_filter *program, unsigned count, int no_new_privs);
int shim_seccomp_state_no_new_privs(const struct shim_seccomp_state *state);

/* Returns 1 and a raw negative Linux errno when denied, 0 when allowed. */
int shim_seccomp_state_check(const struct shim_seccomp_state *state,
                            uint32_t nr, const uint64_t args[6],
                            uint64_t instruction_pointer, long *result);

/* Capability transfer: an unlinked, read-only pipe end holds a validated
 * snapshot across host exec. Caller protects its fd and strips guest env. */
int shim_seccomp_state_export_fd(const struct shim_seccomp_state *state);
struct shim_seccomp_state *shim_seccomp_state_import_fd(int fd);

#endif
