/* Ordinary Linux AArch64 ELF. Native parent supplies tty fd0, fault-test fd3. */
.section .text
.global _start
_start:
    mov x16, #0
    mov x24, #11
    mov x0, #-1
    mov x1, #1
    mov x2, #1
    mov x8, #63
    svc #0
    cmn x0, #9
    b.ne fail
    mov x24, #12
    mov x0, #0
    mov x1, #0
    mov x2, #0
    mov x8, #63
    svc #0
    cbnz x0, fail
    mov x24, #13
    mov x0, #3
    mov x1, #1
    mov x2, #1
    mov x8, #63
    svc #0
    cmn x0, #14
    b.ne fail
    adrp x1, ready
    add x1, x1, :lo12:ready
    mov x2, #(ready_end-ready)
    bl emit
    mov x24, #14
    adrp x19, buffer
    add x19, x19, :lo12:buffer
    mov x0, #0
    mov x1, x19
    mov x2, #16
    mov x8, #63
    svc #0
    cmp x0, #4
    b.ne fail
    ldr w0, [x19]
    mov w1, #0x6261
    movk w1, #0x0a44, lsl #16
    cmp w0, w1
    b.ne fail
    adrp x1, line
    add x1, x1, :lo12:line
    mov x2, #(line_end-line)
    bl emit
    mov x24, #15
    mov x0, #0
    mov x1, x19
    mov x2, #16
    mov x8, #63
    svc #0
    cbnz x0, fail
    adrp x1, eof
    add x1, x1, :lo12:eof
    mov x2, #(eof_end-eof)
    bl emit
    mov x24, #0
fail:
    mov x0, x24
    mov x8, #94
    svc #0
    b .
emit:
    mov x0, #1
    mov x8, #64
    svc #0
    cmp x0, x2
    b.ne fail
    ret
.section .rodata
ready: .ascii "READ_READY\n"
ready_end:
line: .ascii "READ_LINE_PASS\n"
line_end:
eof: .ascii "READ_EOF_PASS\n"
eof_end:
.section .bss
.balign 16
buffer: .skip 16
