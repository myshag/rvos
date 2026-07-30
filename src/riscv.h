#pragma once
/* riscv.h — fixed-width types, CSR accessors and machine constants for the
   QEMU 'virt' board. rvos boots in M-mode, configures delegation and PMP,
   then spends its life in S-mode with Sv39 paging enabled. */

typedef unsigned long  uint64;
typedef long           int64;
typedef unsigned int   uint32;
typedef int            int32;
typedef unsigned short uint16;
typedef unsigned char  uint8;

/* ---- mstatus / sstatus ---------------------------------------------- */
#define MSTATUS_MIE     (1UL << 3)
#define MSTATUS_MPIE    (1UL << 7)
#define MSTATUS_MPP_M   (3UL << 11)
#define MSTATUS_MPP_S   (1UL << 11)
#define MSTATUS_MPP_MASK (3UL << 11)

#define SSTATUS_SIE     (1UL << 1)   /* S-mode interrupts enabled */
#define SSTATUS_SPIE    (1UL << 5)   /* prior SIE, restored by sret */
#define SSTATUS_SPP     (1UL << 8)   /* 1 => sret returns to S-mode */

/* ---- interrupt enables ---------------------------------------------- */
#define SIE_SSIE        (1UL << 1)
#define SIE_STIE        (1UL << 5)   /* supervisor timer */
#define SIE_SEIE        (1UL << 9)

/* ---- cause codes ----------------------------------------------------- */
#define CAUSE_INT           (1UL << 63)
#define IRQ_S_TIMER         5
#define EXC_ECALL_U         8        /* environment call from U-mode */
#define EXC_ECALL_S         9        /* environment call from S-mode */
#define EXC_INST_PAGE_FAULT 12
#define EXC_LOAD_PAGE_FAULT 13
#define EXC_STORE_PAGE_FAULT 15

/* ---- Sv39 ------------------------------------------------------------
   A virtual address is 39 bits: three 9-bit page-table indices plus a
   12-bit offset, sign-extended to 64. Each table is one 4 KiB page holding
   512 eight-byte PTEs. A PTE with R=W=X=0 points at the next table; any of
   R/W/X set makes it a leaf, and a leaf above level 0 is a superpage
   (2 MiB at level 1, 1 GiB at level 2). */
#define PGSIZE      4096UL
#define PGSHIFT     12
#define MEGASIZE    (2UL * 1024 * 1024)

#define PGROUNDUP(a)   (((a) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(a) ((a) & ~(PGSIZE - 1))

#define PTE_V (1UL << 0)
#define PTE_R (1UL << 1)
#define PTE_W (1UL << 2)
#define PTE_X (1UL << 3)
#define PTE_U (1UL << 4)
#define PTE_G (1UL << 5)
#define PTE_A (1UL << 6)
#define PTE_D (1UL << 7)

/* PPN sits at bits 53:10 of a PTE, i.e. the address shifted right 12 then
   left 10 — the low 10 bits being the flags. */
#define PA2PTE(pa)     ((((uint64)(pa)) >> 12) << 10)
#define PTE2PA(pte)    ((((uint64)(pte)) >> 10) << 12)
#define PTE_FLAGS(pte) ((pte) & 0x3FFUL)
#define PTE_IS_LEAF(pte) ((pte) & (PTE_R | PTE_W | PTE_X))

/* index of `va` at page-table `level` (0 = leaf level) */
#define PX(level, va) ((((uint64)(va)) >> (PGSHIFT + 9 * (level))) & 0x1FFUL)

#define SATP_SV39     (8UL << 60)
#define MAKE_SATP(pt) (SATP_SV39 | (((uint64)(pt)) >> 12))

typedef uint64  pte_t;
typedef uint64 *pagetable_t;

/* ---- MMIO ------------------------------------------------------------ */
#define CLINT_BASE        0x02000000UL
#define CLINT_MTIMECMP(h) (CLINT_BASE + 0x4000 + 8 * (h))
#define CLINT_MTIME       (CLINT_BASE + 0xBFF8)
#define UART_BASE_PA      0x10000000UL

#define RAM_BASE          0x80000000UL
#define RAM_TOP           0x88000000UL   /* qemu virt default: 128 MiB */
#define DISK_PA           0x84000000UL   /* FAT16 image loaded here */
#define DISK_SIZE         (8UL * 1024 * 1024)
#define PMM_TOP           0x83000000UL   /* free pages end before the disk */

#define R(csr) ({ uint64 x; __asm__ volatile("csrr %0, " csr : "=r"(x)); x; })
#define W(csr, v) __asm__ volatile("csrw " csr ", %0" :: "r"((uint64)(v)))

static inline uint64 r_mhartid(void)  { return R("mhartid"); }
static inline uint64 r_mstatus(void)  { return R("mstatus"); }
static inline void   w_mstatus(uint64 v) { W("mstatus", v); }
static inline void   w_mepc(uint64 v)    { W("mepc", v); }
static inline void   w_medeleg(uint64 v) { W("medeleg", v); }
static inline void   w_mideleg(uint64 v) { W("mideleg", v); }
static inline void   w_mcounteren(uint64 v) { W("mcounteren", v); }
static inline void   w_pmpaddr0(uint64 v)   { W("pmpaddr0", v); }
static inline void   w_pmpcfg0(uint64 v)    { W("pmpcfg0", v); }
/* menvcfg (0x30a): bit 63 STCE enables the Sstc stimecmp CSR for S-mode */
static inline uint64 r_menvcfg(void)
{ uint64 x; __asm__ volatile("csrr %0, 0x30a" : "=r"(x)); return x; }
static inline void   w_menvcfg(uint64 v)
{ __asm__ volatile("csrw 0x30a, %0" :: "r"(v)); }

static inline uint64 r_sstatus(void)  { return R("sstatus"); }
static inline void   w_sstatus(uint64 v) { W("sstatus", v); }
static inline void   w_stvec(uint64 v)   { W("stvec", v); }
static inline uint64 r_sie(void)      { return R("sie"); }
static inline void   w_sie(uint64 v)     { W("sie", v); }
static inline uint64 r_scause(void)   { return R("scause"); }
static inline uint64 r_sepc(void)     { return R("sepc"); }
static inline uint64 r_stval(void)    { return R("stval"); }
static inline uint64 r_satp(void)     { return R("satp"); }
static inline void   w_satp(uint64 v)    { W("satp", v); }

/* stimecmp (0x14d), Sstc: S-mode's own timer compare register. */
static inline void w_stimecmp(uint64 v)
{ __asm__ volatile("csrw 0x14d, %0" :: "r"(v)); }
static inline uint64 r_time(void)
{ uint64 x; __asm__ volatile("rdtime %0" : "=r"(x)); return x; }

static inline void sfence_vma(void)
{ __asm__ volatile("sfence.vma zero, zero"); }

static inline void mmio_w64(uint64 addr, uint64 v) { *(volatile uint64 *)addr = v; }
static inline uint64 mmio_r64(uint64 addr)         { return *(volatile uint64 *)addr; }
