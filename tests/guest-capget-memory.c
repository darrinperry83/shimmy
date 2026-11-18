#define _GNU_SOURCE
#include <errno.h>
#include <linux/capability.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
  struct __user_cap_header_struct header = {.version=0, .pid=0};
  struct { struct __user_cap_data_struct data[2]; uint64_t canary; } out;
  memset(&out, 0xa5, sizeof out);
  out.canary = 0x4e5c72f0b38a196dULL;
  errno = 0;
  if (syscall(SYS_capget, &header, out.data) != -1 || errno != EINVAL ||
      header.version != _LINUX_CAPABILITY_VERSION_3 ||
      out.canary != 0x4e5c72f0b38a196dULL ||
      out.data[0].effective != 0xa5a5a5a5U) return 1;

  if (syscall(SYS_capget, &header, out.data) != 0 ||
      out.canary != 0x4e5c72f0b38a196dULL) return 2;
  for (unsigned i = 0; i < sizeof out.data; ++i)
    if (((unsigned char *)out.data)[i] != 0) return 3;
  errno = 0;
  if (syscall(SYS_capget, &header, (void *)1) != -1 || errno != EFAULT)
    return 4;
  errno = 0;
  if (syscall(SYS_capget, (void *)1, out.data) != -1 || errno != EFAULT)
    return 5;
  errno = 0;
  if (syscall(SYS_capget, NULL, out.data) != -1 || errno != EFAULT)
    return 6;

  header.version = _LINUX_CAPABILITY_VERSION_1;
  memset(&out, 0xa5, sizeof out);
  out.canary = 0x4e5c72f0b38a196dULL;
  if (syscall(SYS_capget, &header, out.data) != 0 ||
      out.data[0].effective || out.data[0].permitted ||
      out.data[0].inheritable ||
      out.data[1].effective != 0xa5a5a5a5U ||
      out.canary != 0x4e5c72f0b38a196dULL) return 7;
  puts("GUEST_CAPGET_MEMORY_OK");
  return 0;
}
