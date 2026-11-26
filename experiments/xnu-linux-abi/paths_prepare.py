"""Opt-in path descriptors and statx; compose with the proven startup overlay."""
from pathlib import Path
from elf_prepare import replace_once
HERE=Path(__file__).resolve().parent

def overlay(root,edits):
    root=Path(root)
    path=root/'bsd/kern/rdxnu_linux_exec.h';before,data=edits[path];s=data.decode()
    s=replace_once(s,'static int64_t rd_linux_memory(', (HERE/'paths/dispatch.inc').read_text()+'\nstatic int64_t rd_linux_memory(')
    s=replace_once(s,'    if (nr == 48 || nr == 96',
        '    if (nr == 291) value=rd_linux_statx(p,state);\n    else if (nr == 48 || nr == 96')
    s=replace_once(s,'    if (nr == 56) {','''    if (nr == 56) {
        if (a2 & 0x200000) {
            error=rd_linux_opath(p,rd_linux_dirfd(a0),a1,a2,&result);
            return error ? -rd_linux_errno(error) : result;
        }''')
    s=replace_once(s,'    } else if (nr == 62) {','    } else if (nr == 62) {\n        if(rd_linux_path_fd(p,(int)a0))return -9;')
    s=replace_once(s,'        args.pos = (flags & 0x20) ? 0 : (off_t)offset;',
        '        args.pos = (flags & 0x20) ? 0 : (off_t)offset;\n        if(!(flags&0x20) && rd_linux_path_fd(p,args.fd))return -9;')
    s=replace_once(s,'        struct readlinkat_args args = {};','''        if(!a3)return -22;
        char first;
        if(copyin(a1,&first,1))return -14;
        if(!first){
            if((int)a0==-100)return -2;
            error=rd_linux_path_readlink(p,(int)a0,a2,(size_t)a3,&result);
            if(error==EINVAL)error=ENOENT; /* Linux empty-path non-symlink. */
            return error ? -rd_linux_errno(error) : result;
        }
        struct readlinkat_args args = {};''')
    s=replace_once(s,'''        if (a1 != 1 && a1 != 2) return -95; /* F_GETFD/F_SETFD only. */
        struct fcntl_args args = {};
        args.fd = (int)a0; args.cmd = a1 == 1 ? F_GETFD : F_SETFD;
        args.arg = a2 & 1;''','''        int path_status=-1;
        error=rd_linux_path_status(p,(int)a0,&path_status);
        if(error)return -rd_linux_errno(error);
        if(a1==3 && path_status>=0)return path_status;
        if(a1!=0 && a1!=1 && a1!=2 && a1!=3 && a1!=1030)
            return path_status>=0 ? -9 : -95;
        struct fcntl_args args = {};
        args.fd=(int)a0;
        args.cmd=a1==0?F_DUPFD:a1==1?F_GETFD:a1==2?F_SETFD:a1==3?F_GETFL:F_DUPFD_CLOEXEC;
        args.arg=a1==2 ? (a2&1) : a2;''')
    s=replace_once(s,'        error = sys_fcntl(p,&args,&result);','''        error = sys_fcntl(p,&args,&result);
        if(!error && a1==3){
            int native=result;result=(native&O_ACCMODE)|0x20000;
            if(native&O_APPEND)result|=0x400;
            if(native&O_NONBLOCK)result|=0x800;
            if(native&O_DSYNC)result|=0x1000;
            if(native&O_ASYNC)result|=0x2000;
            if(native&O_SYNC)result|=0x101000;
        }''')
    edits[path]=(before,s.encode())
    for relative in ('bsd/kern/kern_descrip.c','bsd/vfs/vfs_syscalls.c'):
        path=root/relative;before,data=edits[path];s=data.decode()
        s=replace_once(s,'#include "rdxnu_linux_stat.h"','#include "rdxnu_linux_stat.h"\n#include "rdxnu_linux_statx.h"')
        s=replace_once(s,'? rd_linux_stat_copyout(&source.sb64, ub) :',
            '? (isstat64==2 ? rd_linux_statx_copyout(&source.sb64, ub, rd_linux_mount_id) : rd_linux_stat_copyout(&source.sb64, ub)) :')
        if relative.startswith('bsd/kern'):
            s=replace_once(s,'fstat(proc_t p, int fd, user_addr_t ub, user_addr_t xsecurity,\n    user_addr_t xsecurity_size, int isstat64)\n{',
                'fstat(proc_t p, int fd, user_addr_t ub, user_addr_t xsecurity,\n    user_addr_t xsecurity_size, int isstat64)\n{\n\tuint64_t rd_linux_mount_id=0;')
            s=replace_once(s,'\t\tif ((error = vnode_getwithref((vnode_t)data)) == 0) {',
                '\t\tif ((error = vnode_getwithref((vnode_t)data)) == 0) {\n\t\t\tif(isstat64==2)rd_linux_mount_id=vfs_mount_id(vnode_mount((vnode_t)data))+1;')
            s+='\nint rd_linux_statx_fd(proc_t p,int fd,user_addr_t out)\n{ return fstat(p,fd,out,0,0,2); }\n'
        else:
            s=replace_once(s,'\tint needsrealdev = 0;','\tint needsrealdev = 0;\n\tuint64_t rd_linux_mount_id=0;')
            s=replace_once(s,'\tneedsrealdev = flag & AT_REALDEV ? 1 : 0;',
                '\tneedsrealdev = flag & AT_REALDEV ? 1 : 0;\n\tif(isstat64==2)rd_linux_mount_id=vfs_mount_id(vnode_mount(ndp->ni_vp))+1;')
            s+='\n'+(HERE/'paths/native.inc').read_text()
        edits[path]=(before,s.encode())
        path=path.parent/'rdxnu_linux_statx.h'
        if path.exists():raise ValueError(f'refusing to replace {path}')
        edits[path]=(None,(HERE/'paths/statx.h').read_bytes())
    return edits
