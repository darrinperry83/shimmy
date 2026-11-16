#include "../src/unix_outq_peer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
  char path[sizeof(((struct sockaddr_un*)0)->sun_path)];
  snprintf(path, sizeof path, "/tmp/rd-outq-%d", getpid());
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strcpy(addr.sun_path, path);
  int listener = socket(AF_UNIX, SOCK_STREAM, 0);
  assert(listener >= 0);
  unlink(path);
  assert(bind(listener, (struct sockaddr*)&addr, sizeof addr) == 0);
  assert(listen(listener, 1) == 0);

  int start[2], done[2];
  assert(pipe(start) == 0 && pipe(done) == 0);
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    close(start[1]); close(done[0]); close(listener);
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(client >= 0);
    assert(connect(client, (struct sockaddr*)&addr, sizeof addr) == 0);
    char byte;
    assert(read(start[0], &byte, 1) == 1);
    struct iovec iov = {&byte, 1};
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = control, .msg_controllen = sizeof control};
    assert(recvmsg(client, &msg, 0) == 1);
    int received_fd;
    memcpy(&received_fd, CMSG_DATA(CMSG_FIRSTHDR(&msg)), sizeof received_fd);
    close(received_fd);
    assert(write(done[1], "D", 1) == 1);
    assert(read(start[0], &byte, 1) == 1);
    close(client);
    _exit(0);
  }
  close(start[0]); close(done[1]);
  int server = accept(listener, NULL, NULL);
  assert(server >= 0);
  int other[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, other) == 0);

  int bytes = -1;
  assert(rd_unix_peer_outq(server, &bytes) == 0 && bytes == 0);
  char byte = 'F', control[CMSG_SPACE(sizeof(int))] = {0};
  struct iovec iov = {&byte, 1};
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                       .msg_control = control, .msg_controllen = sizeof control};
  struct cmsghdr* hdr = CMSG_FIRSTHDR(&msg);
  hdr->cmsg_len = CMSG_LEN(sizeof(int));
  hdr->cmsg_level = SOL_SOCKET;
  hdr->cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(hdr), &other[0], sizeof(int));
  assert(sendmsg(server, &msg, 0) == 1);
  assert(rd_unix_peer_outq(server, &bytes) == 0 && bytes > 128);
  printf("cross_process_queued_fd_bytes=%d\n", bytes);
  assert(write(start[1], "R", 1) == 1);
  assert(read(done[0], &byte, 1) == 1);
  assert(rd_unix_peer_outq(server, &bytes) == 0 && bytes == 0);
  assert(write(start[1], "X", 1) == 1);
  int status = -1;
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  // The endpoint no longer has an inspectable peer. Never claim its queue is empty.
  assert(rd_unix_peer_outq(server, &bytes) < 0);

  close(server); close(listener); close(other[0]); close(other[1]);
  close(start[1]); close(done[0]);
  unlink(path);
  puts("unix_outq_crossproc PASS");
  return 0;
}
