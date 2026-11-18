#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_seccomp
#define SYS_seccomp 277
#endif

int main(void) {
    errno = 0;
    if (syscall(SYS_seccomp, 0 /* SET_MODE_STRICT */, 0, 0) != -1 || errno != ENOSYS)
        return 1;
    errno = 0;
    if (syscall(SYS_seccomp, 1 /* SET_MODE_FILTER */, 0, 1) != -1 || errno != ENOSYS)
        return 2;
    if (prctl(PR_GET_SECCOMP, 0, 0, 0, 0) != 0) return 3;
    errno = 0;
    if (prctl(PR_SET_SECCOMP, 2, 1, 0, 0) != -1 || errno != EINVAL) return 4;
    puts("GUEST_SECCOMP_UNAVAILABLE_OK");
    return 0;
}
