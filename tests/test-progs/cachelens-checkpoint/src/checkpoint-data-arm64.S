/*
 * Copyright (c) 2026 The Regents of the University of California
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

.section .bss
.balign 64
buffer:
    .skip 4096

.section .rodata
success_message:
    .ascii "CACHELENS_CHECKPOINT_DATA_OK\n"
.set success_message_size, . - success_message

.section .text
.globl _start
.type _start, %function
_start:
    adrp x9, buffer
    add x9, x9, :lo12:buffer
    mov x10, #0
    mov x11, #4096
    mov w13, #0x5a
.Lfill:
    eor w12, w10, w13
    strb w12, [x9, x10]
    add x10, x10, #1
    cmp x10, x11
    b.ne .Lfill

    mov x0, #0
    mov x1, #0
    bl m5_checkpoint

    adrp x9, buffer
    add x9, x9, :lo12:buffer
    mov x10, #0
    mov x11, #4096
    mov w13, #0x5a
.Lverify:
    eor w12, w10, w13
    and w12, w12, #0xff
    ldrb w14, [x9, x10]
    cmp w12, w14
    b.ne .Lfailure
    add x10, x10, #1
    cmp x10, x11
    b.ne .Lverify

    mov x0, #1
    adrp x1, success_message
    add x1, x1, :lo12:success_message
    mov x2, #success_message_size
    mov x8, #64
    svc #0

    mov x0, #0
    mov x8, #93
    svc #0

.Lfailure:
    mov x0, #1
    mov x8, #93
    svc #0

.type m5_checkpoint, %function
m5_checkpoint:
    .inst 0xff430110
    ret

.section .note.GNU-stack,"",%progbits
