#pragma once
#include "riscv.h"

/* Just enough ELF64 to load a static executable: the header, and the program
   headers that say which byte ranges of the file become which pages of the
   address space. Section headers are a link-time concept and are not read.

   Deliberately shared by both sides but *parsed* only in user space — the
   kernel is never told what an ELF is. */

#define ELF_MAGIC 0x464C457FU        /* "\x7fELF" little-endian */

#define PT_LOAD   1
#define PF_X      1
#define PF_W      2
#define PF_R      4

struct elf64_ehdr {
    uint32 e_magic;
    uint8  e_class, e_data, e_ver, e_osabi, e_pad[8];
    uint16 e_type, e_machine;
    uint32 e_version;
    uint64 e_entry, e_phoff, e_shoff;
    uint32 e_flags;
    uint16 e_ehsize, e_phentsize, e_phnum;
    uint16 e_shentsize, e_shnum, e_shstrndx;
};

struct elf64_phdr {
    uint32 p_type, p_flags;
    uint64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

/* Argument block for SYS_VMLOAD: one PT_LOAD segment, described exactly as
   the program header describes it. The kernel allocates and maps memsz bytes
   at va, copies filesz of them from the caller, and leaves the remainder
   zero — which is where .bss comes from. */
struct vmload {
    uint64 va;
    uint64 src;        /* address in the *caller's* space */
    uint64 filesz;
    uint64 memsz;
    uint64 perm;       /* PTE_R / PTE_W / PTE_X; PTE_U is added by the kernel */
};
