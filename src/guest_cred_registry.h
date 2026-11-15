#ifndef RD_GUEST_CRED_REGISTRY_H
#define RD_GUEST_CRED_REGISTRY_H

#include <stdint.h>
#include <sys/types.h>

/* Virtual credentials for kernel-identified peers in one private Shimmy boot. */
int guest_cred_registry_init(const char *socket_group, uint32_t uid, uint32_t gid);
int guest_cred_registry_update(uint32_t uid, uint32_t gid);
int guest_cred_registry_lookup(pid_t host_pid, uint32_t *uid, uint32_t *gid);

#endif
