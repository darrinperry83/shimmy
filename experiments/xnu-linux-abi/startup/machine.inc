/* Compiled in pcb.c: BSD code keeps struct thread opaque. */
void rd_linux_machine_exec(thread_t thread, bool linux, uint64_t mask)
{
    thread->machine.reserved4=thread->machine.reserved5=0;
    thread->machine.reserved6=mask;
    thread->machine.arm_machine_flags &= ~ARM_MACHINE_THREAD_RDXNU_LINUX;
    if (linux) thread->machine.arm_machine_flags |= ARM_MACHINE_THREAD_RDXNU_LINUX;
}
void rd_linux_machine_set_clear_tid(thread_t thread, uint64_t address)
{ thread->machine.reserved5=address; }
uint64_t rd_linux_machine_clear_tid(thread_t thread)
{ return thread->machine.reserved5; }
void rd_linux_machine_set_mask(thread_t thread, uint64_t mask)
{ thread->machine.reserved6=mask; }
uint64_t rd_linux_machine_mask(thread_t thread)
{ return thread->machine.reserved6; }
