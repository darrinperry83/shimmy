/* Only the KDK locore object's two imports are redirected here.
 * Preserve the custom helper ABI: all registers except x1/x2/x16/x17.
 * No userspace instruction or original KDK instruction is changed.
 */
#include "assym.s"
.text
.p2align 2
.globl _rd_linux_entry_sign
_rd_linux_entry_sign:
    tbnz w2, #2, 1f              /* EL1 state: retain the native path. */
    tbnz w2, #3, 1f
    mrs x16, TPIDR_EL1
    adrp x17, _rd_linux_uthread_tail_offset@PAGE
    ldr x17, [x17, _rd_linux_uthread_tail_offset@PAGEOFF]
    add x16, x16, x17
    add x16, x16, #ACT_RDXNU_THREAD_SIZE
    ldr x17, [x16, #24]
    cbz x17, 1f
    mrs x17, TPIDR_EL0
    str x17, [x16]
    mrs x16, TPIDR_EL1
    ldr x16, [x16, ACT_CPUDATAP]
    ldr x16, [x16, CPU_TPIDR_EL0]
    msr TPIDR_EL0, x16
1:
    b _ml_sign_thread_state

.p2align 2
.globl _rd_linux_entry_check
_rd_linux_entry_check:
    tbnz w2, #2, 1f
    tbnz w2, #3, 1f
    mrs x16, TPIDR_EL1
    adrp x17, _rd_linux_uthread_tail_offset@PAGE
    ldr x17, [x17, _rd_linux_uthread_tail_offset@PAGEOFF]
    add x16, x16, x17
    add x16, x16, #ACT_RDXNU_THREAD_SIZE
    ldr x17, [x16, #24]
    cbz x17, 1f
    pacibsp
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    bl _ml_check_signed_state    /* Authenticate before restoring user TLS. */
    mrs x16, TPIDR_EL1
    adrp x17, _rd_linux_uthread_tail_offset@PAGE
    ldr x17, [x17, _rd_linux_uthread_tail_offset@PAGEOFF]
    add x16, x16, x17
    add x16, x16, #ACT_RDXNU_THREAD_SIZE
    ldr x17, [x16]
    msr TPIDR_EL0, x17
    ldr x18, [x0, SS64_X18]
    ldp x29, x30, [sp], #16
    autibsp
    ret
1:
    b _ml_check_signed_state
