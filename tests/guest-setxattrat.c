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

static long raw_setxattrat(int dfd, const char *path, unsigned int at_flags,
                           const char *name, void *args, size_t usize) {
    register long x0 __asm__("x0") = dfd;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = at_flags;
    register long x3 __asm__("x3") = (long)name;
    register long x4 __asm__("x4") = (long)args;
    register long x5 __asm__("x5") = usize;
    register long x8 __asm__("x8") = 463;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                     "r"(x4), "r"(x5), "r"(x8) : "memory");
    return x0;
}

#define CHECK(test, code) do { if (!(test)) return (code); } while (0)

int main(void) {
    const char *path = "/tmp/setxattrat-file";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    CHECK(fd >= 0, 1);
    close(fd);
    const char first[] = "first", second[] = "second";
    struct xattr_args_guest a = {(uint64_t)(uintptr_t)first, 5, 1};
    CHECK(raw_setxattrat(AT_FDCWD, path, 0, "user.shimmy", &a, sizeof a) == 0, 2);
    char got[16] = {0};
    CHECK(getxattr(path, "user.shimmy", got, sizeof got) == 5 &&
          memcmp(got, first, 5) == 0, 3);
    CHECK(raw_setxattrat(AT_FDCWD, path, 0, "user.shimmy", &a, sizeof a) == -17, 4);
    int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0, 5);
    a.value = (uint64_t)(uintptr_t)second; a.size = 6; a.flags = 2;
    CHECK(raw_setxattrat(dfd, "setxattrat-file", 0, "user.shimmy", &a, sizeof a) == 0, 6);
    close(dfd);
    memset(got, 0, sizeof got);
    CHECK(getxattr(path, "user.shimmy", got, sizeof got) == 6 &&
          memcmp(got, second, 6) == 0, 7);
    CHECK(raw_setxattrat(AT_FDCWD, path, 0, "user.missing", &a, sizeof a) == -61, 8);

    // Rejected calls must not change the existing value.
    a.flags = 4;
    CHECK(raw_setxattrat(AT_FDCWD, path, 0, "user.shimmy", &a, sizeof a) == -22, 9);
    a.flags = 3;
    CHECK(raw_setxattrat(AT_FDCWD, path, 0, "user.shimmy", &a, sizeof a) == -22, 10);
    a.flags = 0;
    CHECK(raw_setxattrat(AT_FDCWD, path, 0x80000000u, "user.shimmy", &a, sizeof a) == -22, 11);
    CHECK(raw_setxattrat(AT_FDCWD, path, 0, "user.shimmy", &a, sizeof a - 1) == -22, 12);
    CHECK(raw_setxattrat(AT_FDCWD, path, 0, "user.shimmy", 0, sizeof a) == -14, 13);
    CHECK(raw_setxattrat(AT_FDCWD, (void *)1, 0, "user.shimmy", &a, sizeof a) == -14, 14);
    CHECK(raw_setxattrat(AT_FDCWD, path, 0, (void *)1, &a, sizeof a) == -14, 15);
    a.value = 1;
    CHECK(raw_setxattrat(AT_FDCWD, path, 0, "user.shimmy", &a, sizeof a) == -14, 16);
    a.value = (uint64_t)(uintptr_t)second;
    a.size = 65537;
    CHECK(raw_setxattrat(AT_FDCWD, path, 0, "user.shimmy", &a, sizeof a) == -7, 17);
    a.size = 6;
    CHECK(raw_setxattrat(AT_FDCWD, "", 0x1000, "user.shimmy", &a, sizeof a) == -95, 18);
    struct { struct xattr_args_guest args; uint64_t extra; } ext = {a, 0};
    CHECK(raw_setxattrat(AT_FDCWD, path, 0, "user.shimmy", &ext, sizeof ext) == 0, 19);
    ext.extra = 1;
    CHECK(raw_setxattrat(AT_FDCWD, path, 0, "user.shimmy", &ext, sizeof ext) == -7, 20);
    memset(got, 0, sizeof got);
    CHECK(getxattr(path, "user.shimmy", got, sizeof got) == 6 &&
          memcmp(got, second, 6) == 0, 21);
    puts("GUEST_SETXATTRAT_OK");
    return 0;
}
