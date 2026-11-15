// Standalone regression for an inherited timerfd in a real fork child.
// clang -arch arm64 -O2 -Wall -I src tests/test_timerfd_fork.c src/timerfd_shim.c -o /tmp/shimmy-test-timerfd-fork
#include "timerfd_shim.h"
#include <assert.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int shim_fd_reserve(int fd) { return fd; }
void shim_fd_unreserve(int fd) { (void)fd; }

static void expect_fire(int fd) {
  linux_itimerspec spec = {.it_value = {.tv_nsec = 20000000}};
  assert(timerfd_settime_shim(fd, 0, &spec, NULL) == 0);
  struct pollfd p = {.fd = fd, .events = POLLIN};
  assert(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN));
  uint64_t ticks = 0;
  assert(read(fd, &ticks, sizeof ticks) == sizeof ticks && ticks == 1);
}

int main(void) {
  int inherited = (int)timerfd_create_shim(1, 0);
  assert(inherited >= 0);
  timerfd_shim_prepare_fork();
  pid_t child = fork();
  if (child == 0) {
    timerfd_shim_child_after_fork();
    expect_fire(inherited);
    int fresh = (int)timerfd_create_shim(1, 0);
    assert(fresh >= 0);
    expect_fire(fresh);
    timerfd_shim_closed(fresh); close(fresh);
    timerfd_shim_closed(inherited); close(inherited);
    _exit(0);
  }
  timerfd_shim_parent_after_fork();
  assert(child > 0);
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  // Parent's original source still works after the child used its own waiter.
  expect_fire(inherited);
  timerfd_shim_closed(inherited); close(inherited);
  puts("PASS: inherited and newly created child timerfds; parent remains usable");
  return 0;
}
