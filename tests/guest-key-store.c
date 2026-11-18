#include "guest_key_store.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  gk_store *a = gk_store_create(), *b = gk_store_create();
  assert(a && b);
  const char secret[] = "disposable-key-payload";
  long id = gk_store_insert_user(a, 1000, "fixture", secret, sizeof secret);
  assert(id > 0);
  assert(gk_store_find_user(a, 1000, "fixture") == id);
  assert(gk_store_find_user(a, 1001, "fixture") == -ENOENT);
  assert(gk_store_find_user(b, 1000, "fixture") == -ENOENT);
  char out[sizeof secret] = {0};
  assert(gk_store_read(a, 1001, (int32_t)id, out, sizeof out) == -EACCES);
  assert(gk_store_read(a, 1000, (int32_t)id, out, sizeof out - 1) == -ERANGE);
  assert(gk_store_read(a, 1000, (int32_t)id, out, sizeof out) == sizeof secret);
  assert(!memcmp(out, secret, sizeof out));
  assert(gk_store_revoke(a, 1001, (int32_t)id) == -EACCES);
  assert(gk_store_revoke(a, 1000, (int32_t)id) == 0);
  assert(gk_store_find_user(a, 1000, "fixture") == -ENOENT);
  assert(gk_store_read(a, 1000, (int32_t)id, out, sizeof out) == -ENOENT);
  assert(gk_store_revoke(a, 1000, (int32_t)id) == -ENOENT);
  assert(gk_store_insert_user(a, 1, "", secret, sizeof secret) == -EINVAL);
  assert(gk_store_insert_user(a, 1, "fixture", secret, GK_MAX_PAYLOAD + 1) == -EINVAL);
  assert(gk_store_insert_user(a, 1, "fixture", NULL, 1) == -EINVAL);
  for (int i = 0; i < GK_MAX_KEYS - 1; i++) {
    char desc[32];
    snprintf(desc, sizeof desc, "key-%d", i);
    assert(gk_store_insert_user(a, 1, desc, NULL, 0) > 0);
  }
  assert(gk_store_insert_user(a, 1, "full", NULL, 0) == -ENOSPC);
  gk_store_destroy(a);
  gk_store_destroy(b);
  puts("guest key object store: PASS");
}
