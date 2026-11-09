/* Copyright (c) 2026; SPDX-License-Identifier: BSD-3-Clause
 * Rebuild with build_arm_split_timing.py. Four cores, NS EL1, 2 GHz CNTVCT.
 * Per-core records at 0x80200000 + cpu*64KiB, traces at +0x100:
 * {code, CNTVCT, argument}. IRQ records are 0x100 + interrupt ID.
 */
.text
.global _start
.macro address reg, value
    movz \reg, #(\value & 0xffff)
    movk \reg, #((\value >> 16) & 0xffff), lsl #16
.endm
.macro barrier phase
    mov x0, #\phase
    str x0, [x20, #56]
    dsb ish
    address x2, 0x80200000
    mov x3, #4
1:  ldr x4, [x2, #56]
    cmp x4, #\phase
    b.lo 1b
    add x2, x2, #16, lsl #12
    subs x3, x3, #1
    b.ne 1b
.endm
.macro wait_timer count
1:  ldr x0, [x20, #32]
    cmp x0, #\count
    b.hs 2f
    nop
    b 1b
2:
.endm
_start:
    msr daifset, #15
    mrs x19, mpidr_el1
    and x19, x19, #255
    address x20, 0x80200000
    add x20, x20, x19, lsl #16
    add x0, x20, #15, lsl #12
    msr sp_el1, x0
    mov sp, x0
    adr x0, vectors
    msr vbar_el3, x0
    msr vbar_el1, x0
    address x26, 0x2c001000
    address x27, 0x2c002000
    mov w0, #3
    str w0, [x26]
    str w0, [x27]
    mov w0, #255
    str w0, [x27, #4]
    mov w0, #-1
    str w0, [x26, #0x80] // all banked interrupts Group 1
    address x0, 0x0880ffff // virtual timer 27, PMU 23, all SGIs
    str w0, [x26, #0x100]
    mov x0, #0x501 // SCR: NS, HCE, RW
    msr scr_el3, x0
    mov x0, #(1 << 31)
    msr hcr_el2, x0
    mov x0, #3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    adr x0, el1
    msr elr_el3, x0
    mov x0, #0x3c5
    msr spsr_el3, x0
    eret
el1:
    adr x0, table
    msr ttbr0_el1, x0
    mov x0, #0xff00
    msr mair_el1, x0
    address x0, 0x80803520
    msr tcr_el1, x0
    isb
    tlbi vmalle1
    dsb sy
    isb
    mrs x0, sctlr_el1
    mov x1, #0x1005
    orr x0, x0, x1
    msr sctlr_el1, x0
    isb
    msr daifclr, #2
    mov x0, #1
    str x0, [x20, #24]
    mov x1, #20000
    str x1, [x20, #64]
    msr cntv_cval_el0, x1
    msr cntv_ctl_el0, x0
    wait_timer 3

    // Immediate expiry: zero TVAL, negative TVAL and CVAL now+0.5ns.
    mov x0, #2
    str x0, [x20, #24]
    msr cntv_tval_el0, xzr
    mov x0, #1
    msr cntv_ctl_el0, x0
    wait_timer 4
    mov x0, #-1
    msr cntv_tval_el0, x0
    mov x0, #1
    msr cntv_ctl_el0, x0
    wait_timer 5
    mrs x0, cntvct_el0
    add x0, x0, #1
    msr cntv_cval_el0, x0
    mov x0, #1
    msr cntv_ctl_el0, x0
    wait_timer 6

    // Mask across expiry, then unmask an already-expired counter.
    mov x0, #3
    msr cntv_ctl_el0, x0
    mrs x22, cntvct_el0
    add x22, x22, #2000
    msr cntv_cval_el0, x22
    add x0, x22, #1000
    bl wait_until
    ldr x1, [x20, #32]
    cmp x1, #6
    b.ne fail
    mov x0, #20
    bl log
    mov x0, #1
    msr cntv_ctl_el0, x0
    wait_timer 7
    // Disable before expiry; re-enable only after the deadline.
    mrs x22, cntvct_el0
    add x22, x22, #2000
    msr cntv_cval_el0, x22
    mov x0, #1
    msr cntv_ctl_el0, x0
    msr cntv_ctl_el0, xzr
    add x0, x22, #1000
    bl wait_until
    ldr x1, [x20, #32]
    cmp x1, #7
    b.ne fail
    mov x0, #21
    bl log
    mov x0, #1
    msr cntv_ctl_el0, x0
    wait_timer 8
    barrier 1

    // All other cores sleep in WFI before core 0 sends SGI 1.
    cbnz x19, sgi_target
    mrs x0, cntvct_el0
    add x0, x0, #2000
    bl wait_until
    address x0, 0x000e0001
    str w0, [x26, #0xf00]
    b sgi_done
sgi_target:
1:  ldr x0, [x20, #48]
    cbnz x0, sgi_done
    wfi
    b 1b
sgi_done:
    barrier 2

    // Consume the local event mailbox, then wake remote WFE waiters by SEV.
    sevl
    wfe
    cbnz x19, sev_target
    mrs x0, cntvct_el0
    add x0, x0, #2000
    bl wait_until
    sev
    b sev_done
sev_target:
    wfe
sev_done:
    mov x0, #30
    mov x1, x19
    bl log
    barrier 3

    // Independent per-core event streams wake WFE on CNTVCT bit 10.
    mov x0, #0xa4
    msr cntkctl_el1, x0
    sevl
    wfe
    wfe
    mov x0, #31
    mov x1, x19
    bl log
    msr cntkctl_el1, xzr
    barrier 4

    // Cycle-counter overflow, 32-bit counter, interrupt PPI 23.
    msr pmcr_el0, xzr
    msr pmccfiltr_el0, xzr
    mov x0, #-1
    msr pmovsclr_el0, x0
    mov x0, #(1 << 31)
    msr pmintenset_el1, x0
    msr pmcntenset_el0, x0
    address x0, 0xfffffff0
    msr pmccntr_el0, x0
    mov x0, #1
    msr pmcr_el0, x0
1:  ldr x0, [x20, #40]
    cbz x0, 1b
    barrier 5

    // All cores cache VA 0x60000000. Core 0 switches the leaf PTE, issues
    // VAE1IS + DSB ISH, then publishes completion. Remote final reads must
    // use the new mapping. Identity mappings and the page tables stay put.
    cbnz x19, mapping_ready
    address x0, 0x81000000
    mov x1, #0x111
    str x1, [x0]
    mov x1, #0x222
    str x1, [x0, #4096]
    dsb ish
mapping_ready:
    barrier 6
    address x23, 0x60000000
    ldr x1, [x23]
    cmp x1, #0x111
    b.ne fail
    barrier 7
    cbnz x19, remote_mapping
    adr x0, leaf
    address x1, 0x81001707
    str x1, [x0]
    dsb ishst
    mov x0, #0x60000
    tlbi vae1is, x0
    dsb ish
    isb
    mov x0, #40
    mov x1, #0
    bl log
    mov x0, #1
    str x0, [x20, #72]
    dsb ish
    b mapping_done
remote_mapping:
    address x2, 0x80200000
1:  ldr x1, [x23]
    ldr x0, [x2, #72]
    cbz x0, 1b
    dmb ish
mapping_done:
    ldr x1, [x23]
    cmp x1, #0x222
    b.ne fail
    mov x0, #41
    bl log
    barrier 8
    msr daifset, #2
    mov x0, #0x600d
    str x0, [x20]
    dsb sy
park:
    wfi
    b park
fail:
    mov x0, #0xbad
    str x0, [x20]
    mrs x1, esr_el1
    str x1, [x20, #80]
    mrs x1, elr_el1
    str x1, [x20, #88]
    dsb sy
    b park

// x0 deadline; clobbers x1.
wait_until:
    mrs x1, cntvct_el0
    cmp x1, x0
    b.lo wait_until
    ret
// Append {x0 code, CNTVCT, x1 argument}; clobbers x2-x5.
log:
    ldr x2, [x20, #16]
    cmp x2, #100
    b.hs fail
    add x3, x2, x2, lsl #1
    add x3, x20, x3, lsl #3
    add x3, x3, #0x100
    mrs x4, cntvct_el0
    stp x0, x4, [x3]
    str x1, [x3, #16]
    add x2, x2, #1
    str x2, [x20, #16]
    ret

.balign 2048
vectors:
.rept 4
    b fail
    .balign 128
    b irq
    .balign 128
    b fail
    .balign 128
    b fail
    .balign 128
.endr
irq:
    sub sp, sp, #176
    stp x0, x1, [sp]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x30, [sp, #144]
    ldr w17, [x27, #12]
    and x18, x17, #1023
    add x0, x18, #0x100
    ldr x1, [x20, #24]
    bl log
    cmp x18, #1023
    b.eq irq_restore
    cmp x18, #27
    b.eq timer_irq
    cmp x18, #23
    b.eq pmu_irq
    cmp x18, #1
    b.ne irq_finish
    ldr x0, [x20, #48]
    add x0, x0, #1
    str x0, [x20, #48]
    b irq_finish
timer_irq:
    ldr x0, [x20, #32]
    add x0, x0, #1
    str x0, [x20, #32]
    cmp x0, #3
    b.hs timer_off
    ldr x0, [x20, #64]
    mov x1, #20000
    add x0, x0, x1
    str x0, [x20, #64]
    msr cntv_cval_el0, x0
    b irq_finish
timer_off:
    msr cntv_ctl_el0, xzr
    b irq_finish
pmu_irq:
    msr pmcr_el0, xzr
    mov x0, #-1
    msr pmovsclr_el0, x0
    msr pmintenclr_el1, x0
    ldr x0, [x20, #40]
    add x0, x0, #1
    str x0, [x20, #40]
irq_finish:
    str w17, [x27, #16]
    dsb sy
irq_restore:
    ldp x0, x1, [sp]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x30, [sp, #144]
    add sp, sp, #176
    eret
.org 0x4000
table:
    .quad 0x0000000000000401
    .quad 0x0000000080005003
    .quad 0x0000000080000705
    .quad 0x00000000c0000705
.org 0x5800
    .quad 0x0000000080006003
.org 0x6000
leaf:
    .quad 0x0000000081000707
