#include "mqueue_shared.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
  char dir[] = "/tmp/shimmy-mq-fork-lock.XXXXXX";
  assert(mkdtemp(dir));
  int fd = -1;
  assert(mq_shared_open(dir, "/fork", 1, 1, 32, 8, &fd) == 0);
  pid_t children[2];
  for (int j = 0; j < 2; ++j) {
    children[j] = fork();
    assert(children[j] >= 0);
    if (children[j] == 0) {
      for (int i = 0; i < 16; ++i) {
        char message[8];
        snprintf(message, sizeof message, "%c%02d", j ? 'B' : 'A', i);
        assert(mq_shared_send(fd, message, 3, (unsigned)j) == 0);
      }
      assert(mq_shared_close(fd) == 0);
      _exit(0);
    }
  }
  for (int j = 0; j < 2; ++j) {
    int status = 0;
    assert(waitpid(children[j], &status, 0) == children[j]);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  unsigned count = 0;
  assert(mq_shared_status(fd, &count, NULL, NULL, NULL, NULL) == 0);
  assert(count == 32);
  int seen[2][16] = {{0}};
  for (int i = 0; i < 32; ++i) {
    char message[8] = {0};
    size_t length = 0;
    unsigned priority = 0;
    assert(mq_shared_receive(fd, message, sizeof message, &length, &priority) == 0);
    assert(length == 3);
    int group = message[0] == 'A' ? 0 : 1;
    assert(message[0] == 'A' || message[0] == 'B');
    int index = (message[1] - '0') * 10 + (message[2] - '0');
    assert(index >= 0 && index < 16 && priority == (unsigned)group);
    assert(!seen[group][index]);
    seen[group][index] = 1;
  }
  for (int j = 0; j < 2; ++j)
    for (int i = 0; i < 16; ++i) assert(seen[j][i]);
  assert(mq_shared_close(fd) == 0);
  assert(mq_shared_unlink(dir, "/fork") == 0);
  char lockfile[256];
  snprintf(lockfile, sizeof lockfile, "%s/.lock", dir);
  assert(unlink(lockfile) == 0);
  assert(rmdir(dir) == 0);
  puts("mqueue fork lock PASS");
  return 0;
}
