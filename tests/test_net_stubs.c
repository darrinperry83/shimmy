// Standalone net.c test hooks. The full Shimmy binary supplies these symbols.
#include <unistd.h>

int g_uid0 = 0;
unsigned guest_cred_uid(int which) { (void)which; return getuid(); }
unsigned guest_cred_gid(int which) { (void)which; return getgid(); }
int shim_fd_reserve(int fd) { return fd; }
void shim_fd_unreserve(int fd) { (void)fd; }
const char* syscalls_get_root(void) { return ""; }
long syscalls_guest_pid(void) { return getpid(); }
