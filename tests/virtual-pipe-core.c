#include "virtual_pipe_core.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc == 4 && !strcmp(argv[1], "child")) {
    vp_core *in = vp_core_open(argv[2]), *out = vp_core_open(argv[3]);
    assert(in && out);
    assert(vp_core_tee(in, out, 4) == 4);
    assert(vp_core_pending(in) == 6 && vp_core_pending(out) == 4);
    vp_core_close(in); vp_core_close(out);
    return 0;
  }
  assert(argc == 1);
  char dir[] = "/tmp/shimmy-vpipe-test-XXXXXX";
  assert(mkdtemp(dir));
  char in_path[PATH_MAX], out_path[PATH_MAX];
  vp_core *in = vp_core_create(dir, in_path, sizeof in_path);
  vp_core *out = vp_core_create(dir, out_path, sizeof out_path);
  assert(in && out);
  vp_core *duplicate = vp_core_open(in_path);
  assert(duplicate);
  assert(vp_core_pending(in) == 0 && vp_core_space(in) == VP_CAPACITY);
  assert(vp_core_read(in, (char[1]){0}, 1) == -EAGAIN);
  assert(vp_core_write(in, "abcdef", 6) == 6);
  assert(vp_core_pending(duplicate) == 6);
  assert(vp_core_tee(in, duplicate, 1) == -EINVAL);

  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    execl(argv[0], argv[0], "child", in_path, out_path, (char *)NULL);
    _exit(111);
  }
  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  char a[8] = {0}, b[8] = {0};
  assert(vp_core_read(out, a, 4) == 4 && !memcmp(a, "abcd", 4));
  assert(vp_core_read(duplicate, b, 6) == 6 && !memcmp(b, "abcdef", 6));
  assert(vp_core_pending(in) == 0 && vp_core_pending(out) == 0);

  unsigned char full[VP_CAPACITY], drained[VP_CAPACITY];
  memset(full, 'Q', sizeof full);
  assert(vp_core_write(in, full, sizeof full) == VP_CAPACITY);
  assert(vp_core_space(in) == 0 && vp_core_write(in, "X", 1) == -EAGAIN);
  assert(vp_core_read(in, drained, VP_CAPACITY - 2) == VP_CAPACITY - 2);
  assert(vp_core_write(in, "YZ", 2) == 2);
  assert(vp_core_tee(in, out, 4) == 4);
  assert(vp_core_read(out, a, 4) == 4 && !memcmp(a, "QQYZ", 4));
  assert(vp_core_read(in, b, 4) == 4 && !memcmp(b, "QQYZ", 4));
  vp_core_close(duplicate); vp_core_close(in); vp_core_close(out);
  assert(unlink(in_path) == 0 && unlink(out_path) == 0 && rmdir(dir) == 0);
  puts("virtual pipe core: PASS (tee preserves input, duplicate open, fork/exec, wrap, space)");
}
