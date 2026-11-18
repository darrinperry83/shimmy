#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/xattr.h>
#include <unistd.h>

#define LX_AT_EMPTY_PATH 0x1000
#define LX_AT_SYMLINK_NOFOLLOW 0x100

static long lx_listxattrat(int dirfd, const char *path, unsigned long flags,
                           char *list, unsigned long size) {
    register long x0 __asm__("x0") = dirfd;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = flags;
    register long x3 __asm__("x3") = (long)list;
    register long x4 __asm__("x4") = size;
    register long x8 __asm__("x8") = 465;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                     "r"(x4), "r"(x8) : "memory");
    return x0;
}

static int contains(const char *list, long len, const char *name) {
    for (long pos = 0; pos < len;) {
        long n = (long)strlen(list + pos) + 1;
        if (n > len - pos) return 0;
        if (!strcmp(list + pos, name)) return 1;
        pos += n;
    }
    return 0;
}

int main(void) {
    const char *name = "user.shimmy-listxattrat";
    int fd = open("/tmp/listxattrat-file", O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) return 1;
    /* Populate via the existing translated setxattr syscall. */
    if (setxattr("/tmp/listxattrat-file", name, "v", 1, 0) < 0) return 2;
    long needed = lx_listxattrat(AT_FDCWD, "/tmp/listxattrat-file", 0, 0, 0);
    if (needed <= 0 || needed > 256) return 3;
    char list[256] = {0};
    if (lx_listxattrat(AT_FDCWD, "/tmp/listxattrat-file", 0, list, needed) != needed ||
        !contains(list, needed, name)) return 4;
    if (lx_listxattrat(AT_FDCWD, "/tmp/listxattrat-file", 0, list, 1) != -34)
        return 5;  /* ERANGE */
    if (lx_listxattrat(fd, "", LX_AT_EMPTY_PATH, list, sizeof(list)) != needed ||
        !contains(list, needed, name)) return 6;
    if (lx_listxattrat(fd, NULL, LX_AT_EMPTY_PATH, list, sizeof(list)) != needed)
        return 7;
    if (lx_listxattrat(-1, "", LX_AT_EMPTY_PATH, list, sizeof(list)) != -9)
        return 8;
    if (lx_listxattrat(AT_FDCWD, "/tmp/listxattrat-file", 0x200, list,
                       sizeof(list)) != -22) return 9;
    int dirfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) return 10;
    if (lx_listxattrat(dirfd, "listxattrat-file", 0, list, sizeof(list)) != needed)
        return 11;
    if (symlink("listxattrat-file", "/tmp/listxattrat-link") < 0) return 12;
    if (lx_listxattrat(AT_FDCWD, "/tmp/listxattrat-link", 0, list,
                       sizeof(list)) != needed) return 13;
    if (lx_listxattrat(AT_FDCWD, "/tmp/listxattrat-link",
                       LX_AT_SYMLINK_NOFOLLOW, list, sizeof(list)) != 0)
        return 14;
    puts("GUEST_LISTXATTRAT_OK");
    close(dirfd);
    close(fd);
    return 0;
}
