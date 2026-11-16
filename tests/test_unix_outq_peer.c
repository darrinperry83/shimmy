#include "../src/unix_outq_peer.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  int stream[2], passed[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, stream) == 0);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, passed) == 0);
  int bytes = -1;
  assert(rd_unix_peer_outq(stream[0], &bytes) == 0 && bytes == 0);

  char c = 'F', control[CMSG_SPACE(sizeof(int))] = {0};
  struct iovec iov = {&c, 1};
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                      .msg_control = control, .msg_controllen = sizeof control};
  struct cmsghdr* hdr = CMSG_FIRSTHDR(&msg);
  hdr->cmsg_len = CMSG_LEN(sizeof(int));
  hdr->cmsg_level = SOL_SOCKET;
  hdr->cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(hdr), &passed[0], sizeof(int));
  assert(sendmsg(stream[0], &msg, 0) == 1);
  assert(rd_unix_peer_outq(stream[0], &bytes) == 0 && bytes > 128);
  int one_charge = bytes;
  assert(sendmsg(stream[0], &msg, 0) == 1);
  assert(rd_unix_peer_outq(stream[0], &bytes) == 0 && bytes > one_charge);
  printf("two_queued_fd_bytes=%d\n", bytes);

  char received_control[CMSG_SPACE(sizeof(int))] = {0};
  struct msghdr received = {.msg_iov = &iov, .msg_iovlen = 1,
                            .msg_control = received_control,
                            .msg_controllen = sizeof received_control};
  assert(recvmsg(stream[1], &received, 0) == 1);
  assert(rd_unix_peer_outq(stream[0], &bytes) == 0 && bytes >= one_charge);
  int received_fd;
  memcpy(&received_fd, CMSG_DATA(CMSG_FIRSTHDR(&received)), sizeof received_fd);
  close(received_fd);
  received.msg_controllen = sizeof received_control;
  memset(received_control, 0, sizeof received_control);
  assert(recvmsg(stream[1], &received, 0) == 1);
  assert(rd_unix_peer_outq(stream[0], &bytes) == 0 && bytes == 0);
  memcpy(&received_fd, CMSG_DATA(CMSG_FIRSTHDR(&received)), sizeof received_fd);
  close(received_fd);

  errno = 0;
  assert(rd_unix_peer_outq(-1, &bytes) < 0);
  printf("invalid_fd_errno=%d\n", errno);
  assert(errno == EBADF);
  errno = 0;
  assert(rd_unix_peer_outq(stream[0], NULL) < 0);
  assert(errno == EFAULT);
  close(stream[0]); close(stream[1]); close(passed[0]); close(passed[1]);
  puts("unix_outq_peer PASS");
  return 0;
}
