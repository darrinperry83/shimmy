#pragma once
#include <stdint.h>
#include <sys/stat.h>
/* RDUID0 metadata only; never changes Darwin ownership or permissions. */
void guest_ownership_set_root(const char *root);
int guest_ownership_get(const struct stat *st, uint32_t *uid, uint32_t *gid);
int guest_ownership_set(const struct stat *st, uint32_t uid, uint32_t gid);
