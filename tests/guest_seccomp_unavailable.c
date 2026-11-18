#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_seccomp
#define SYS_seccomp 277
#endif

/* Linux UAPI layouts the test needs to hand a valid program to the shim. */
struct sock_filter { uint16_t code; uint8_t jt, jf; uint32_t k; };
struct sock_fprog  { unsigned short len; struct sock_filter *filter; };

int main(void) {
    /* SET_MODE_STRICT is not supported: the shim only installs BPF filters. */
    errno = 0;
    if (syscall(SYS_seccomp, 0 /* SET_MODE_STRICT */, 0, 0) != -1 || errno != EINVAL)
        return 1;

    /* A valid filter is still refused before no_new_privs is set. */
    struct sock_filter insns[1] = { { 0x06, 0, 0, 0x7fff0000u } /* RET ALLOW */ };
    struct sock_fprog fp = { 1, insns };
    errno = 0;
    if (syscall(SYS_seccomp, 1 /* SET_MODE_FILTER */, 0, &fp) != -1 || errno != EACCES)
        return 2;

    if (prctl(PR_GET_SECCOMP, 0, 0, 0, 0) != 0) return 3;

    /* PR_SET_SECCOMP only accepts SECCOMP_MODE_FILTER (2). */
    errno = 0;
    if (prctl(PR_SET_SECCOMP, 0 /* STRICT */, &fp, 0, 0) != -1 || errno != EINVAL)
        return 4;

    puts("GUEST_SECCOMP_UNAVAILABLE_OK");
    return 0;
}
