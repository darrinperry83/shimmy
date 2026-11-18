#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
  errno = 0;
  if (syscall(81) != -1 || errno != ENOSYS) return 1;
  puts("GUEST_SYNC_REFUSAL_OK");
  return 0;
}
