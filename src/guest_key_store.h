#ifndef SHIMMY_GUEST_KEY_STORE_H
#define SHIMMY_GUEST_KEY_STORE_H

#include <stddef.h>
#include <stdint.h>

/* Private object-store primitive, not the Linux keyring ABI. No syscall may
 * return one of these serials until keyring links and fork/exec sharing exist. */
typedef struct gk_store gk_store;

enum { GK_MAX_KEYS = 64, GK_MAX_DESCRIPTION = 255, GK_MAX_PAYLOAD = 4096 };

gk_store *gk_store_create(void);
void gk_store_destroy(gk_store *store);
long gk_store_insert_user(gk_store *store, uint32_t owner, const char *description,
                          const void *payload, size_t length);
long gk_store_find_user(gk_store *store, uint32_t caller, const char *description);
long gk_store_read(gk_store *store, uint32_t caller, int32_t serial,
                   void *buffer, size_t capacity);
long gk_store_revoke(gk_store *store, uint32_t caller, int32_t serial);

#endif
