# boot.S — first instructions of rvos. QEMU 'virt' enters here in M-mode on
# every hart with a0=hartid, a1=dtb. We keep only hart 0, clear .bss, set the
# stack, and jump to kmain().
.section .text.init
.global _start
_start:
    csrr    t0, mhartid
    bnez    t0, park              # secondary harts: idle forever

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
    call    mstart                # M-mode setup, then mret into S-mode

park:
    wfi
    j       park
