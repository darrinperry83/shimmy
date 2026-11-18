#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define LX_EXECVEAT 281
#define LX_AT_SYMLINK_NOFOLLOW 0x100
#define LX_AT_EMPTY_PATH 0x1000

int main(int argc, char **argv) {
    if (argc != 2) return 90;
    char *args[] = {"echo", "EXECVEAT_OK", NULL};
    char *env[] = {"PATH=/usr/bin", NULL};
    int fd = -100;
    const char *path = NULL;
    unsigned flags = 0;
    int expected = 0;
    if (!strcmp(argv[1], "relative")) {
        fd = open("/usr/bin", O_RDONLY | O_DIRECTORY);
        path = "echo";
    } else if (!strcmp(argv[1], "empty")) {
        fd = open("/usr/bin/echo", O_PATH);
        path = "";
        flags = LX_AT_EMPTY_PATH;
    } else if (!strcmp(argv[1], "nofollow")) {
        path = "/tmp/execveat-echo-link";
        flags = LX_AT_SYMLINK_NOFOLLOW;
        expected = ELOOP;
    } else if (!strcmp(argv[1], "bad-flags")) {
        path = "/usr/bin/echo";
        flags = 0x80000000U;
        expected = EINVAL;
    } else if (!strcmp(argv[1], "bad-fd")) {
        fd = -1;
        path = "";
        flags = LX_AT_EMPTY_PATH;
        expected = EBADF;
    } else if (!strcmp(argv[1], "detached")) {
        fd = open("/tmp/execveat-detached", O_PATH);
        if (fd < 0 || unlink("/tmp/execveat-detached") < 0) return 92;
        path = "";
        flags = LX_AT_EMPTY_PATH;
        expected = ENOSYS;
    } else if (!strcmp(argv[1], "empty-no-flag")) {
        path = "";
        expected = ENOENT;
    } else if (!strcmp(argv[1], "bad-path")) {
        path = (const char *)1;
        expected = EFAULT;
    } else return 91;
    long rc = syscall(LX_EXECVEAT, fd, path, args, env, flags);
    if (expected && rc == -1 && errno == expected) {
        printf("EXECVEAT_ERR_OK %s %d\n", argv[1], errno);
        return 0;
    }
    fprintf(stderr, "execveat %s: rc=%ld errno=%d expected=%d\n", argv[1], rc, errno, expected);
    return 93;
}
