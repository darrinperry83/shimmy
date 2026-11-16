// Isolated cross-process AF_UNIX listener wake probe for Shimmy's epoll bridge.
// Run through test_unix_listener_epoll_wake.py for a bounded timeout.
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "net.h"

const char* syscalls_get_root(void) { return "/"; }
long syscalls_guest_pid(void) { return (long)getpid(); }
int shim_fd_reserve(int fd) { return fd; }
void shim_fd_unreserve(int fd) { (void)fd; }
int g_uid0;
unsigned guest_cred_uid(int which) { (void)which; return (unsigned)getuid(); }
unsigned guest_cred_gid(int which) { (void)which; return (unsigned)getgid(); }

struct linux_epoll_event { uint32_t events, pad; uint64_t data; };

int main(int argc, char** argv) {
  if (argc != 3 || (strcmp(argv[1], "native") && strcmp(argv[1], "shim")) ||
      (strcmp(argv[2], "queued") && strcmp(argv[2], "delayed"))) {
    fprintf(stderr, "usage: %s native|shim queued|delayed\n", argv[0]);
    return 2;
  }
  int use_shim = !strcmp(argv[1], "shim");
  int queued = !strcmp(argv[2], "queued");
  char dir[] = "/tmp/shimmy-unix-wake.XXXXXX";
  assert(mkdtemp(dir));
  char path[sizeof(((struct sockaddr_un*)0)->sun_path)];
  assert(snprintf(path, sizeof path, "%s/socket", dir) < (int)sizeof path);
  int listener = socket(AF_UNIX, SOCK_STREAM, 0);
  assert(listener >= 0);
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  strcpy(addr.sun_path, path);
  if (bind(listener, (struct sockaddr*)&addr, sizeof addr) != 0) {
    perror("bind isolated Unix listener");
    close(listener);
    rmdir(dir);
    return 3;
  }
  assert(listen(listener, 4) == 0);

  int event_fd;
  if (use_shim) {
    long ep = net_syscall(20, 0, 0, 0, 0, 0, 0);
    assert(ep >= 0);
    event_fd = (int)ep;
    struct linux_epoll_event interest = { .events = 1, .data = 0x12345678 };
    assert(net_syscall(21, (unsigned long)event_fd, 1, (unsigned long)listener,
                       (unsigned long)&interest, 0, 0) == 0);
  } else {
    event_fd = kqueue();
    assert(event_fd >= 0);
    struct kevent ev;
    EV_SET(&ev, listener, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    assert(kevent(event_fd, &ev, 1, NULL, 0, NULL) == 0);
  }

  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    if (!queued) usleep(200000);
    int peer = socket(AF_UNIX, SOCK_STREAM, 0);
    if (peer < 0 || connect(peer, (struct sockaddr*)&addr, sizeof addr) != 0 ||
        write(peer, "H", 1) != 1) _exit(4);
    usleep(500000);
    close(peer);
    _exit(0);
  }
  if (queued) usleep(200000); // connection queued before the infinite wait begins

  fprintf(stderr, "%s %s: waiting on listener fd=%d epfd=%d\n", argv[1], argv[2], listener, event_fd);
  fflush(stderr);
  int n;
  if (use_shim) {
    struct linux_epoll_event out = {0};
    n = (int)net_syscall(22, (unsigned long)event_fd, (unsigned long)&out,
                         1, (unsigned long)-1, 0, 0);
    assert(n == 1 && (out.events & 1) && out.data == 0x12345678);
  } else {
    struct kevent out;
    n = kevent(event_fd, NULL, 0, &out, 1, NULL);
    assert(n == 1 && out.filter == EVFILT_READ && (int)out.ident == listener);
  }
  int accepted = accept(listener, NULL, NULL);
  assert(accepted >= 0);
  char byte = 0;
  assert(read(accepted, &byte, 1) == 1 && byte == 'H');
  int status = 0;
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  printf("%s %s: listener woke, accepted cross-process connection\n", argv[1], argv[2]);
  close(accepted); close(listener); close(event_fd);
  assert(unlink(path) == 0 && rmdir(dir) == 0);
  return 0;
}
