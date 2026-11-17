#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv) {
  if (argc == 4 && !strcmp(argv[1], "verify-after-exec")) {
    int ref = atoi(argv[2]), listener = atoi(argv[3]);
    struct stat st; struct statx sx;
    if (fstat(ref, &st) || !S_ISSOCK(st.st_mode) ||
        statx(ref, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx) ||
        (sx.stx_mode & S_IFMT) != S_IFSOCK) return 10;
    char name[64]; snprintf(name, sizeof name, "/proc/self/fd/%d", ref);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", name);
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client < 0 || connect(client, (void*)&addr,
        offsetof(struct sockaddr_un, sun_path) + strlen(name) + 1)) {
      perror("connect through O_PATH after exec/unlink"); return 11;
    }
    int accepted = accept(listener, 0, 0);
    if (accepted < 0) { perror("accept after exec"); return 12; }
    close(accepted); close(client); close(ref); close(listener);
    puts("GUEST_OPATH_SOCKET_OK");
    return 0;
  }
  const char* path = "/tmp/shimmy-opath-guest-test.sock";
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
  unlink(path);
  int listener = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listener < 0 || bind(listener, (void*)&addr,
      offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1) || listen(listener, 1)) {
    perror("bind/listen"); return 1;
  }
  pid_t opener = fork();
  if (opener == 0) {
    int other = open(path, O_PATH);
    struct stat other_stat;
    _exit(other < 0 || fstat(other, &other_stat) || !S_ISSOCK(other_stat.st_mode));
  }
  int opener_status = 0;
  if (opener < 0 || waitpid(opener, &opener_status, 0) != opener ||
      !WIFEXITED(opener_status) || WEXITSTATUS(opener_status) != 0) {
    fprintf(stderr, "cross-process pathname O_PATH failed\n"); return 16;
  }
  int ref = open(path, O_PATH | O_CLOEXEC);
  if (ref < 0) { perror("open O_PATH"); return 2; }
  if ((fcntl(ref, F_GETFL) & O_PATH) != O_PATH) return 15;
  struct stat fd_stat;
  if (fstat(ref, &fd_stat) || !S_ISSOCK(fd_stat.st_mode)) {
    fprintf(stderr, "O_PATH fstat is not a socket\n"); return 3;
  }
  struct stat empty_stat;
  if (fstatat(ref, "", &empty_stat, AT_EMPTY_PATH) ||
      !S_ISSOCK(empty_stat.st_mode) || empty_stat.st_ino != fd_stat.st_ino) return 17;
  char token_byte;
  if (read(ref, &token_byte, 1) > 0) return 18;
  ino_t expected_ino = fd_stat.st_ino;
  struct statx sx;
  if (statx(ref, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx) ||
      (sx.stx_mode & S_IFMT) != S_IFSOCK || sx.stx_ino != expected_ino) {
    perror("O_PATH statx"); return 9;
  }
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair)) { perror("socketpair"); return 4; }
  char byte = 'x', control[CMSG_SPACE(sizeof(int))] = {0};
  struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
  struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = control, .msg_controllen = sizeof control };
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &ref, sizeof ref);
  if (sendmsg(pair[0], &msg, 0) != 1) { perror("sendmsg"); return 5; }
  memset(control, 0, sizeof control); msg.msg_controllen = sizeof control;
  if (recvmsg(pair[1], &msg, 0) != 1) { perror("recvmsg"); return 6; }
  cmsg = CMSG_FIRSTHDR(&msg);
  int received = -1;
  if (cmsg && cmsg->cmsg_type == SCM_RIGHTS)
    memcpy(&received, CMSG_DATA(cmsg), sizeof received);
  if (received < 0 || fstat(received, &fd_stat) || !S_ISSOCK(fd_stat.st_mode) ||
      fd_stat.st_ino != expected_ino) {
    fprintf(stderr, "received O_PATH reference lost socket identity\n"); return 7;
  }
  unlink(path);
  if (fstat(ref, &fd_stat) || fd_stat.st_ino != expected_ino) return 8;
  close(received); close(pair[0]); close(pair[1]);
  if (fcntl(ref, F_SETFD, 0)) return 13;
  char ref_number[20], listener_number[20];
  snprintf(ref_number, sizeof ref_number, "%d", ref);
  snprintf(listener_number, sizeof listener_number, "%d", listener);
  execl("/tmp/guest_opath_socket", "guest_opath_socket", "verify-after-exec",
        ref_number, listener_number, (char*)0);
  perror("exec"); return 14;
}
