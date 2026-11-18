#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif

int main(void) {
    int fd = open("/tmp/pidfd-getfd-file", O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "abc", 3) != 3 || lseek(fd, 1, SEEK_SET) != 1)
        return 1;
    int pfd = (int)syscall(SYS_pidfd_open, getpid(), 0);
    if (pfd < 0) { perror("pidfd_open"); return 2; }
    int copy = (int)syscall(SYS_pidfd_getfd, pfd, fd, 0);
    if (copy < 0 || copy == fd || !(fcntl(copy, F_GETFD) & FD_CLOEXEC)) {
        perror("pidfd_getfd"); return 3;
    }
    char c = 0;
    if (read(copy, &c, 1) != 1 || c != 'b' || lseek(fd, 0, SEEK_CUR) != 2)
        return 4; // same open file description, not a separate open of the path
    errno = 0;
    if (syscall(SYS_pidfd_getfd, pfd, fd, 1) != -1 || errno != EINVAL)
        return 5;
    errno = 0;
    if (syscall(SYS_pidfd_getfd, pfd, -1, 0) != -1 || errno != EBADF)
        return 6;
    errno = 0;
    if (syscall(SYS_pidfd_getfd, fd, fd, 0) != -1 || errno != EBADF)
        return 7;
    errno = 0;
    if (syscall(SYS_pidfd_getfd, pfd, pfd, 0) != -1 || errno != EOPNOTSUPP)
        return 8;
    close(fd);
    if (read(copy, &c, 1) != 1 || c != 'c') return 9;
    close(copy);
    close(pfd);
    puts("GUEST_PIDFD_GETFD_SELF_OK");
    return 0;
}
