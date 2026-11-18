#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/xattr.h>
#include <unistd.h>

struct xattr_args_guest {
    uint64_t value;
    uint32_t size;
    uint32_t flags;
};

static long raw_getxattrat(int dfd, const char *path, unsigned int flags,
                           const char *name, void *args, size_t usize) {
    register long x0 __asm__("x0") = dfd;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = flags;
    register long x3 __asm__("x3") = (long)name;
    register long x4 __asm__("x4") = (long)args;
    register long x5 __asm__("x5") = usize;
    register long x8 __asm__("x8") = 464;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                     "r"(x4), "r"(x5), "r"(x8) : "memory");
    return x0;
}

int main(void) {
    const char *path = "/tmp/getxattrat-file";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) return 1;
    close(fd);
    if (setxattr(path, "user.shimmy", "hello", 5, 0) != 0) return 2;

    char got[16] = {0};
    struct xattr_args_guest a = {(uint64_t)(uintptr_t)got, sizeof got, 0};
    if (raw_getxattrat(AT_FDCWD, path, 0, "user.shimmy", &a, sizeof a) != 5 ||
        memcmp(got, "hello", 5) != 0) return 3;
    int dirfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (dirfd < 0 || raw_getxattrat(dirfd, "getxattrat-file", 0,
                                   "user.shimmy", &a, sizeof a) != 5) return 14;
    close(dirfd);

    a.value = 0; a.size = 0;
    if (raw_getxattrat(AT_FDCWD, path, 0, "user.shimmy", &a, sizeof a) != 5)
        return 4;
    a.value = (uint64_t)(uintptr_t)got; a.size = 3;
    if (raw_getxattrat(AT_FDCWD, path, 0, "user.shimmy", &a, sizeof a) != -34)
        return 5; /* ERANGE */

    a.size = sizeof got; a.flags = 1;
    if (raw_getxattrat(AT_FDCWD, path, 0, "user.shimmy", &a, sizeof a) != -22)
        return 6;
    a.flags = 0;
    if (raw_getxattrat(AT_FDCWD, path, 0x80000000u, "user.shimmy", &a, sizeof a) != -22)
        return 7;
    if (raw_getxattrat(AT_FDCWD, path, 0, "user.shimmy", &a, sizeof a - 1) != -22)
        return 8;
    if (raw_getxattrat(AT_FDCWD, path, 0, "user.shimmy", 0, sizeof a) != -14)
        return 9;
    if (raw_getxattrat(AT_FDCWD, "", 0x1000, "user.shimmy", &a, sizeof a) != -95)
        return 10; /* fd-relative empty path is deliberately unsupported */

    struct { struct xattr_args_guest args; uint64_t extra; } extended = {a, 0};
    if (raw_getxattrat(AT_FDCWD, path, 0, "user.shimmy", &extended,
                       sizeof extended) != 5) return 11;
    extended.extra = 1;
    if (raw_getxattrat(AT_FDCWD, path, 0, "user.shimmy", &extended,
                       sizeof extended) != -7) return 12; /* E2BIG */
    if (raw_getxattrat(AT_FDCWD, path, 0, "user.missing", &a,
                       sizeof a) != -61) return 13; /* ENODATA */
    if (raw_getxattrat(AT_FDCWD, path, 0, "user.shimmy", &a,
                       sizeof a + 16384) != -7) return 15;
    if (raw_getxattrat(AT_FDCWD, path, 0, (void *)1, &a,
                       sizeof a) != -14) return 16;
    if (raw_getxattrat(AT_FDCWD, (void *)1, 0, "user.shimmy", &a,
                       sizeof a) != -14) return 17;
    a.value = 1; a.size = sizeof got;
    if (raw_getxattrat(AT_FDCWD, path, 0, "user.shimmy", &a,
                       sizeof a) != -14) return 18;
    puts("GUEST_GETXATTRAT_OK");
    return 0;
}
