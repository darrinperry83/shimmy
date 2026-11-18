#ifndef SHIMMY_GUEST_KEY_SHARED_H
#define SHIMMY_GUEST_KEY_SHARED_H
#include <stddef.h>
#include <stdint.h>

/* Cross-process, guest-private object storage. This is not a Linux keyring
 * syscall interface: keyring links and Linux permissions are still absent. */
typedef struct gks_namespace gks_namespace;
enum { GKS_MAX_KEYS = 64, GKS_MAX_RINGS = 16, GKS_MAX_DESCRIPTION = 255,
       GKS_MAX_PAYLOAD = 4096 };

gks_namespace *gks_open(const char *private_group_directory);
void gks_close(gks_namespace *ns);
long gks_insert_user(gks_namespace *ns, uint32_t owner, const char *description,
                     const void *payload, size_t length);
long gks_find_user(gks_namespace *ns, uint32_t caller, const char *description);
long gks_read(gks_namespace *ns, uint32_t caller, int32_t serial,
              void *buffer, size_t capacity);
long gks_revoke(gks_namespace *ns, uint32_t caller, int32_t serial);
/* Explicit owner-only ring primitives. No ambient process subscription yet. */
long gks_ring_create(gks_namespace *ns, uint32_t owner);
long gks_ring_link(gks_namespace *ns, uint32_t caller, int32_t ring, int32_t key);
long gks_ring_search_user(gks_namespace *ns, uint32_t caller, int32_t ring,
                          const char *description);
long gks_ring_unlink(gks_namespace *ns, uint32_t caller, int32_t ring, int32_t key);
#endif
