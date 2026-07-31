#pragma once
#include "lib.h"
#include "elf.h"

/* spawn.h — starting a program, from a program that is itself on the disk.

   The README has said for several stages that a program loaded from the disk
   cannot start another, "because spawn lives in the shared user text of the
   kernel image". That sentence was true and the conclusion drawn from it was
   wrong. What cannot be reached is the *function*: user programs share their
   text, and a disk program is not linked against that text. The three
   syscalls underneath it were never restricted at all —

       SYS_NEWTASK   an empty address space          -> task id
       SYS_VMLOAD    one PT_LOAD segment into pages
       SYS_START     an entry point, and let it run

   — and they are deliberately unguarded, as the comment beside them in
   syscall.h says: any task may build another. Real systems put a capability
   in front of exactly these three.

   So this is the same answer lib.h gives to not having a libc: carry your own
   copy. It is the loader from src/loader.c, forty lines of it, reading
   through the ordinary filesystem interface and allocating with the ordinary
   allocator. Nothing here is privileged and nothing here is new; what is new
   is only that the file browser can now include it.

   The child inherits its parent's namespace, which is what makes this useful
   and what makes it dangerous in a full-screen program: /dev/console means
   the same terminal for both, so the caller has to get off the screen first
   and put it back afterwards. */

/* The whole file, in a buffer its own size. The size is not asked for in
   advance — doubling works against every server, including one at the far end
   of a connection that may not answer that question. */
static inline char *elf_slurp(const char *path, int *lenout)
{
    int fd = vfs_open(path);
    if (fd < 0)
        return 0;

    unsigned long cap = 8192, len = 0;
    char *buf = malloc(cap);
    while (buf) {
        if (len == cap) {
            char *bigger = realloc(buf, cap * 2);
            if (!bigger) { free(buf); buf = 0; break; }
            buf = bigger;
            cap *= 2;
        }
        int n = vfs_read(fd, buf + len, (int)(cap - len));
        if (n <= 0)
            break;
        len += (unsigned long)n;
    }
    vfs_close(fd);
    if (!buf)
        return 0;
    *lenout = (int)len;
    return buf;
}

/* Lay out argv the way a C program expects to find it: pointers first, then
   the strings. The pointers name addresses in the *new* task's space, which
   is why they are computed from ARG_BASE and not from where this is being
   assembled. */
static inline int elf_args(char *blk, int argc, char *const argv[])
{
    unsigned long *ptrs = (unsigned long *)blk;
    int off = (argc + 1) * 8;

    for (int i = 0; i < argc; i++) {
        int l = plen(argv[i]) + 1;
        if (off + l > ARG_MAX)
            return -1;
        pcpy(blk + off, argv[i], l);
        ptrs[i] = ARG_BASE + (unsigned long)off;
        off += l;
    }
    ptrs[argc] = 0;
    return off;
}

/* Load and start `path`. Returns the child's task id, or -1. */
static inline int prun(const char *path, int argc, char *const argv[])
{
    int n = 0;
    char *img = elf_slurp(path, &n);
    if (!img || n <= 0) {
        free(img);
        return -1;
    }

    struct elf64_ehdr *eh = (struct elf64_ehdr *)img;
    int tid = -1;
    if (eh->e_magic != ELF_MAGIC)
        goto done;

    tid = sys_newtask(path);
    if (tid < 0)
        goto done;

    for (int i = 0; i < eh->e_phnum; i++) {
        struct elf64_phdr *ph =
            (struct elf64_phdr *)(img + eh->e_phoff +
                                  (unsigned long)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;
        if (ph->p_offset + ph->p_filesz > (unsigned long)n) {
            tid = -1;
            goto done;
        }
        struct vmload seg;
        seg.va     = ph->p_vaddr;
        seg.src    = (unsigned long)(img + ph->p_offset);
        seg.filesz = ph->p_filesz;
        seg.memsz  = ph->p_memsz;
        seg.perm   = ((ph->p_flags & PF_R) ? PTE_R : 0) |
                     ((ph->p_flags & PF_W) ? PTE_W : 0) |
                     ((ph->p_flags & PF_X) ? PTE_X : 0);
        if (sys_vmload(tid, &seg) < 0) {
            tid = -1;
            goto done;
        }
    }

    struct startinfo si;
    si.entry = eh->e_entry;
    si.sp    = ARG_BASE - 32;           /* below the block, 16-byte aligned */
    si.a0    = (unsigned long)argc;
    si.a1    = ARG_BASE;

    if (argc > 0) {
        char blk[ARG_MAX];
        int len = elf_args(blk, argc, argv);
        if (len < 0) { tid = -1; goto done; }
        struct vmload as;
        as.va = ARG_BASE;
        as.src = (unsigned long)blk;
        as.filesz = (unsigned long)len;
        as.memsz  = (unsigned long)len;
        as.perm = PTE_R | PTE_W;
        if (sys_vmload(tid, &as) < 0) { tid = -1; goto done; }
    }

    if (sys_start(tid, &si) < 0)
        tid = -1;

done:
    free(img);                  /* the pages are in the child now, or nowhere */
    return tid;
}
