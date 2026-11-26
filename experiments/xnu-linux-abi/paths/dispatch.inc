/* Reuse shim O_PATH/statx contracts; descriptors and lookup stay native. */
#include <sys/fcntl.h>
int rd_linux_opath(struct proc *,int,user_addr_t,uint64_t,int32_t *);
int rd_linux_path_status(struct proc *,int,int *);
int rd_linux_path_readlink(struct proc *,int,user_addr_t,size_t,int32_t *);
int rd_linux_statx_fd(struct proc *,int,user_addr_t);
int rd_linux_statx_path(struct proc *,int,user_addr_t,user_addr_t,int);
static bool rd_linux_path_fd(struct proc *p,int fd)
{ int status=-1;return !rd_linux_path_status(p,fd,&status)&&status>=0; }
static int64_t rd_linux_statx(struct proc *p,arm_saved_state_t *state)
{
    int fd=(int)get_saved_state_reg(state,0);user_addr_t path=get_saved_state_reg(state,1);
    uint32_t flags=get_saved_state_reg(state,2),mask=get_saved_state_reg(state,3);
    user_addr_t out=get_saved_state_reg(state,4);
    if((flags&~0x7900U)||(flags&0x6000)==0x6000||(mask&0x80000000))return -22;
    if(flags&0x6000)return -95; /* Unqualified explicit synchronization modes. */
    char first=0;
    if(!path && !(flags&0x1000))return -14;
    if(path&&copyin(path,&first,1))return -14;
    int error;
    if(!first&&(flags&0x1000)){
        if(fd==-100)error=rd_linux_statx_path(p,AT_FDCWD,0,out,0);
        else error=rd_linux_statx_fd(p,fd,out);
    }else error=rd_linux_statx_path(p,fd==-100?AT_FDCWD:fd,path,out,(flags&0x100)?AT_SYMLINK_NOFOLLOW:0);
    return error ? -rd_linux_errno(error) : 0;
}
