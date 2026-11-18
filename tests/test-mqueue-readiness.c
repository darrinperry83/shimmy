#include "mqueue_shared.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static void child_send(const char* dir) {
  int fd = -1;
  assert(mq_shared_open(dir, "/ready", 0, 0, 0, 0, &fd) == 0);
  usleep(20000);
  assert(mq_shared_send(fd, "x", 1, 1) == 0);
  close(fd);
  _exit(0);
}

static void child_receive(const char* dir) {
  int fd = -1;
  assert(mq_shared_open(dir, "/ready", 0, 0, 0, 0, &fd) == 0);
  usleep(20000);
  char buf[2];
  size_t length = 0;
  assert(mq_shared_receive(fd, buf, sizeof buf, &length, NULL) == 0);
  close(fd);
  _exit(0);
}

static void wait_child(pid_t child) {
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void) {
  char dir[] = "/tmp/shimmy-mqueue-ready.XXXXXX";
  assert(mkdtemp(dir));
  int fd = -1, ready = -1;
  assert(mq_shared_open(dir, "/ready", 1, 1, 1, 2, &fd) == 0);
  assert(mq_shared_wait_ready(fd, MQ_SHARED_READABLE, 0, &ready) == 0 && ready == 0);
  assert(mq_shared_wait_ready(fd, MQ_SHARED_READABLE, 20, &ready) == 0 && ready == 0);
  assert(mq_shared_wait_ready(fd, MQ_SHARED_WRITABLE, 0, &ready) == 0 &&
         ready == MQ_SHARED_WRITABLE);
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) child_send(dir);
  assert(mq_shared_wait_ready(fd, MQ_SHARED_READABLE, 1000, &ready) == 0 &&
         ready == MQ_SHARED_READABLE);
  wait_child(child);
  assert(mq_shared_wait_ready(fd, MQ_SHARED_WRITABLE, 0, &ready) == 0 && ready == 0);
  child = fork();
  assert(child >= 0);
  if (child == 0) child_receive(dir);
  assert(mq_shared_wait_ready(fd, MQ_SHARED_WRITABLE, 1000, &ready) == 0 &&
         ready == MQ_SHARED_WRITABLE);
  wait_child(child);
  assert(mq_shared_unlink(dir, "/ready") == 0);
  close(fd);
  char lockfile[256];
  snprintf(lockfile, sizeof lockfile, "%s/.lock", dir);
  assert(unlink(lockfile) == 0);
  assert(rmdir(dir) == 0);
  puts("mqueue readiness PASS");
  return 0;
}
