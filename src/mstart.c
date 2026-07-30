/* mstart.c — the only code that runs in machine mode. Its whole job is to
   hand the machine over to S-mode, because that is where paging exists:
   M-mode ignores satp entirely and treats every address as physical.

   Four things must be arranged before the handover, and forgetting any of
   them wedges the machine in a way that is hard to debug:

     1. delegation   — otherwise every S-mode trap escalates to M-mode, where
                       we no longer have a handler installed;
     2. PMP          — physical memory protection defaults to "S-mode may
                       touch nothing", so an unconfigured PMP faults on the
                       first instruction after mret;
     3. mcounteren   — lets S-mode execute rdtime, which our timer needs;
     4. menvcfg.STCE — enables Sstc, i.e. the stimecmp CSR. Without it S-mode
                       has no timer of its own: CLINT's mtimecmp is M-mode
                       only and the machine timer interrupt is not delegable.
*/
#include "riscv.h"

void smain(void);

void mstart(void)
{
    /* mret will drop us to S-mode... */
    uint64 x = r_mstatus();
    x &= ~MSTATUS_MPP_MASK;
    x |= MSTATUS_MPP_S;
    w_mstatus(x);

    /* ...landing here, with translation still off (smain turns it on). */
    w_mepc((uint64)smain);
    w_satp(0);

    /* Route traps to S-mode instead of escalating them to M-mode. */
    w_medeleg(0xffff);
    w_mideleg(0xffff);
    w_sie(r_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

    /* TOR entry spanning all of physical memory, RWX — without this S-mode
       has no access to anything at all. */
    w_pmpaddr0(0x3fffffffffffffUL);
    w_pmpcfg0(0xf);

    w_mcounteren(0xffffffff);                  /* rdtime from S-mode */
    w_menvcfg(r_menvcfg() | (1UL << 63));      /* STCE: enable stimecmp */

    __asm__ volatile("mret");
}
