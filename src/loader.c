/* loader.c — an ELF loader that is not part of the kernel.

   It reads the executable through the ordinary filesystem interface, parses
   the program headers itself, and then asks the kernel for three things it
   cannot do unprivileged: an empty address space, pages filled from a buffer,
   and a starting instruction. The kernel is never told what ELF is — it is
   handed the numbers a program header already contains.

   Fixed load address, so no relocation: the segments go exactly where
   p_vaddr says. */
#include "syscall.h"
#include "vfs.h"
#include "ulib.h"
#include "elf.h"

#define ELFMAX 8192
static char elfbuf[ELFMAX];        /* this program's private data region */

static void say(const char *s) { uputs(s); }

static void say_num(const char *label, unsigned long v, const char *tail)
{
    char n[24];
    int k = uutoa(v, n);
    n[k] = 0;
    say(label); say(n); say(tail);
}

static void say_hex(const char *label, unsigned long v, const char *tail)
{
    char n[24];
    int k = uxtoa(v, n);
    n[k] = 0;
    say(label); say("0x"); say(n); say(tail);
}

/* Slurp the whole file: our fs server only reads forward, and program headers
   live at an arbitrary offset. Small executables make this reasonable; a real
   loader would seek, or map the file. */
static int read_file(const char *path, char *dst, int cap)
{
    int fd = vfs_open(path);
    if (fd < 0)
        return -1;
    int total = 0;
    for (;;) {
        int n = vfs_read(fd, dst + total, cap - total);
        if (n <= 0)
            break;
        total += n;
        if (total >= cap)
            break;
    }
    vfs_close(fd);
    return total;
}

void loader_main(void)
{
    unsigned long go;
    sys_recv(&go, (int)sizeof(go));

    say("\n--- loader (U-mode) ------------------------------------\n");
    say("$ exec /HELLO.ELF\n");

    int n = read_file("/HELLO.ELF", elfbuf, ELFMAX);
    if (n <= 0) {
        say("  cannot read /HELLO.ELF\n");
        goto done;
    }
    say_num("  read ", (unsigned long)n, " bytes from the fs server\n");

    struct elf64_ehdr *eh = (struct elf64_ehdr *)elfbuf;
    if (eh->e_magic != ELF_MAGIC) {
        say("  not an ELF\n");
        goto done;
    }
    say_hex("  entry ", eh->e_entry, "");
    say_num(", program headers: ", (unsigned long)eh->e_phnum, "\n");

    int tid = sys_newtask("hello");
    if (tid < 0) {
        say("  no free task slot\n");
        goto done;
    }
    say_num("  new address space for task ", (unsigned long)tid, "\n");

    for (int i = 0; i < eh->e_phnum; i++) {
        struct elf64_phdr *ph =
            (struct elf64_phdr *)(elfbuf + eh->e_phoff +
                                  (unsigned long)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;
        if (ph->p_offset + ph->p_filesz > (unsigned long)n) {
            say("  segment runs past the end of the file\n");
            goto done;
        }

        struct vmload seg;
        seg.va     = ph->p_vaddr;
        seg.src    = (unsigned long)(elfbuf + ph->p_offset);
        seg.filesz = ph->p_filesz;
        seg.memsz  = ph->p_memsz;
        seg.perm   = ((ph->p_flags & PF_R) ? PTE_R : 0) |
                     ((ph->p_flags & PF_W) ? PTE_W : 0) |
                     ((ph->p_flags & PF_X) ? PTE_X : 0);

        say_hex("  segment -> ", ph->p_vaddr, " ");
        say_num("filesz ", ph->p_filesz, " ");
        say_num("memsz ", ph->p_memsz, " ");
        say((ph->p_flags & PF_X) ? "r-x\n" : "rw-\n");

        if (sys_vmload(tid, &seg) < 0) {
            say("  vmload failed\n");
            goto done;
        }
    }

    if (sys_start(tid, eh->e_entry) < 0) {
        say("  start failed\n");
        goto done;
    }
    say("  started\n");

done:
    for (;;)
        _ecall1(SYS_YIELD, 0);
}
