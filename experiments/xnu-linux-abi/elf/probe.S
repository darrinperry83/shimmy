/* Unmodified Linux AArch64 ET_EXEC: no Darwin calls and no P0 x16 marker. */
.section .text
.global _start
_start:
    mov x24, #11
    mov x16, #0
    mov x20, sp
    tst x20, #15
    b.ne fail
    ldr x0, [sp]
    cmp x0, #1
    b.lt fail
    ldr x0, [sp, #8]
    cbz x0, fail
    mov x24, #12
    ldr x21, [sp]
    add x22, sp, #16
    add x22, x22, x21, lsl #3  // skip argc, argv and argv terminator
find_env_end:
    ldr x0, [x22], #8
    cbnz x0, find_env_end
    mov x23, #0
find_aux:
    ldp x0, x1, [x22], #16
    cbz x0, end_aux
    cmp x0, #3
    b.ne aux_phent
    cbz x1, fail
    orr x23, x23, #1
aux_phent:
    cmp x0, #4
    b.ne aux_phnum
    cmp x1, #56
    b.ne fail
    orr x23, x23, #2
aux_phnum:
    cmp x0, #5
    b.ne aux_pagesz
    cbz x1, fail
    orr x23, x23, #4
aux_pagesz:
    cmp x0, #6
    b.ne aux_entry
    cmp x1, #4096
    b.eq page_ok
    cmp x1, #16384
    b.ne fail
page_ok:
    orr x23, x23, #8
aux_entry:
    cmp x0, #9
    b.ne find_aux
    adrp x2, _start
    add x2, x2, :lo12:_start
    cmp x1, x2
    b.ne fail
    orr x23, x23, #16
    b find_aux
end_aux:
    cmp x23, #31
    b.ne fail
    mov x24, #13
    adrp x19, result
    add x19, x19, :lo12:result
    ldr x0, [x19, #8]       // zero-initialized BSS
    cbnz x0, fail
    mov x8, #172
    svc #0
    cmp x0, #0
    b.le fail
    str x0, [x19, #8]
    mov x24, #14
    mov x8, #-1
    svc #0
    cmn x0, #38
    b.ne fail
    mov x24, #15
    mov x0, #-1
    mov x1, x19
    mov x2, #1
    mov x8, #64
    svc #0
    cmn x0, #9
    b.ne fail
    mov x0, #1
    mov x24, #16
    mov x1, #1
    mov x2, #1
    mov x8, #64
    svc #0
    cmn x0, #14
    b.ne fail
    mov x24, #17
    adrp x1, magic
    add x1, x1, :lo12:magic
    ldr x0, [x1]
    str x0, [x19]
    mov x0, #1
    mov x1, x19
    mov x2, #16
    mov x8, #64
    svc #0
    cmp x0, #16
    b.ne fail
    mov x0, #37
    mov x8, #94
    svc #0
fail:
    mov x0, x24
    mov x8, #93
    svc #0
    b fail
.section .rodata
magic: .ascii "RDELF001"
.section .bss
.balign 16
result: .skip 16
.section .note.GNU-stack,"",@progbits
