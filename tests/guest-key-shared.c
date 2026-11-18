#include "guest_key_shared.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char sample[] = "synthetic disposable payload";

int main(int argc, char **argv) {
  if (argc == 4 && !strcmp(argv[1], "child")) {
    gks_namespace *ns = gks_open(argv[2]);
    assert(ns);
    long id = strtol(argv[3], NULL, 10);
    char out[sizeof sample] = {0};
    assert(gks_find_user(ns, 1000, "sample") == id);
    assert(gks_read(ns, 1000, (int32_t)id, out, sizeof out) == sizeof sample);
    assert(!memcmp(out, sample, sizeof out));
    assert(gks_read(ns, 1001, (int32_t)id, out, sizeof out) == -EACCES);
    assert(gks_revoke(ns, 1001, (int32_t)id) == -EACCES);
    assert(gks_revoke(ns, 1000, (int32_t)id) == 0);
    gks_close(ns);
    return 0;
  }
  assert(argc == 1);
  char dir[] = "/tmp/shimmy-key-shared-XXXXXX";
  assert(mkdtemp(dir));
  gks_namespace *ns = gks_open(dir);
  assert(ns);
  long id = gks_insert_user(ns, 1000, "sample", sample, sizeof sample);
  assert(id > 0);
  assert(gks_find_user(ns, 1000, "sample") == id);
  assert(gks_find_user(ns, 1001, "sample") == -ENOENT);
  assert(gks_insert_user(ns, 1000, "oversize", sample, GKS_MAX_PAYLOAD + 1) == -EINVAL);

  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    char serial[32];
    snprintf(serial, sizeof serial, "%ld", id);
    execl(argv[0], argv[0], "child", dir, serial, (char *)NULL);
    _exit(111);
  }
  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(gks_find_user(ns, 1000, "sample") == -ENOENT);
  char out[sizeof sample];
  assert(gks_read(ns, 1000, (int32_t)id, out, sizeof out) == -ENOENT);

  char other[PATH_MAX], file[PATH_MAX];
  snprintf(other, sizeof other, "%s/other", dir);
  assert(mkdir(other, 0700) == 0);
  gks_namespace *isolated = gks_open(other);
  assert(isolated);
  assert(gks_find_user(isolated, 1000, "sample") == -ENOENT);
  long other_id = gks_insert_user(isolated, 1000, "other", sample, sizeof sample);
  assert(other_id > 0);
  assert(gks_find_user(ns, 1000, "other") == -ENOENT);
  gks_close(isolated);
  gks_close(ns);

  assert(chmod(other, 0755) == 0);
  errno = 0;
  assert(gks_open(other) == NULL && errno == EACCES);
  assert(chmod(other, 0700) == 0);
  snprintf(file, sizeof file, "%s/.shimmy-guest-keys", other);
  assert(chmod(file, 0644) == 0);
  errno = 0;
  assert(gks_open(other) == NULL && errno == EACCES);
  assert(chmod(file, 0600) == 0);

  snprintf(file, sizeof file, "%s/.shimmy-guest-keys", other);
  assert(unlink(file) == 0 && rmdir(other) == 0);
  snprintf(file, sizeof file, "%s/.shimmy-guest-keys", dir);
  assert(unlink(file) == 0 && rmdir(dir) == 0);
  puts("guest shared key objects: PASS (fork/exec, ownership, isolation, revoke)");
}
