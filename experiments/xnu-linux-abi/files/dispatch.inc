/* Reuses src/syscalls.c ARM64 flag/errno/stat contracts. XNU owns descriptors. */
#include <sys/fcntl.h>
static int rd_linux_dirfd(uint64_t fd)
{
    return (int)fd == -100 ? AT_FDCWD : (int)fd;
}

static int64_t rd_linux_files(struct proc *p, arm_saved_state_t *state, uint64_t nr)
{
    uint64_t a0 = get_saved_state_reg(state,0), a1 = get_saved_state_reg(state,1);
    uint64_t a2 = get_saved_state_reg(state,2), a3 = get_saved_state_reg(state,3);
    int error = 0;
    int32_t result = 0;
    if (nr == 56) {
        /* O_PATH, O_DIRECT, O_NOATIME, O_TMPFILE and unknown bits are deferred.
         * Never pass Linux bit values directly to Darwin openat. */
        const uint64_t supported = 3 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 |
            0x800 | 0x1000 | 0x2000 | 0x4000 | 0x8000 | 0x20000 | 0x80000 | 0x100000;
        if (a2 & ~supported) return -95;
        if ((a2 & 3) == 3) return -22;
        int flags = (int)(a2 & 3);
        if (a2 & 0x40) flags |= O_CREAT;
        if (a2 & 0x80) flags |= O_EXCL;
        if (a2 & 0x100) flags |= O_NOCTTY;
        if (a2 & 0x200) flags |= O_TRUNC;
        if (a2 & 0x400) flags |= O_APPEND;
        if (a2 & 0x800) flags |= O_NONBLOCK;
        if (a2 & 0x1000) flags |= O_DSYNC;
        if (a2 & 0x2000) flags |= O_ASYNC;
        if (a2 & 0x4000) flags |= O_DIRECTORY;
        if (a2 & 0x8000) flags |= O_NOFOLLOW;
        if (a2 & 0x80000) flags |= O_CLOEXEC;
        if (a2 & 0x100000) flags |= O_SYNC;
        struct openat_args args = {};
        args.fd = rd_linux_dirfd(a0); args.path = a1;
        args.flags = flags; args.mode = (int)(a3 & 07777);
        error = openat(p,&args,&result);
    } else if (nr == 57) {
        struct close_args args = {};
        args.fd = (int)a0;
        error = sys_close(p,&args,&result);
    } else if (nr == 62) {
        if (a2 > 2) return -22; /* SEEK_DATA/HOLE need Linux-specific handling. */
        struct lseek_args args = {};
        args.fd = (int)a0; args.offset = (off_t)a1; args.whence = (int)a2;
        off_t offset = 0;
        error = lseek(p,&args,&offset);
        return error ? -rd_linux_errno(error) : offset;
    } else if (nr == 80) {
        struct fstat64_args args = {};
        args.fd = (int)a0; args.ub = a1;
        error = sys_fstat64(p,&args,&result);
    } else if (nr == 79) {
        if (a3 & ~0x1900ULL) return -22; /* NOFOLLOW, NO_AUTOMOUNT, EMPTY_PATH */
        char first = 0;
        if (!a1 && !(a3 & 0x1000)) return -14;
        if (a1 && copyin(a1,&first,1)) return -14;
        if (!first && (a3 & 0x1000)) {
            if ((int)a0 == -100) return -95; /* cwd empty-path form deferred */
            struct fstat64_args args = {};
            args.fd = (int)a0; args.ub = a2;
            error = sys_fstat64(p,&args,&result);
        } else {
            struct fstatat64_args args = {};
            args.fd = rd_linux_dirfd(a0); args.path = a1; args.ub = a2;
            args.flag = (a3 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0;
            error = fstatat64(p,&args,&result);
        }
    } else if (nr == 78) {
        if (!a3 || a3 > INT32_MAX) return -22;
        struct readlinkat_args args = {};
        args.fd = rd_linux_dirfd(a0); args.path = a1;
        args.buf = a2; args.bufsize = (int)a3;
        error = readlinkat(p,&args,&result);
    } else if (nr == 25) {
        if (a1 != 1 && a1 != 2) return -95; /* F_GETFD/F_SETFD only. */
        struct fcntl_args args = {};
        args.fd = (int)a0; args.cmd = a1 == 1 ? F_GETFD : F_SETFD;
        args.arg = a2 & 1;
        error = sys_fcntl(p,&args,&result);
    } else return -38;
    return error ? -rd_linux_errno(error) : result;
}
