#include "mqueue_shared.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void check_message(int fd, const char* expected, unsigned expected_prio) {
  char buf[16] = {0};
  size_t length = 0;
  unsigned priority = 0;
  assert(mq_shared_receive(fd, buf, sizeof buf, &length, &priority) == 0);
  assert(length == strlen(expected) && memcmp(buf, expected, length) == 0);
  assert(priority == expected_prio);
}

int main(int argc, char** argv) {
  if (argc == 3 && strcmp(argv[1], "--child") == 0) {
    int fd = -1;
    assert(mq_shared_open(argv[2], "/queue", 0, 0, 0, 0, &fd) == 0);
    assert(mq_shared_send(fd, "exec", 4, 3) == 0);
    close(fd);
    return 0;
  }
  char dir[] = "/tmp/shimmy-mqueue-shared.XXXXXX";
  assert(mkdtemp(dir));
  int fd = -1;
  assert(mq_shared_open(dir, "bad", 1, 0, 2, 16, &fd) == EINVAL);
  assert(mq_shared_open(dir, "/queue", 0, 0, 0, 0, &fd) == ENOENT);
  assert(mq_shared_open(dir, "/queue", 1, 1, 2, 16, &fd) == 0);
  int duplicate = -1;
  assert(mq_shared_open(dir, "/queue", 1, 1, 2, 16, &duplicate) == EEXIST);
  unsigned count = 9;
  int readable = -1, writable = -1;
  assert(mq_shared_status(fd, &count, NULL, NULL, &readable, &writable) == 0);
  assert(count == 0 && !readable && writable);
  char small[2];
  size_t length = 0;
  assert(mq_shared_receive(fd, small, sizeof small, &length, NULL) == EMSGSIZE);
  char buf[16];
  assert(mq_shared_receive(fd, buf, sizeof buf, &length, NULL) == EAGAIN);

  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    int childfd = -1;
    assert(mq_shared_open(dir, "/queue", 0, 0, 0, 0, &childfd) == 0);
    assert(mq_shared_send(childfd, "low", 3, 1) == 0);
    assert(mq_shared_send(childfd, "high", 4, 7) == 0);
    close(childfd);
    _exit(0);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(mq_shared_status(fd, &count, NULL, NULL, &readable, &writable) == 0);
  assert(count == 2 && readable && !writable);
  assert(mq_shared_send(fd, "full", 4, 1) == EAGAIN);
  check_message(fd, "high", 7);
  check_message(fd, "low", 1);

  child = fork();
  assert(child >= 0);
  if (child == 0) { execl(argv[0], argv[0], "--child", dir, (char*)NULL); _exit(127); }
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  check_message(fd, "exec", 3);

  assert(mq_shared_unlink(dir, "/queue") == 0);
  assert(mq_shared_open(dir, "/queue", 0, 0, 0, 0, &duplicate) == ENOENT);
  assert(mq_shared_send(fd, "old", 3, 2) == 0); // open file survives unlink
  assert(mq_shared_open(dir, "/queue", 1, 1, 1, 16, &duplicate) == 0);
  assert(mq_shared_receive(duplicate, buf, sizeof buf, &length, NULL) == EAGAIN);
  check_message(fd, "old", 2);
  close(fd);
  close(duplicate);
  assert(mq_shared_unlink(dir, "/queue") == 0);
  char lockfile[256];
  snprintf(lockfile, sizeof lockfile, "%s/.lock", dir);
  assert(unlink(lockfile) == 0);
  assert(rmdir(dir) == 0);
  puts("mqueue shared store PASS");
  return 0;
}
