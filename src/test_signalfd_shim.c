#include "signalfd_shim.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
  uint64_t usr2 = 1ULL << (12 - 1), term = 1ULL << (15 - 1);
  assert(signalfd4_shim(-1, &usr2, 7, 0x80800) == -22);
  assert(signalfd4_shim(-1, &usr2, 8, 0x80801) == -22);
  int fd = (int)signalfd4_shim(-1, &usr2, 8, 0x80800);
  assert(fd >= 0);
  assert((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
  assert((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
  struct pollfd p = {.fd = fd, .events = POLLIN};
  assert(poll(&p, 1, 0) == 0);
  assert(kill(getpid(), SIGUSR2) == 0);
  assert(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN));
  uint8_t record[128];
  assert(read(fd, record, sizeof record) == sizeof record);
  assert(*(uint32_t*)record == 12);
  assert(*(uint32_t*)(record + 12) == (uint32_t)getpid());
  assert(read(fd, record, sizeof record) == -1 && errno == EAGAIN);
  assert(signalfd4_shim(fd, &term, 8, 0x80800) == fd);
  assert(kill(getpid(), SIGTERM) == 0);
  int ready;
  do { ready = poll(&p, 1, 1000); } while (ready < 0 && errno == EINTR);
  assert(ready == 1);
  assert(read(fd, record, sizeof record) == sizeof record);
  assert(*(uint32_t*)record == 15);
  signalfd_shim_closed(fd);
  close(fd);
  assert(signalfd4_shim(fd, &term, 8, 0) == -9);
  struct sigaction action;
  assert(sigaction(SIGTERM, NULL, &action) == 0 && action.sa_handler == SIG_DFL);
  uint64_t childmask = 1ULL << (17 - 1);
  int childfd = (int)signalfd4_shim(-1, &childmask, 8, 0x80800);
  assert(childfd >= 0);
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) _exit(7);
  p.fd = childfd;
  do { ready = poll(&p, 1, 1000); } while (ready < 0 && errno == EINTR);
  assert(ready == 1);
  assert(read(childfd, record, sizeof record) == sizeof record);
  assert(*(uint32_t*)record == 17);
  assert(*(uint32_t*)(record + 12) == (uint32_t)child);
  assert(*(int32_t*)(record + 40) == 7);
  assert(waitpid(child, NULL, 0) == child);
  signalfd_shim_closed(childfd);
  close(childfd);
  printf("PASS: signalfd readiness, 128-byte records, mask update, nonblock, close\n");
  return 0;
}
