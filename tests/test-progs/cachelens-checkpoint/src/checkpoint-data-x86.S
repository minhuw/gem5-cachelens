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
.type _start, @function
_start:
    lea buffer(%rip), %rdi
    xor %ecx, %ecx
.Lfill:
    mov %ecx, %eax
    xor $0x5a, %al
    mov %al, (%rdi,%rcx)
    inc %ecx
    cmp $4096, %ecx
    jne .Lfill

    xor %edi, %edi
    xor %esi, %esi
    call m5_checkpoint

    lea buffer(%rip), %rdi
    xor %ecx, %ecx
.Lverify:
    mov %ecx, %eax
    xor $0x5a, %al
    cmp %al, (%rdi,%rcx)
    jne .Lfailure
    inc %ecx
    cmp $4096, %ecx
    jne .Lverify

    mov $1, %eax
    mov $1, %edi
    lea success_message(%rip), %rsi
    mov $success_message_size, %edx
    syscall

    xor %edi, %edi
    mov $60, %eax
    syscall

.Lfailure:
    mov $1, %edi
    mov $60, %eax
    syscall

.type m5_checkpoint, @function
m5_checkpoint:
    .byte 0x0f, 0x04
    .word 0x43
    ret

.section .note.GNU-stack,"",@progbits
