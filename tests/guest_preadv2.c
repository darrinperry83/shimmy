#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef SYS_preadv2
#define SYS_preadv2 286
#endif

static long rd(int fd, struct iovec *iov, unsigned long off, unsigned long hi,
               unsigned long flags) {
    return syscall(SYS_preadv2, fd, iov, 1UL, off, hi, flags);
}

int main(void) {
    const char *path = "/tmp/preadv2-file";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "abcdef", 6) != 6 || lseek(fd, 1, SEEK_SET) != 1) return 1;
    char buf[3] = {0};
    struct iovec iov = {.iov_base = buf, .iov_len = 2};

    // A positioned read does not change the shared file offset. x4 is the
    // ignored high half on arm64; flags occupy x5.
    if (rd(fd, &iov, 3, 1, 0) != 2 || memcmp(buf, "de", 2) != 0 ||
        lseek(fd, 0, SEEK_CUR) != 1) return 2;
    memset(buf, 0, sizeof buf);
    if (rd(fd, &iov, (unsigned long)-1L, 0, 0) != 2 ||
        memcmp(buf, "bc", 2) != 0 || lseek(fd, 0, SEEK_CUR) != 3) return 3;

    // RWF_NOWAIT must not turn into a successful blocking read or consume data.
    memset(buf, 0, sizeof buf);
    if (rd(fd, &iov, (unsigned long)-1L, 0, 8) != -1 ||
        errno != EOPNOTSUPP || lseek(fd, 0, SEEK_CUR) != 3) return 4;
    if (rd(fd, &iov, (unsigned long)-2L, 0, 0) != -1 || errno != EINVAL) return 5;
    if (rd(fd, &iov, (unsigned long)-1L, 0, 0) != 2 ||
        memcmp(buf, "de", 2) != 0 || lseek(fd, 0, SEEK_CUR) != 5) return 6;
    close(fd);
    puts("GUEST_PREADV2_OK");
    return 0;
}
