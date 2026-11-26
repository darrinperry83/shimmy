/* BSD-owned tail follows the unchanged native thread and uthread fields. */
#include <sys/user.h>
void rd_linux_machine_exec(thread_t thread, bool linux, uint64_t mask)
{
    struct uthread *ut = get_bsdthread_info(thread);
    ut->rd_linux_tls = ut->rd_linux_clear_tid = 0;
    ut->rd_linux_mask = mask;
    ut->rd_linux_enabled = linux;
}
void rd_linux_machine_set_clear_tid(thread_t thread, uint64_t address)
{ get_bsdthread_info(thread)->rd_linux_clear_tid=address; }
uint64_t rd_linux_machine_clear_tid(thread_t thread)
{ return get_bsdthread_info(thread)->rd_linux_clear_tid; }
void rd_linux_machine_set_mask(thread_t thread, uint64_t mask)
{ get_bsdthread_info(thread)->rd_linux_mask=mask; }
uint64_t rd_linux_machine_mask(thread_t thread)
{ return get_bsdthread_info(thread)->rd_linux_mask; }
