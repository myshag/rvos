#pragma once
/* riscv.h — fixed-width types, M-mode CSR accessors, and MMIO addresses for
   the QEMU 'virt' machine. Educational rvos runs entirely in machine mode. */

typedef unsigned long  uint64;
typedef long           int64;
typedef unsigned int   uint32;
typedef int            int32;
typedef unsigned short uint16;
typedef unsigned char  uint8;

/* mstatus bits */
#define MSTATUS_MIE   (1UL << 3)
#define MSTATUS_MPIE  (1UL << 7)
#define MSTATUS_MPP_M (3UL << 11)   /* previous privilege = machine */

/* mie / mip bits */
#define MIE_MTIE      (1UL << 7)    /* machine timer interrupt enable */

/* mcause */
#define MCAUSE_INT    (1UL << 63)   /* set => interrupt, clear => exception */
#define IRQ_M_TIMER   7

/* CLINT (core-local interruptor): mtime + per-hart mtimecmp */
#define CLINT_BASE        0x02000000UL
#define CLINT_MTIMECMP(h) (CLINT_BASE + 0x4000 + 8 * (h))
#define CLINT_MTIME       (CLINT_BASE + 0xBFF8)

#define R(csr) ({ uint64 x; __asm__ volatile("csrr %0, " csr : "=r"(x)); x; })
#define W(csr, v) __asm__ volatile("csrw " csr ", %0" :: "r"((uint64)(v)))

static inline uint64 r_mhartid(void) { return R("mhartid"); }
static inline uint64 r_mstatus(void) { return R("mstatus"); }
static inline void   w_mstatus(uint64 v) { W("mstatus", v); }
static inline void   w_mtvec(uint64 v)   { W("mtvec", v); }
static inline uint64 r_mie(void)     { return R("mie"); }
static inline void   w_mie(uint64 v)     { W("mie", v); }
static inline uint64 r_mcause(void)  { return R("mcause"); }
static inline uint64 r_mepc(void)    { return R("mepc"); }
static inline uint64 r_mtval(void)   { return R("mtval"); }

static inline void mmio_w64(uint64 addr, uint64 v) { *(volatile uint64 *)addr = v; }
static inline uint64 mmio_r64(uint64 addr)         { return *(volatile uint64 *)addr; }
