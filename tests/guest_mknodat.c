#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_mknodat
#define SYS_mknodat 33
#endif

static int absent(int dirfd, const char *name) {
    struct stat st;
    return fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT;
}

int main(void) {
    const char *dir = "/tmp/mknodat-dir";
    if (mkdir(dir, 0700) != 0) return 1;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) return 2;
    struct stat st;

    if (syscall(SYS_mknodat, dfd, "regular", S_IFREG | 0640, 123) != 0 ||
        fstatat(dfd, "regular", &st, 0) != 0 || !S_ISREG(st.st_mode)) return 3;
    if (syscall(SYS_mknodat, dfd, "fifo", S_IFIFO | 0600, 123) != 0 ||
        fstatat(dfd, "fifo", &st, 0) != 0 || !S_ISFIFO(st.st_mode)) return 4;

    if (syscall(SYS_mknodat, dfd, "directory", S_IFDIR | 0700, 0) != -1 ||
        errno != EPERM || !absent(dfd, "directory")) return 5;
    if (syscall(SYS_mknodat, dfd, "symlink", S_IFLNK | 0700, 0) != -1 ||
        errno != EINVAL || !absent(dfd, "symlink")) return 6;
    if (syscall(SYS_mknodat, dfd, "device", S_IFCHR | 0600, 0) != -1 ||
        errno != EPERM || !absent(dfd, "device")) return 7;
    if (symlink("missing", "/tmp/mknodat-dir/existing-link") != 0) return 8;
    if (syscall(SYS_mknodat, dfd, "existing-link", S_IFREG | 0600, 0) != -1 ||
        errno != EEXIST) return 9;
    close(dfd);
    puts("GUEST_MKNODAT_OK");
    return 0;
}
