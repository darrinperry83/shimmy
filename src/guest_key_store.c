#include "guest_key_store.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct gk_key {
  int32_t serial;
  uint32_t owner;
  char description[GK_MAX_DESCRIPTION + 1];
  unsigned char payload[GK_MAX_PAYLOAD];
  size_t length;
  int occupied;
  int revoked;
};

struct gk_store {
  pthread_mutex_t lock;
  int32_t next_serial;
  struct gk_key keys[GK_MAX_KEYS];
};

static void erase(void *p, size_t n) {
  volatile unsigned char *v = p;
  while (n--) *v++ = 0;
}

gk_store *gk_store_create(void) {
  gk_store *store = calloc(1, sizeof *store);
  if (!store) return NULL;
  if (pthread_mutex_init(&store->lock, NULL)) { free(store); return NULL; }
  store->next_serial = 1;
  return store;
}

void gk_store_destroy(gk_store *store) {
  if (!store) return;
  for (size_t i = 0; i < GK_MAX_KEYS; i++)
    erase(store->keys[i].payload, sizeof store->keys[i].payload);
  pthread_mutex_destroy(&store->lock);
  erase(store, sizeof *store);
  free(store);
}

long gk_store_insert_user(gk_store *store, uint32_t owner, const char *description,
                          const void *payload, size_t length) {
  if (!store || !description || (length && !payload)) return -EINVAL;
  size_t dlen = strnlen(description, GK_MAX_DESCRIPTION + 1);
  if (!dlen || dlen > GK_MAX_DESCRIPTION || length > GK_MAX_PAYLOAD) return -EINVAL;
  pthread_mutex_lock(&store->lock);
  struct gk_key *slot = NULL;
  for (size_t i = 0; i < GK_MAX_KEYS; i++)
    if (!store->keys[i].occupied) { slot = &store->keys[i]; break; }
  if (!slot || store->next_serial == INT32_MAX) {
    pthread_mutex_unlock(&store->lock);
    return -ENOSPC;
  }
  slot->serial = store->next_serial++;
  slot->owner = owner;
  memcpy(slot->description, description, dlen + 1);
  if (length) memcpy(slot->payload, payload, length);
  slot->length = length;
  slot->occupied = 1;
  slot->revoked = 0;
  long result = slot->serial;
  pthread_mutex_unlock(&store->lock);
  return result;
}

long gk_store_find_user(gk_store *store, uint32_t caller, const char *description) {
  if (!store || !description) return -EINVAL;
  size_t dlen = strnlen(description, GK_MAX_DESCRIPTION + 1);
  if (!dlen || dlen > GK_MAX_DESCRIPTION) return -EINVAL;
  pthread_mutex_lock(&store->lock);
  long result = -ENOENT;
  for (size_t i = 0; i < GK_MAX_KEYS; i++) {
    struct gk_key *key = &store->keys[i];
    if (key->occupied && !key->revoked && key->owner == caller &&
        !strcmp(key->description, description)) { result = key->serial; break; }
  }
  pthread_mutex_unlock(&store->lock);
  return result;
}

long gk_store_read(gk_store *store, uint32_t caller, int32_t serial,
                   void *buffer, size_t capacity) {
  if (!store || serial <= 0 || (capacity && !buffer)) return -EINVAL;
  pthread_mutex_lock(&store->lock);
  long result = -ENOENT;
  for (size_t i = 0; i < GK_MAX_KEYS; i++) {
    struct gk_key *key = &store->keys[i];
    if (!key->occupied || key->serial != serial) continue;
    if (key->owner != caller) result = -EACCES;
    else if (key->revoked) result = -ENOENT;
    else if (capacity < key->length) result = -ERANGE;
    else {
      if (key->length) memcpy(buffer, key->payload, key->length);
      result = (long)key->length;
    }
    break;
  }
  pthread_mutex_unlock(&store->lock);
  return result;
}

long gk_store_revoke(gk_store *store, uint32_t caller, int32_t serial) {
  if (!store || serial <= 0) return -EINVAL;
  pthread_mutex_lock(&store->lock);
  long result = -ENOENT;
  for (size_t i = 0; i < GK_MAX_KEYS; i++) {
    struct gk_key *key = &store->keys[i];
    if (!key->occupied || key->serial != serial) continue;
    if (key->owner != caller) result = -EACCES;
    else if (!key->revoked) {
      erase(key->payload, sizeof key->payload);
      key->length = 0;
      key->revoked = 1;
      result = 0;
    }
    break;
  }
  pthread_mutex_unlock(&store->lock);
  return result;
}
