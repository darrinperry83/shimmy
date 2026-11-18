#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/xattr.h>
#include <unistd.h>

static long raw_removexattrat(int dfd, const char *path, unsigned int flags,
                              const char *name) {
    register long x0 __asm__("x0") = dfd;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = flags;
    register long x3 __asm__("x3") = (long)name;
    register long x8 __asm__("x8") = 466;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                     "r"(x8) : "memory");
    return x0;
}

int main(void) {
    const char *path = "/tmp/removexattrat-file";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) return 1;
    close(fd);
    if (setxattr(path, "user.shimmy", "v", 1, 0) != 0) return 2;

    if (raw_removexattrat(AT_FDCWD, path, 0x80000000u,
                          "user.shimmy") != -22) return 3;
    if (getxattr(path, "user.shimmy", 0, 0) != 1) return 4;
    if (raw_removexattrat(AT_FDCWD, path, 0, "user.shimmy") != 0) return 5;
    if (getxattr(path, "user.shimmy", 0, 0) >= 0) return 6;
    if (raw_removexattrat(AT_FDCWD, path, 0, "user.shimmy") != -61)
        return 7; /* ENODATA */

    if (setxattr(path, "user.shimmy", "v", 1, 0) != 0) return 8;
    int dirfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) return 9;
    if (raw_removexattrat(dirfd, "removexattrat-file", 0,
                          "user.shimmy") != 0) return 10;
    close(dirfd);
    if (getxattr(path, "user.shimmy", 0, 0) >= 0) return 11;

    if (raw_removexattrat(AT_FDCWD, "", 0, "user.shimmy") != -2)
        return 12; /* ENOENT */
    if (raw_removexattrat(AT_FDCWD, "", 0x1000,
                          "user.shimmy") != -95) return 13; /* EOPNOTSUPP */
    if (raw_removexattrat(AT_FDCWD, 0, 0, "user.shimmy") != -14)
        return 14; /* EFAULT */
    if (raw_removexattrat(AT_FDCWD, path, 0, 0) != -14) return 15;
    if (raw_removexattrat(AT_FDCWD, (void *)1, 0,
                          "user.shimmy") != -14) return 16;
    if (raw_removexattrat(AT_FDCWD, path, 0, (void *)1) != -14)
        return 17;
    puts("GUEST_REMOVEXATTRAT_OK");
    return 0;
}
