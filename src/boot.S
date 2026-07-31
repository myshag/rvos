# boot.S — first instructions of rvos. QEMU 'virt' enters here in M-mode on
# every hart with a0=hartid, a1=dtb. We keep only hart 0, clear .bss, set the
# stack, and jump to kmain().
.section .text.init
.global _start
_start:
    csrr    t0, mhartid
    bnez    t0, park              # secondary harts: idle forever

    # The machine describes itself, and the description is in a1. Until now
    # this was thrown away and every address in the kernel was a constant that
    # happened to be right. Keep it in a callee-saved register across the bss
    # clear, and store it after — storing before would zero it.
    mv      s0, a1

    # zero .bss
    la      t0, __bss_start
    la      t1, __bss_end
1:
    bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b
2:
    la      sp, __stack_top       # set boot stack
    la      t0, dtb_pa
    sd      s0, 0(t0)             # after the clear, or it would be cleared
    call    mstart                # M-mode setup, then mret into S-mode

park:
    wfi
    j       park
