#include "guest_cred_registry.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CRED_MAGIC 0x52444352u
struct cred_record { uint32_t magic, pid, uid, gid; };
static char g_group[PATH_MAX];

static int record_path(char *out, size_t capacity, pid_t pid) {
  if (!g_group[0] || pid <= 0) return -1;
  int n = snprintf(out, capacity, "%s/.rdx-cred-%ld", g_group, (long)pid);
  return n > 0 && (size_t)n < capacity ? 0 : -1;
}

int guest_cred_registry_update(uint32_t uid, uint32_t gid) {
  char target[PATH_MAX], temp[PATH_MAX];
  if (record_path(target, sizeof target, getpid()) < 0) return -1;
  int n = snprintf(temp, sizeof temp, "%s/.rdx-cred-tmp-%ld-XXXXXX", g_group, (long)getpid());
  if (n <= 0 || (size_t)n >= sizeof temp) return -1;
  int fd = mkstemp(temp);
  if (fd < 0) return -1;
  struct cred_record rec = {CRED_MAGIC, (uint32_t)getpid(), uid, gid};
  int ok = fchmod(fd, 0600) == 0 && write(fd, &rec, sizeof rec) == sizeof rec &&
           rename(temp, target) == 0;
  close(fd);
  if (!ok) { unlink(temp); return -1; }
  return 0;
}

int guest_cred_registry_init(const char *group, uint32_t uid, uint32_t gid) {
  if (!group || group[0] != '/') return -1;
  int n = snprintf(g_group, sizeof g_group, "%s", group);
  if (n <= 0 || (size_t)n >= sizeof g_group) { g_group[0] = 0; return -1; }
  return guest_cred_registry_update(uid, gid);
}

int guest_cred_registry_lookup(pid_t pid, uint32_t *uid, uint32_t *gid) {
  if (!uid || !gid || pid <= 0) return -1;
  char path[PATH_MAX];
  if (record_path(path, sizeof path, pid) < 0) return -1;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return -1;
  struct stat st;
  struct cred_record rec;
  int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
           (st.st_mode & 077) == 0 && st.st_size == sizeof rec &&
           read(fd, &rec, sizeof rec) == sizeof rec && rec.magic == CRED_MAGIC &&
           rec.pid == (uint32_t)pid;
  close(fd);
  if (!ok) return -1;
  *uid = rec.uid;
  *gid = rec.gid;
  return 0;
}
