/* Included after native kern_resource.c helpers. All limits remain native. */
int rd_linux_prlimit(struct proc *p,int pid,unsigned resource,user_addr_t address,user_addr_t old_address)
{
    if(pid&&pid!=proc_getpid(p))return ENOTSUP;
    static const int map[]={RLIMIT_CPU,RLIMIT_FSIZE,RLIMIT_DATA,RLIMIT_STACK,
        RLIMIT_CORE,RLIMIT_RSS,RLIMIT_NPROC,RLIMIT_NOFILE,RLIMIT_MEMLOCK,RLIMIT_AS};
    if(resource>=sizeof(map)/sizeof(map[0]))return EINVAL;
    struct rlimit before=proc_limitget(p,map[resource]);int error=0;
    if(address){
        if(resource!=4&&resource!=7)return ENOTSUP;
        uint64_t pair[2];if(copyin(address,pair,sizeof(pair)))return EFAULT;
        if(pair[0]>pair[1])return EINVAL;
        for(unsigned i=0;i<2;i++)if(pair[i]!=UINT64_MAX&&pair[i]>RLIM_INFINITY)return ENOTSUP;
        struct rlimit limit={.rlim_cur=pair[0]==UINT64_MAX?RLIM_INFINITY:pair[0],
                             .rlim_max=pair[1]==UINT64_MAX?RLIM_INFINITY:pair[1]};
        error=dosetrlimit(p,map[resource],&limit);
    }
    if(!error&&old_address){uint64_t pair[2]={before.rlim_cur==RLIM_INFINITY?UINT64_MAX:before.rlim_cur,
                                            before.rlim_max==RLIM_INFINITY?UINT64_MAX:before.rlim_max};
        error=copyout(pair,old_address,sizeof(pair));}
    return error;
}
