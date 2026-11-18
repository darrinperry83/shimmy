#include "../src/seccomp_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define I(code, jt, jf, k) {code, jt, jf, k}
static const struct shim_sock_filter policy[] = {
  I(0x20, 0, 0, 0),
  I(0x15, 0, 1, 64),
  I(0x06, 0, 0, SHIM_SECCOMP_RET_ERRNO | 13),
  I(0x06, 0, 0, SHIM_SECCOMP_RET_ALLOW),
};

static void check(const struct shim_seccomp_state *s) {
  const uint64_t args[6] = {1, 2, 3, 4, 5, 6};
  long result = 123;
  assert(shim_seccomp_state_no_new_privs(s) == 1);
  assert(shim_seccomp_state_check(s, 64, args, 0, &result) == 1);
  assert(result == -13);
  assert(shim_seccomp_state_check(s, 172, args, 0, &result) == 0);
}

int main(int argc, char **argv) {
  if (argc == 3) {
    int fd = atoi(argv[2]);
    struct shim_seccomp_state *s = shim_seccomp_state_import_fd(fd);
    assert(s);
    check(s);
    close(fd);
    puts("SECCOMP_REEXEC_STATE_OK");
    return 0;
  }
  assert(!shim_seccomp_state_new(policy, 4, 0));
  struct shim_seccomp_state *s = shim_seccomp_state_new(policy, 4, 1);
  assert(s);
  check(s);
  struct shim_sock_filter mutable_policy[4];
  for (int i = 0; i < 4; ++i) mutable_policy[i] = policy[i];
  struct shim_seccomp_state *immutable = shim_seccomp_state_new(mutable_policy, 4, 1);
  assert(immutable);
  mutable_policy[2].k = SHIM_SECCOMP_RET_ALLOW;
  check(immutable); /* installation copied instructions before caller mutation */
  int bad[2];
  assert(pipe(bad) == 0);
  assert(write(bad[1], "bad", 3) == 3);
  close(bad[1]);
  assert(!shim_seccomp_state_import_fd(bad[0]));
  close(bad[0]);
  int fd = shim_seccomp_state_export_fd(s);
  assert(fd >= 0);
  struct shim_seccomp_state *copy = shim_seccomp_state_import_fd(fd);
  assert(copy);
  check(copy);
  close(fd);

  fd = shim_seccomp_state_export_fd(s);
  assert(fd >= 0);
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    check(s); /* fork retains immutable process memory */
    char number[24];
    snprintf(number, sizeof number, "%d", fd);
    execl(argv[0], argv[0], "child", number, (char *)NULL);
    _exit(127);
  }
  close(fd);
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  puts("SECCOMP_RUNTIME_STATE_OK");
}
