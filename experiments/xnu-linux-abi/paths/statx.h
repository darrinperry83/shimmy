/* Linux AArch64 statx layout and masks from src/syscalls_ext.c. */
struct rd_statx_time { int64_t sec;uint32_t nsec;int32_t reserved; };
struct rd_linux_statx {
    uint32_t mask,blksize;uint64_t attributes;
    uint32_t nlink,uid,gid;uint16_t mode,pad;
    uint64_t ino,size,blocks,attributes_mask;
    struct rd_statx_time atime,btime,ctime,mtime;
    uint32_t rdev_major,rdev_minor,dev_major,dev_minor;
    uint64_t mnt_id;uint32_t dio_mem_align,dio_offset_align;uint64_t spare[12];
};
_Static_assert(sizeof(struct rd_linux_statx)==256,"statx size");
_Static_assert(__builtin_offsetof(struct rd_linux_statx,ino)==32,"statx ino");
_Static_assert(__builtin_offsetof(struct rd_linux_statx,atime)==64,"statx times");
_Static_assert(__builtin_offsetof(struct rd_linux_statx,dev_major)==136,"statx device");
static int rd_linux_statx_copyout(const struct stat64 *s,user_addr_t address,uint64_t mount_id)
{
    struct rd_linux_statx out={};
    out.mask=0x7ff;
    if(mount_id){out.mask|=0x1000;out.mnt_id=mount_id;}
    out.blksize=s->st_blksize;out.nlink=s->st_nlink;out.uid=s->st_uid;out.gid=s->st_gid;
    out.mode=s->st_mode;out.ino=s->st_ino;out.size=s->st_size;out.blocks=s->st_blocks;
    out.atime.sec=s->st_atimespec.tv_sec;out.atime.nsec=s->st_atimespec.tv_nsec;
    out.mtime.sec=s->st_mtimespec.tv_sec;out.mtime.nsec=s->st_mtimespec.tv_nsec;
    out.ctime.sec=s->st_ctimespec.tv_sec;out.ctime.nsec=s->st_ctimespec.tv_nsec;
    out.rdev_major=major(s->st_rdev);out.rdev_minor=minor(s->st_rdev);
    out.dev_major=major(s->st_dev);out.dev_minor=minor(s->st_dev);
    return copyout(&out,address,sizeof(out));
}
