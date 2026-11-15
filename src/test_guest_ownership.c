#include "guest_ownership.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
  char root[] = "/tmp/shimmy-ownership-XXXXXX";
  assert(mkdtemp(root));
  char path[1024];
  snprintf(path, sizeof path, "%s/var", root); assert(mkdir(path, 0755) == 0);
  snprintf(path, sizeof path, "%s/var/lib", root); assert(mkdir(path, 0755) == 0);
  snprintf(path, sizeof path, "%s/file", root);
  FILE *f = fopen(path, "w"); assert(f); fclose(f);
  struct stat st; assert(stat(path, &st) == 0);
  assert(chmod(root, 0555) == 0);

  guest_ownership_set_root(root);
  assert(guest_ownership_set(&st, 974, 975) == 0);
  uint32_t uid, gid;
  assert(guest_ownership_get(&st, &uid, &gid) == 0);
  assert(uid == 974 && gid == 975);
  char metadata[1024];
  snprintf(metadata, sizeof metadata, "%s/var/lib/.rdshim-ownership", root);
  assert(stat(metadata, &st) == 0 && S_ISDIR(st.st_mode));
  assert(chmod(root, 0755) == 0);

  char legacy_root[] = "/tmp/shimmy-legacy-ownership-XXXXXX";
  assert(mkdtemp(legacy_root));
  snprintf(path, sizeof path, "%s/var", legacy_root); assert(mkdir(path, 0755) == 0);
  snprintf(path, sizeof path, "%s/var/lib", legacy_root); assert(mkdir(path, 0755) == 0);
  snprintf(path, sizeof path, "%s/.rdshim-ownership", legacy_root);
  assert(mkdir(path, 0700) == 0);
  snprintf(path, sizeof path, "%s/file", legacy_root);
  f = fopen(path, "w"); assert(f); fclose(f);
  assert(stat(path, &st) == 0);
  assert(chmod(legacy_root, 0555) == 0);
  guest_ownership_set_root(legacy_root);
  assert(guest_ownership_set(&st, 42, 43) == 0);
  assert(guest_ownership_get(&st, &uid, &gid) == 0 && uid == 42 && gid == 43);
  snprintf(metadata, sizeof metadata, "%s/var/lib/.rdshim-ownership", legacy_root);
  assert(access(metadata, F_OK) != 0);
  assert(chmod(legacy_root, 0755) == 0);
  printf("ownership metadata works with a read-only root and writable /var/lib\n");
  return 0;
}
