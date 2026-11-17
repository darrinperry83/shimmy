/* Native regression: kqueue pidfds cannot cross Darwin SCM_RIGHTS. */
#include "../src/pidfd_shim.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

int linux_errno_from_host(int e) { return e; }
int shim_fd_reserve(int fd) {
  if (fd >= 256) return fd;
  int high = fcntl(fd, F_DUPFD_CLOEXEC, 256);
  if (high >= 0) close(fd);
  return high;
}
void shim_fd_unreserve(int fd) { (void)fd; }

static int send_fd(int socket_fd, int descriptor) {
  char byte = 'x', control[CMSG_SPACE(sizeof(int))] = {0};
  struct iovec iov = {&byte, 1};
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                       .msg_control = control, .msg_controllen = sizeof control};
  struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
  c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof descriptor);
  memcpy(CMSG_DATA(c), &descriptor, sizeof descriptor);
  return sendmsg(socket_fd, &msg, 0);
}

static int recv_fd(int socket_fd) {
  char byte, control[CMSG_SPACE(sizeof(int))] = {0};
  struct iovec iov = {&byte, 1};
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                       .msg_control = control, .msg_controllen = sizeof control};
  if (recvmsg(socket_fd, &msg, 0) != 1) return -1;
  struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
  if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
      c->cmsg_len != CMSG_LEN(sizeof(int))) return -1;
  int fd; memcpy(&fd, CMSG_DATA(c), sizeof fd); return fd;
}

int main(void) {
  char dir[] = "/tmp/rdhybrid-pfd-XXXXXX";
  if (!mkdtemp(dir) || setenv("RDSOCKETGROUP", dir, 1) ||
      setenv("RDNATIVE_SESSION", "1", 1) ||
      unsetenv("RDPIDFD_TRANSFER")) return 1;
  int ready[2], stop[2], channel[2];
  if (pipe(ready) || pipe(stop) || socketpair(AF_UNIX, SOCK_STREAM, 0, channel)) return 2;
  pid_t target = fork(); if (target < 0) return 3;
  if (target == 0) {
    close(ready[0]); close(stop[1]);
    if (pidfd_shim_target_init() < 0 || write(ready[1], "R", 1) != 1) _exit(4);
    char byte; if (read(stop[0], &byte, 1) != 1) _exit(5);
    _exit(0);
  }
  close(ready[1]); close(stop[0]);
  char byte; if (read(ready[0], &byte, 1) != 1) return 6;
  int local = (int)pidfd_open_shim(target, 0);
  if (local < 0 || pidfd_shim_pid(local) != target) return 7;
  if (send_fd(channel[0], local) != -1 || errno != EINVAL) return 8;
  if (pidfd_shim_transferable(channel[0]) != 0) return 9;
  int transferable = pidfd_shim_transferable(local);
  if (transferable < 0 || send_fd(channel[0], transferable) != 1) return 10;
  int received = recv_fd(channel[1]);
  if (received < 0 || pidfd_shim_pid(received) != target) return 11;
  errno = 0;
  if (pidfd_shim_send_signal(received, SIGTERM) != -1 || errno != ENOTSUP) return 16;
  struct pollfd p = {.fd = received, .events = POLLIN | POLLHUP};
  if (poll(&p, 1, 0) != 0) return 12;
  if (write(stop[1], "X", 1) != 1) return 13;
  int status; if (waitpid(target, &status, 0) != target || !WIFEXITED(status)) return 14;
  if (poll(&p, 1, 1000) != 1 || !(p.revents & (POLLIN | POLLHUP))) return 15;
  close(received); close(transferable); close(local);
  close(channel[0]); close(channel[1]); close(ready[0]); close(stop[1]);
  /* The private directory may still contain the target's control socket. */
  DIR* entries = opendir(dir);
  if (entries) {
    struct dirent* entry;
    while ((entry = readdir(entries))) {
      if (entry->d_name[0] == '.') continue;
      char path[256]; snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
      unlink(path);
    }
    closedir(entries);
  }
  rmdir(dir);
  puts("HYBRID_PIDFD_TRANSFER_PASS");
  return 0;
}
