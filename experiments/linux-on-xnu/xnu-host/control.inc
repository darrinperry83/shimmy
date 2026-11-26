/* Root-only trigger; this never changes the Linux ELF syscall dispatcher. */
#include <sys/sysctl.h>
extern int rdxnu_linux_core_status(void);
extern int rdxnu_linux_core_start(void);
static int
rdxnu_linux_core_control SYSCTL_HANDLER_ARGS
{
    (void)arg1; (void)arg2;
    int value = rdxnu_linux_core_status();
    int error = sysctl_handle_int(oidp, &value, 0, req);
    if (error || !req->newptr) return error;
    if (value != 1) return EINVAL;
    int result = rdxnu_linux_core_start();
    return result == 0 ? 0 : result == -1 ? EPERM : result == -2 ? EALREADY : ENOMEM;
}
SYSCTL_PROC(_kern, OID_AUTO, rdxnu_linux_core,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, rdxnu_linux_core_control, "I", "Opt-in Linux core test: 0 idle, 1 running, 2 pass, negative failure");
extern void rdxnu_linux_core_log(char *, size_t);
static int
rdxnu_linux_core_log_control SYSCTL_HANDLER_ARGS
{
    (void)arg1; (void)arg2;
    char buffer[4096]; rdxnu_linux_core_log(buffer,sizeof(buffer));
    return sysctl_handle_string(oidp,buffer,sizeof(buffer),req);
}
SYSCTL_PROC(_kern, OID_AUTO, rdxnu_linux_core_log,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, rdxnu_linux_core_log_control, "A", "Linux core diagnostic startup log");
