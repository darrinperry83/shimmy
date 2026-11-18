// Standalone net.c test hooks. The full Shimmy binary supplies these symbols.
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>

int g_uid0 = 0;
unsigned guest_cred_uid(int which) { (void)which; return getuid(); }
unsigned guest_cred_gid(int which) { (void)which; return getgid(); }
int shim_fd_reserve(int fd) { return fd; }
void shim_fd_unreserve(int fd) { (void)fd; }
const char* syscalls_get_root(void) { return ""; }
long syscalls_guest_pid(void) { return getpid(); }
int linux_errno_from_host(int error) { return error; }
// The standalone net harness passes only its own valid host buffers. Guest
// address validation is exercised separately by ARM64 guest probes.
int gmem_guest_export_copyin(void *unused, uint64_t addr, void *out, size_t n) {
  (void)unused;
  if (!addr) return -1;
  memcpy(out, (const void *)(uintptr_t)addr, n);
  return 0;
}
int gmem_guest_copyout_checked(uint64_t addr, const void *in, size_t n) {
  if (!addr) return -1;
  memcpy((void *)(uintptr_t)addr, in, n);
  return 0;
}
void drm_register_received_fd(int fd) { (void)fd; }
int guest_ownership_set(const struct stat *st, uint32_t uid, uint32_t gid) {
  (void)st; (void)uid; (void)gid; return 0;
}
