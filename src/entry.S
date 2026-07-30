# trap.S — machine-mode trap entry. mscratch holds &current->ctx while a task
# runs. On trap we save all GPRs + mepc/mstatus into that context, switch to a
# private trap stack, call the C handler (which may pick a new `current`), then
# restore the (possibly different) current context and mret.
#
# Context layout mirrors struct context: x[i] at i*8, mepc at 256, mstatus 264.
.section .text
.align 4
.global _mtrap
_mtrap:
    csrrw   t6, mscratch, t6        # t6 = &cur.ctx ; mscratch = original t6

    sd      x1,  8(t6)
    sd      x2,  16(t6)
    sd      x3,  24(t6)
    sd      x4,  32(t6)
    sd      x5,  40(t6)
    sd      x6,  48(t6)
    sd      x7,  56(t6)
    sd      x8,  64(t6)
    sd      x9,  72(t6)
    sd      x10, 80(t6)
    sd      x11, 88(t6)
    sd      x12, 96(t6)
    sd      x13, 104(t6)
    sd      x14, 112(t6)
    sd      x15, 120(t6)
    sd      x16, 128(t6)
    sd      x17, 136(t6)
    sd      x18, 144(t6)
    sd      x19, 152(t6)
    sd      x20, 160(t6)
    sd      x21, 168(t6)
    sd      x22, 176(t6)
    sd      x23, 184(t6)
    sd      x24, 192(t6)
    sd      x25, 200(t6)
    sd      x26, 208(t6)
    sd      x27, 216(t6)
    sd      x28, 224(t6)
    sd      x29, 232(t6)
    sd      x30, 240(t6)
    csrr    t5, mscratch            # original x31 (t6)
    sd      t5, 248(t6)
    csrr    t5, mepc
    sd      t5, 256(t6)
    csrr    t5, mstatus
    sd      t5, 264(t6)

    la      sp, _trap_stack_top
    call    mtrap_handler
    # fall through to restore whatever `current` now is

.global _ret_to_task
_ret_to_task:
    la      t6, current
    ld      t6, 0(t6)               # t6 = current == &current->ctx (offset 0)
    csrw    mscratch, t6

    ld      t5, 264(t6)
    csrw    mstatus, t5
    ld      t5, 256(t6)
    csrw    mepc, t5

    ld      x1,  8(t6)
    ld      x2,  16(t6)
    ld      x3,  24(t6)
    ld      x4,  32(t6)
    ld      x5,  40(t6)
    ld      x6,  48(t6)
    ld      x7,  56(t6)
    ld      x8,  64(t6)
    ld      x9,  72(t6)
    ld      x10, 80(t6)
    ld      x11, 88(t6)
    ld      x12, 96(t6)
    ld      x13, 104(t6)
    ld      x14, 112(t6)
    ld      x15, 120(t6)
    ld      x16, 128(t6)
    ld      x17, 136(t6)
    ld      x18, 144(t6)
    ld      x19, 152(t6)
    ld      x20, 160(t6)
    ld      x21, 168(t6)
    ld      x22, 176(t6)
    ld      x23, 184(t6)
    ld      x24, 192(t6)
    ld      x25, 200(t6)
    ld      x26, 208(t6)
    ld      x27, 216(t6)
    ld      x28, 224(t6)
    ld      x29, 232(t6)
    ld      x30, 240(t6)
    ld      x31, 248(t6)            # t6 restored last (it is our base)
    mret

.section .bss
.align 16
    .space 4096
_trap_stack_top:
