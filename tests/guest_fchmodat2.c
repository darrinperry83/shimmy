#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_fchmodat2
#define SYS_fchmodat2 452
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

static int mode_is(const char *path, mode_t want) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & 07777) == want;
}

int main(void) {
    const char *file = "/tmp/fchmodat2-file";
    const char *dir = "/tmp/fchmodat2-dir";
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return 1;
    int fd = open(file, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0 || chmod(file, 0644) != 0) return 2;

    // An unsupported flag must not silently chmod the target.
    if (syscall(SYS_fchmodat2, AT_FDCWD, file, 0600, 0x400) != -1 || errno != EINVAL)
        return 3;
    if (!mode_is(file, 0644)) return 4;
    if (syscall(SYS_fchmodat2, AT_FDCWD, file, 0600, 0) != 0 || !mode_is(file, 0600))
        return 5;
    if (syscall(SYS_fchmodat2, fd, "", 0640, AT_EMPTY_PATH) != 0 || !mode_is(file, 0640))
        return 6;
    if (syscall(SYS_fchmodat2, AT_FDCWD, "", 0600, 0) != -1 || errno != ENOENT)
        return 7;

    // The host and guest cwd differ; AT_FDCWD must select the guest cwd.
    if (chdir(dir) != 0) return 8;
    if (syscall(SYS_fchmodat2, AT_FDCWD, "", 0700, AT_EMPTY_PATH) != 0 ||
        !mode_is(".", 0700)) return 9;
    if (syscall(SYS_fchmodat2, AT_FDCWD, NULL, 0700, 0) != -1 || errno != EFAULT)
        return 10;
    // A non-null unmapped guest address must not be read by the empty-path
    // shortcut or passed raw to the resolver, nor change either inode.
    const char *bad = (const char *)(unsigned long)3;
    if (syscall(SYS_fchmodat2, fd, bad, 0777, AT_EMPTY_PATH) != -1 ||
        errno != EFAULT || !mode_is(file, 0640)) return 11;
    if (syscall(SYS_fchmodat2, AT_FDCWD, bad, 0777, 0) != -1 ||
        errno != EFAULT || !mode_is(".", 0700)) return 12;
    puts("GUEST_FCHMODAT2_OK");
    return 0;
}
