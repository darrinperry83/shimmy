/* Native path/credential checks precede task replacement. The held vnode is
 * mapped once into the new XNU map, then released on every activator exit. */
struct rd_interp_image {
    vnode_t vp;
    struct vnode_attr attr;
    struct image_params image;
    struct rd_elf_plan plan;
};
static void rd_interp_release(struct rd_interp_image *interp, vfs_context_t context)
{
    if (!interp) return;
    if (interp->vp) {
        VNOP_CLOSE(interp->vp,FREAD,context);
        vnode_put(interp->vp);
    }
    kfree_type(struct rd_interp_image,interp);
}
static int rd_interp_prepare(struct image_params *imgp, struct rd_elf_plan *main,
                            struct rd_interp_image **out)
{
    if (!main->interpreter[0]) return 0;
    struct rd_interp_image *interp = kalloc_type(struct rd_interp_image,Z_WAITOK|Z_ZERO);
    if (!interp) return ENOMEM;
    vnode_t vp = NULLVP;
    int error = vnode_lookup(main->interpreter,0,&vp,imgp->ip_vfs_context);
    if (error) { kfree_type(struct rd_interp_image,interp); return error; }
    /* Reuse exec's vnode/MAC authorization under the caller's real context. */
    interp->image = *imgp;
    interp->image.ip_vp = vp;
    interp->image.ip_vattr = &interp->attr;
    error = exec_check_permissions(&interp->image);
    if (!error && ((vp->v_mount->mnt_flag & MNT_NOEXEC) ||
        (interp->attr.va_mode & (S_ISUID|S_ISGID)))) error = EACCES;
    if (!error) error = VNOP_OPEN(vp,FREAD,imgp->ip_vfs_context);
    if (error) { vnode_put(vp); kfree_type(struct rd_interp_image,interp); return error; }
    interp->vp = vp;
    unsigned char *header = kalloc_data(RD_ELF_PAGE,Z_WAITOK);
    if (!header) { rd_interp_release(interp,imgp->ip_vfs_context); return ENOMEM; }
    int resid = 0;
    error = vn_rdwr(UIO_READ,vp,(caddr_t)header,RD_ELF_PAGE,0,UIO_SYSSPACE,0,
        vfs_context_ucred(imgp->ip_vfs_context),&resid,vfs_context_proc(imgp->ip_vfs_context));
    if (!error && RD_ELF_PAGE-resid < sizeof(struct rd_elf_ehdr)) error=EIO;
    if (!error && (rd_elf_plan_at(header,RD_ELF_PAGE-resid,interp->attr.va_data_size,
                                 &interp->plan,0x80000000ULL,0) ||
                   ((struct rd_elf_ehdr *)(void *)header)->type != 3)) error = ENOEXEC;
    kfree_data(header,RD_ELF_PAGE);
    if (error) { rd_interp_release(interp,imgp->ip_vfs_context); return error; }
    if (main->low < interp->plan.high && main->high > interp->plan.low) {
        rd_interp_release(interp,imgp->ip_vfs_context); return ENOEXEC;
    }
    *out = interp;
    return 0;
}
