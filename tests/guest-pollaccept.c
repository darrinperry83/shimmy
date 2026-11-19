/* Cross-process AF_UNIX listener wake probe for the shim's poll/pselect path.
 *
 * Reproduces the gpg-agent hang: a bound+listening AF_UNIX socket, connect()ed
 * from a DIFFERENT process, whose pending connection macOS poll()/pselect()
 * never reports. The server must block in ppoll (syscall 73) / pselect6
 * (syscall 72) on the listener and wake to accept() the client, without timing
 * out. Emits POLLACCEPT_GUEST_OK on success.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static long raw_syscall6(long nr, long a, long b, long c, long d, long e, long f) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = e;
  register long x5 __asm__("x5") = f;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                   "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}

static int make_listener(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
  unlink(path);
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) { close(fd); return -1; }
  if (listen(fd, 8) != 0) { close(fd); unlink(path); return -1; }
  return fd;
}

/* Fork a client that connects to `path` after `delay_ms`, holds the connection
 * briefly, then exits. Returns the child pid, or -1 on fork failure. */
static pid_t fork_client(const char *path, unsigned delay_ms) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    if (delay_ms) usleep(delay_ms * 1000);
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c < 0) _exit(10);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
    if (connect(c, (struct sockaddr *)&addr, sizeof addr) != 0) _exit(11);
    usleep(300000);
    close(c);
    _exit(0);
  }
  return pid;
}

static int test_ppoll(const char *path) {
  int listener = make_listener(path);
  if (listener < 0) { printf("pollaccept: ppoll make_listener failed\n"); return 1; }
  pid_t client = fork_client(path, 100);
  if (client < 0) { close(listener); unlink(path); return 2; }

  struct pollfd pfd = { listener, POLLIN, 0 };
  struct timespec ts = { 5, 0 };
  long n = raw_syscall6(73, (long)&pfd, 1, (long)&ts, 0, 0, 0);  /* ppoll */
  if (n != 1 || !(pfd.revents & POLLIN)) {
    printf("pollaccept: ppoll n=%ld revents=%#x errno=%d\n", n, pfd.revents, errno);
    waitpid(client, NULL, 0);
    close(listener); unlink(path);
    return 3;
  }
  int accepted = accept(listener, NULL, NULL);
  if (accepted < 0) {
    printf("pollaccept: ppoll accept failed errno=%d\n", errno);
    waitpid(client, NULL, 0);
    close(listener); unlink(path);
    return 4;
  }
  close(accepted);
  waitpid(client, NULL, 0);
  close(listener);
  unlink(path);
  return 0;
}

static int test_pselect6(const char *path) {
  int listener = make_listener(path);
  if (listener < 0) { printf("pollaccept: pselect6 make_listener failed\n"); return 5; }
  pid_t client = fork_client(path, 100);
  if (client < 0) { close(listener); unlink(path); return 6; }

  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(listener, &rfds);
  struct timespec ts = { 5, 0 };
  long n = raw_syscall6(72, listener + 1, (long)&rfds, 0, 0, (long)&ts, 0);  /* pselect6 */
  if (n != 1 || !FD_ISSET(listener, &rfds)) {
    printf("pollaccept: pselect6 n=%ld isset=%d errno=%d\n",
           n, FD_ISSET(listener, &rfds), errno);
    waitpid(client, NULL, 0);
    close(listener); unlink(path);
    return 7;
  }
  int accepted = accept(listener, NULL, NULL);
  if (accepted < 0) {
    printf("pollaccept: pselect6 accept failed errno=%d\n", errno);
    waitpid(client, NULL, 0);
    close(listener); unlink(path);
    return 8;
  }
  close(accepted);
  waitpid(client, NULL, 0);
  close(listener);
  unlink(path);
  return 0;
}

int main(void) {
  char path[128];
  snprintf(path, sizeof path, "/tmp/pollaccept-ppoll-%d", (int)getpid());
  if (test_ppoll(path) != 0) return 1;
  snprintf(path, sizeof path, "/tmp/pollaccept-pselect-%d", (int)getpid());
  if (test_pselect6(path) != 0) return 1;
  puts("POLLACCEPT_GUEST_OK");
  return 0;
}
