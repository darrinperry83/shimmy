/* Linux AArch64 layout from src/syscalls.c. Native lookup/locking stays in XNU. */
struct rd_linux_stat {
    uint64_t dev, ino;
    uint32_t mode, nlink, uid, gid;
    uint64_t rdev, pad1;
    int64_t size;
    int32_t blksize, pad2;
    int64_t blocks, atime_sec, atime_nsec, mtime_sec, mtime_nsec, ctime_sec, ctime_nsec;
    uint32_t unused[2];
};
_Static_assert(sizeof(struct rd_linux_stat) == 128, "Linux stat size");
_Static_assert(__builtin_offsetof(struct rd_linux_stat, size) == 48, "Linux stat size offset");
_Static_assert(__builtin_offsetof(struct rd_linux_stat, ctime_sec) == 104, "Linux stat time offset");

static uint64_t rd_linux_dev(dev_t dev)
{
    /* Darwin has an 8-bit major and 24-bit minor; Linux packs them differently. */
    uint64_t min = minor(dev);
    return ((uint64_t)major(dev) << 8) | (min & 255) | ((min & ~255ULL) << 12);
}

static int rd_linux_stat_copyout(const struct stat64 *native, user_addr_t address)
{
    struct rd_linux_stat out = {};
    out.dev = rd_linux_dev(native->st_dev);
    out.ino = native->st_ino;
    out.mode = native->st_mode;
    out.nlink = native->st_nlink;
    out.uid = native->st_uid;
    out.gid = native->st_gid;
    out.rdev = rd_linux_dev(native->st_rdev);
    out.size = native->st_size;
    out.blksize = native->st_blksize;
    out.blocks = native->st_blocks;
    out.atime_sec = native->st_atimespec.tv_sec;
    out.atime_nsec = native->st_atimespec.tv_nsec;
    out.mtime_sec = native->st_mtimespec.tv_sec;
    out.mtime_nsec = native->st_mtimespec.tv_nsec;
    out.ctime_sec = native->st_ctimespec.tv_sec;
    out.ctime_nsec = native->st_ctimespec.tv_nsec;
    return copyout(&out, address, sizeof(out));
}
