#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_name_to_handle_at
#define SYS_name_to_handle_at 264
#endif
#ifndef AT_HANDLE_FID
#define AT_HANDLE_FID 0x200
#endif

static int check(int fd, const char *path, struct file_handle *handle,
                 int *mount_id, int flags, int expected) {
    errno = 0;
    long r = syscall(SYS_name_to_handle_at, fd, path, handle, mount_id, flags);
    return r == -1 && errno == expected;
}

int main(void) {
    struct { struct file_handle h; unsigned char bytes[16]; } handle = {0};
    handle.h.handle_bytes = 16;
    int mount_id = 12345;
    if (!check(AT_FDCWD, "/tmp", &handle.h, &mount_id, 0, EOPNOTSUPP)) return 1;
    if (handle.h.handle_bytes != 16 || mount_id != 12345) return 2;
    if (!check(AT_FDCWD, "/tmp/does-not-exist", &handle.h, &mount_id, 0, ENOENT)) return 3;
    if (!check(AT_FDCWD, "/tmp", &handle.h, &mount_id, 0x80000000u, EINVAL)) return 4;
    if (!check(AT_FDCWD, "/tmp", NULL, &mount_id, 0, EFAULT)) return 5;
    if (!check(AT_FDCWD, "/tmp", &handle.h, NULL, 0, EFAULT)) return 6;
    if (!check(AT_FDCWD, (const char *)1, &handle.h, &mount_id, 0, EFAULT)) return 7;
    if (!check(AT_FDCWD, "", &handle.h, &mount_id, 0, ENOENT)) return 8;
    if (!check(AT_FDCWD, "/tmp", &handle.h, &mount_id, AT_HANDLE_FID, EOPNOTSUPP)) return 9;
    if (!check(AT_FDCWD, "/tmp", (struct file_handle *)1, &mount_id, 0, EFAULT)) return 10;
    if (!check(AT_FDCWD, "/tmp", &handle.h, (int *)1, 0, EFAULT)) return 11;
    puts("GUEST_NAME_TO_HANDLE_AT_OK");
    return 0;
}
