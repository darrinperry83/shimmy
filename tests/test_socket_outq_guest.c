#include <errno.h>
#include <linux/sockios.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) return 10;
  int count = -1;
  errno = 0;
  int result = ioctl(pair[0], SIOCOUTQ, &count);
  printf("EMPTY result=%d errno=%d count=%d\n", result, errno, count);
  if (result < 0) return errno == EOPNOTSUPP ? 95 : 11;
  if (count != 0) return 12;

  char bytes[8192] = {0};
  if (send(pair[0], bytes, sizeof bytes, 0) != sizeof bytes) return 13;
  count = -1;
  errno = 0;
  result = ioctl(pair[0], SIOCOUTQ, &count);
  printf("SENT result=%d errno=%d count=%d\n", result, errno, count);
  if (result < 0 || count < 0) return 14;

  if (recv(pair[1], bytes, sizeof bytes, 0) != sizeof bytes) return 17;
  int passed[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, passed) < 0) return 18;
  char byte = 'F', control[CMSG_SPACE(sizeof(int))] = {0};
  struct iovec iov = {&byte, 1};
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                       .msg_control = control, .msg_controllen = sizeof control};
  struct cmsghdr* hdr = CMSG_FIRSTHDR(&msg);
  hdr->cmsg_len = CMSG_LEN(sizeof(int));
  hdr->cmsg_level = SOL_SOCKET;
  hdr->cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(hdr), &passed[0], sizeof(int));
  if (sendmsg(pair[0], &msg, 0) != 1) return 19;
  if (ioctl(pair[0], SIOCOUTQ, &count) < 0 || count <= 128) return 20;
  printf("FD_PENDING count=%d\n", count);
  char received_control[CMSG_SPACE(sizeof(int))] = {0};
  struct msghdr received = {.msg_iov = &iov, .msg_iovlen = 1,
                            .msg_control = received_control,
                            .msg_controllen = sizeof received_control};
  if (recvmsg(pair[1], &received, 0) != 1) return 21;
  int received_fd;
  memcpy(&received_fd, CMSG_DATA(CMSG_FIRSTHDR(&received)), sizeof received_fd);
  close(received_fd);
  if (ioctl(pair[0], SIOCOUTQ, &count) < 0 || count != 0) return 22;
  printf("FD_CONSUMED count=%d\n", count);
  close(passed[0]); close(passed[1]);

  errno = 0;
  result = ioctl(-1, SIOCOUTQ, &count);
  if (result != -1 || errno != EBADF) return 15;
  errno = 0;
  result = ioctl(pair[0], SIOCOUTQ, NULL);
  if (result != -1 || errno != EFAULT) return 16;
  close(pair[0]);
  close(pair[1]);
  return 0;
}
