#include "guest_key_subscription.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc == 5 && !strcmp(argv[1], "child")) {
    gks_namespace *ns = gks_open(argv[2]);
    assert(ns);
    gks_subscription *sub = gks_subscription_new(ns, 1000);
    assert(sub);
    long ring = strtol(argv[3], NULL, 10);
    long key = strtol(argv[4], NULL, 10);
    assert(gks_subscription_set(sub, GKS_SESSION_RING, (int32_t)ring) == 0);
    assert(gks_subscription_search_user(sub, "same") == key);
    gks_subscription_free(sub);
    gks_close(ns);
    return 0;
  }
  assert(argc == 1);
  char dir[] = "/tmp/shimmy-key-subscription-XXXXXX";
  assert(mkdtemp(dir));
  gks_namespace *ns = gks_open(dir);
  assert(ns);
  long rings[3], keys[3];
  for (int i = 0; i < 3; i++) {
    rings[i] = gks_ring_create(ns, 1000);
    keys[i] = gks_insert_user(ns, 1000, "same", "x", 1);
    assert(rings[i] > 0 && keys[i] > 0);
    assert(gks_ring_link(ns, 1000, (int32_t)rings[i], (int32_t)keys[i]) == 0);
  }
  gks_subscription *sub = gks_subscription_new(ns, 1000);
  gks_subscription *wrong = gks_subscription_new(ns, 1001);
  assert(sub && wrong);
  assert(gks_subscription_set(wrong, GKS_SESSION_RING, (int32_t)rings[2]) == -EACCES);
  assert(gks_subscription_search_user(wrong, "same") == -ENOENT);
  for (int i = 0; i < 3; i++)
    assert(gks_subscription_set(sub, (enum gks_scope)i, (int32_t)rings[i]) == 0);
  assert(gks_subscription_search_user(sub, "same") == keys[0]);
  assert(gks_subscription_set(sub, GKS_THREAD_RING, 0) == 0);
  assert(gks_subscription_search_user(sub, "same") == keys[1]);
  assert(gks_subscription_set(sub, GKS_THREAD_RING, (int32_t)rings[0]) == 0);
  gks_subscription *forked = gks_subscription_fork_child(sub);
  assert(forked && gks_subscription_search_user(forked, "same") == keys[2]);
  assert(gks_subscription_session_serial(forked) == rings[2]);
  gks_subscription_after_exec(sub);
  assert(gks_subscription_search_user(sub, "same") == keys[2]);

  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    char ring_arg[32], key_arg[32];
    snprintf(ring_arg, sizeof ring_arg, "%ld", rings[2]);
    snprintf(key_arg, sizeof key_arg, "%ld", keys[2]);
    execl(argv[0], argv[0], "child", dir, ring_arg, key_arg, (char *)NULL);
    _exit(111);
  }
  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  gks_subscription_free(forked);
  gks_subscription_free(wrong);
  gks_subscription_free(sub);
  gks_close(ns);
  char file[PATH_MAX];
  snprintf(file, sizeof file, "%s/.shimmy-guest-keys", dir);
  assert(unlink(file) == 0 && rmdir(dir) == 0);
  puts("guest key subscriptions: PASS (precedence, fork/exec model, UID)");
}
