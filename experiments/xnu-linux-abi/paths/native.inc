/* Linux O_PATH owns a native fileglob and vnode reference, never a data open.
 * No VNOP_OPEN/CLOSE, data permission request, device open, or fd shadow table.
 */
/* APFS drops the target on unlink even with an ordinary Darwin O_SYMLINK
 * descriptor held. Symlink text is immutable: capture it while acquiring the
 * reference. A per-symlink ops object owns the text without changing fileglob
 * layout or replacing its native vnode data. Dup shares this object naturally.
 */
struct rd_path_link { struct fileops ops;size_t length;char target[MAXPATHLEN]; };
static const struct fileops rd_path_ops;
static int rd_path_io(struct fileproc *fp, struct uio *uio, int flags, vfs_context_t ctx)
{ (void)fp;(void)uio;(void)flags;(void)ctx;return EBADF; }
static int rd_path_ioctl(struct fileproc *fp,u_long cmd,caddr_t data,vfs_context_t ctx)
{ (void)fp;(void)cmd;(void)data;(void)ctx;return EBADF; }
static int rd_path_select(struct fileproc *fp,int which,void *wql,vfs_context_t ctx)
{ (void)fp;(void)which;(void)wql;(void)ctx;return 0; }
static int rd_path_filter(struct fileproc *fp,struct knote *kn,struct kevent_qos_s *kev)
{ (void)fp;(void)kn;(void)kev;return EBADF; }
static int rd_path_drain(struct fileproc *fp,vfs_context_t ctx)
{ (void)fp;(void)ctx;return 0; }
static int rd_path_close(struct fileglob *fg,vfs_context_t ctx)
{
    (void)ctx;vnode_rele((vnode_t)fg_get_data(fg));
    if(fg->fg_ops!=&rd_path_ops){
        struct rd_path_link *link=__DECONST(struct rd_path_link *,fg->fg_ops);
        fg->fg_ops=&rd_path_ops;kfree_type(struct rd_path_link,link);
    }
    return 0;
}
static const struct fileops rd_path_ops={
    .fo_type=DTYPE_VNODE,.fo_read=rd_path_io,.fo_write=rd_path_io,
    .fo_ioctl=rd_path_ioctl,.fo_select=rd_path_select,.fo_close=rd_path_close,
    .fo_kqfilter=rd_path_filter,.fo_drain=rd_path_drain,
};

int rd_linux_path_status(proc_t p,int fd,int *status)
{
    struct fileproc *fp;int error=fp_lookup(p,fd,&fp,0);
    if(error)return error;
    *status=-1;
    if(fp->fp_glob->fg_ops->fo_close==rd_path_close)
        *status=0x200000 | ((fp->fp_glob->fg_flag&O_DIRECTORY)?0x4000:0) |
            ((fp->fp_glob->fg_flag&O_SYMLINK)?0x8000:0);
    fp_drop(p,fd,fp,0);
    return 0;
}

int rd_linux_path_readlink(proc_t p,int fd,user_addr_t out,size_t size,int32_t *result)
{
    if(!size || size>INT32_MAX)return EINVAL;
    struct fileproc *fp;int error=fp_lookup(p,fd,&fp,0);
    if(error)return error;
    const struct fileops *ops=fp->fp_glob->fg_ops;
    if(ops->fo_close!=rd_path_close || ops==&rd_path_ops){
        fp_drop(p,fd,fp,0);
        struct freadlink_args args={};args.fd=fd;args.buf=out;args.bufsize=size;
        return freadlink(p,&args,result);
    }
    vnode_t vp=(vnode_t)fp_get_data(fp);
    error=vnode_getwithref(vp);
    if(!error){
        vfs_context_t ctx=vfs_context_current();
#if CONFIG_MACF
        error=mac_vnode_check_readlink(ctx,vp);
#endif
        if(!error)error=vnode_authorize(vp,NULL,KAUTH_VNODE_READ_DATA,ctx);
        if(!error){
            const struct rd_path_link *link=(const struct rd_path_link *)ops;
            size_t length=MIN(size,link->length);
            error=copyout(link->target,out,length);
            if(!error)*result=(int32_t)length;
        }
        vnode_put(vp);
    }
    fp_drop(p,fd,fp,0);return error;
}

int rd_linux_opath(proc_t p,int dirfd,user_addr_t path,uint64_t flags,int32_t *result)
{
    vfs_context_t ctx=vfs_context_current();struct nameidata nd;
    NDINIT(&nd,LOOKUP,OP_OPEN,((flags&0x8000)?NOFOLLOW:FOLLOW)|AUDITVNPATH1,
        UIO_USERSPACE64,path,ctx);
    int error=nameiat(&nd,dirfd);
    if(error)return error;
    vnode_t vp=nd.ni_vp;nameidone(&nd);
    if((flags&0x4000)&&!vnode_isdir(vp)){vnode_put(vp);return ENOTDIR;}
    error=vnode_ref(vp);
    if(error){vnode_put(vp);return error;}
    struct rd_path_link *link=NULL;
    if(vnode_islnk(vp)){
        link=kalloc_type(struct rd_path_link,Z_WAITOK|Z_ZERO);
        if(!link){vnode_rele(vp);vnode_put(vp);return ENOMEM;}
        int length=0;
        error=readlinkat_internal(ctx,-1,vp,0,UIO_SYSSPACE,
            CAST_USER_ADDR_T(link->target),sizeof(link->target),UIO_SYSSPACE,&length);
        if(error){kfree_type(struct rd_path_link,link);vnode_rele(vp);vnode_put(vp);return error;}
        link->ops=rd_path_ops;link->length=(size_t)length;
    }
    struct fileproc *fp;int fd;
    error=falloc_withinit(p,current_cached_proc_cred(p),ctx,&fp,&fd,NULL,NULL);
    if(error){if(link)kfree_type(struct rd_path_link,link);vnode_rele(vp);vnode_put(vp);return error;}
    fp->fp_glob->fg_ops=link ? &link->ops : &rd_path_ops;
    fp->fp_glob->fg_flag= ((flags&0x4000)?O_DIRECTORY:0) | ((flags&0x8000)?O_SYMLINK:0);
    if(flags&0x80000)fp->fp_flags|=FP_CLOEXEC;
    fp_set_data(fp,vp);vnode_put(vp);
    proc_fdlock(p);procfdtbl_releasefd(p,fd,NULL);fp_drop(p,fd,fp,1);proc_fdunlock(p);
    *result=fd;return 0;
}

int rd_linux_statx_path(proc_t p,int fd,user_addr_t path,user_addr_t out,int flags)
{
    (void)p;
    return fstatat_internal(vfs_context_current(),path ? path : CAST_USER_ADDR_T("."),out,0,0,2,
        path ? UIO_USERSPACE64 : UIO_SYSSPACE,fd,flags);
}
