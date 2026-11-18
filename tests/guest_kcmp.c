#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/kcmp.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_kcmp
#define SYS_kcmp 272
#endif

static long compare(pid_t p1, pid_t p2, int type, unsigned long i1,
                    unsigned long i2) {
    return syscall(SYS_kcmp, p1, p2, type, i1, i2);
}

int main(void) {
    pid_t me = getpid();
    int a = open("/tmp/kcmp-a", O_CREAT | O_RDWR | O_TRUNC, 0600);
    int b = open("/tmp/kcmp-a", O_RDONLY);
    if (a < 0 || b < 0) return 1;
    if (compare(me, me, KCMP_FILE, a, a) != 0) return 2;
    errno = 0;
    if (compare(me, me, KCMP_FILE, a, b) != -1 || errno != EOPNOTSUPP)
        return 3; // identical vnode does not imply shared open description
    errno = 0;
    if (compare(me, me, KCMP_FILE, a, 123456) != -1 || errno != EBADF)
        return 4;
    if (compare(me, me, KCMP_VM, 99, 123) != 0) return 5;
    if (compare(me, me, KCMP_FILES, 99, 123) != 0) return 6;
    if (compare(me, me, KCMP_FS, 99, 123) != 0) return 7;
    if (compare(me, me, KCMP_SIGHAND, 99, 123) != 0) return 8;
    if (compare(me, me, KCMP_IO, 99, 123) != 0) return 9;
    if (compare(me, me, KCMP_SYSVSEM, 99, 123) != 0) return 10;
    errno = 0;
    if (compare(me, me, KCMP_EPOLL_TFD, a, 0) != -1 || errno != EOPNOTSUPP)
        return 11;
    errno = 0;
    if (compare(me, me, 999, 0, 0) != -1 || errno != EINVAL) return 12;
    errno = 0;
    if (compare(me, me + 999999, KCMP_VM, 0, 0) != -1 ||
        errno != EOPNOTSUPP) return 13;
    close(a);
    close(b);
    puts("GUEST_KCMP_SELF_OK");
    return 0;
}
