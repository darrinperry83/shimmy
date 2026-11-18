#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_open_by_handle_at
#define SYS_open_by_handle_at 265
#endif

static int expect(int mount_fd, struct file_handle *handle, int flags, int error) {
    errno = 0;
    return syscall(SYS_open_by_handle_at, mount_fd, handle, flags) == -1 &&
           errno == error;
}

int main(void) {
    struct { struct file_handle h; unsigned char bytes[16]; } handle = {0};
    handle.h.handle_bytes = 16;
    handle.h.handle_type = 1;
    int mount_fd = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (mount_fd < 0) return 1;
    if (!expect(mount_fd, &handle.h, O_RDONLY, EOPNOTSUPP)) return 2;
    if (!expect(AT_FDCWD, &handle.h, O_RDONLY, EOPNOTSUPP)) return 3;
    if (!expect(mount_fd, NULL, O_RDONLY, EFAULT)) return 4;
    if (!expect(mount_fd, (struct file_handle *)1, O_RDONLY, EFAULT)) return 5;
    handle.h.handle_bytes = 0;
    if (!expect(mount_fd, &handle.h, O_RDONLY, EINVAL)) return 6;
    handle.h.handle_bytes = 129;
    if (!expect(mount_fd, &handle.h, O_RDONLY, EINVAL)) return 7;
    handle.h.handle_bytes = 16;
    handle.h.handle_type = -1;
    if (!expect(mount_fd, &handle.h, O_RDONLY, EINVAL)) return 8;
    handle.h.handle_type = 1;
    if (!expect(999999, &handle.h, O_RDONLY, EBADF)) return 9;
    close(mount_fd);
    puts("GUEST_OPEN_BY_HANDLE_AT_OK");
    return 0;
}
