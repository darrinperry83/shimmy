#include "guest_cred_registry.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "child") == 0) {
    if (guest_cred_registry_init(argv[2], 0, 0) < 0) return 2;
    if (guest_cred_registry_update(974, 974) < 0) return 3;
    sleep(3);
    return 0;
  }
  if (argc != 1) return 2;
  char group[] = "/tmp/rd-cred-registry-XXXXXX";
  if (!mkdtemp(group)) return 2;
  if (guest_cred_registry_init(group, 0, 0) < 0) return 3;
  uint32_t uid = UINT32_MAX, gid = UINT32_MAX;
  if (guest_cred_registry_lookup(getpid(), &uid, &gid) < 0 || uid || gid) return 4;
  pid_t child;
  char *child_argv[] = {argv[0], "child", group, NULL};
  if (posix_spawn(&child, argv[0], NULL, NULL, child_argv, environ)) return 5;
  int found = 0;
  for (int i = 0; i < 100; i++) {
    if (guest_cred_registry_lookup(child, &uid, &gid) == 0 && uid == 974 && gid == 974) {
      found = 1;
      break;
    }
    usleep(10000);
  }
  char path[256];
  snprintf(path, sizeof path, "%s/.rdx-cred-%ld", group, (long)child);
  chmod(path, 0644);
  int open_mode_rejected = guest_cred_registry_lookup(child, &uid, &gid) < 0;
  chmod(path, 0600);
  unlink(path);
  char own_path[256];
  snprintf(own_path, sizeof own_path, "%s/.rdx-cred-%ld", group, (long)getpid());
  if (symlink(own_path, path) < 0) return 7;
  int symlink_rejected = guest_cred_registry_lookup(child, &uid, &gid) < 0;
  kill(child, SIGTERM);
  waitpid(child, NULL, 0);
  unlink(path);
  unlink(own_path);
  rmdir(group);
  if (!found || !open_mode_rejected || !symlink_rejected) return 6;
  printf("GUEST_CRED_REGISTRY_PASS child=974 unsafe-mode-and-symlink=rejected\n");
  return 0;
}
