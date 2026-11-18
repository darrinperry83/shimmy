#ifndef SHIMMY_SECCOMP_RUNTIME_H
#define SHIMMY_SECCOMP_RUNTIME_H

#include "seccomp_bpf.h"
#include <stdint.h>

/* Immutable, process-private policy. Guest syscall 277 does not install it yet. */
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
