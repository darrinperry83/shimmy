/* Linux AArch64 waitid siginfo layout; all spare bytes are zero. */
struct rd_linux_wait_info {
    int32_t signo,error,code,pad;
    int32_t pid;uint32_t uid;int32_t status,pad2;
    int64_t utime,stime;uint64_t spare[10];
};
_Static_assert(sizeof(struct rd_linux_wait_info)==128,"waitid siginfo size");
_Static_assert(offsetof(struct rd_linux_wait_info,pid)==16,"waitid child offset");
static int rd_linux_wait_signal(int native)
{
    static const uint8_t signals[32]={0,1,2,3,4,5,6,10,8,9,30,11,31,13,14,15,
                                     0,20,19,17,18,21,22,16,24,25,26,27,28,23,0,12};
    for(int linux=1;linux<32;linux++)if(signals[linux]==native)return linux;
    return 0;
}
static bool rd_linux_wait_caller(void)
{ return (os_atomic_load(&current_proc()->p_ladvflag,relaxed)&P_RDXNU_LINUX)!=0; }
static int rd_linux_wait_copyout(user_siginfo_t *native,user_addr_t address)
{
    struct rd_linux_wait_info out={};
    if(native){
        out.signo=17;out.code=native->si_code;out.pid=native->si_pid;
        out.uid=native->si_uid;out.status=native->si_status;
        if(native->si_code==CLD_KILLED || native->si_code==CLD_DUMPED)
            out.status=rd_linux_wait_signal(native->si_status);
    }
    return copyout(&out,address,sizeof(out));
}
