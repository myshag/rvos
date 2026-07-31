/* loader.c — an ELF loader that is not part of the kernel.

   It reads the executable through the ordinary filesystem interface, parses
   the program headers itself, and then asks the kernel for the few things it
   cannot do unprivileged: an empty address space, pages filled from a buffer,
   and a starting register set. The kernel is never told what ELF is — it is
   handed the numbers a program header already contains.

   spawn() used to take a scratch buffer from its caller, because user
   programs share their code but not their data: the shell calls this very
   function, and a static buffer here would be at an address mapped into the
   loader's task and not into the shell's. That was a real constraint answered
   with a fixed number — ELFMAX, 16 KiB and then 32 KiB, and a program that
   outgrew it could not be run at all.

   It allocates now. The allocator has the same constraint and answers it
   differently: its state lives at a fixed address inside the caller's own
   address space rather than in a variable, so one copy of malloc works for
   whoever is running. The buffer is the size of the file. */
#include "syscall.h"
#include "vfs.h"
#include "ulib.h"
#include "elf.h"
#include "malloc.h"

/* Slurp a whole file: the fs server only reads forward, and program headers
   live at an arbitrary offset. A grown-up loader would seek, or map the file.

   The size is not asked for in advance. It could be — the interface has an
   ioctl for it — but doubling works against every server, including one at
   the far end of a TCP connection that may not answer that question, and the
   copy it costs is one memcpy against a disk read. */
static char *read_whole(const char *path, int *lenout)
{
    int fd = vfs_open(path);
    if (fd < 0)
        return 0;

    unsigned long cap = 8192, len = 0;
    char *buf = malloc(cap);
    while (buf) {
        if (len == cap) {
            char *bigger = realloc(buf, cap * 2);
            if (!bigger) {
                free(buf);
                buf = 0;
                break;
            }
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

/* Lay out argv the way a C program expects to find it: an array of pointers
   terminated by NULL, followed by the strings themselves. The pointers have
   to name addresses in the *new* task's space, which is why they are computed
   from ARG_BASE rather than from where we are assembling the block. */
static int build_args(char *blk, int argc, char *const argv[])
{
    uint64 *ptrs = (uint64 *)blk;
    int off = (argc + 1) * 8;

    for (int i = 0; i < argc; i++) {
        int l = ustrlen(argv[i]) + 1;
        if (off + l > ARG_MAX)
            return -1;
        umemcpy(blk + off, argv[i], (unsigned long)l);
        ptrs[i] = ARG_BASE + (uint64)off;
        off += l;
    }
    ptrs[argc] = 0;
    return off;
}

/* Load `path` into a new task and start it. Returns the task id, or -1.

   Nothing here arranges where the child's output goes, and nothing needs to:
   it inherits its parent's namespace, and if /dev/console means a connection
   there, it means one here. This used to take a connection number and attach
   it to the task id in the window before SYS_START — a mechanism that existed
   only because a name could not be bound to another name. It can now. */
static int spawn_image(const char *path, char *scratch, int n,
                       int argc, char *const argv[])
{
    struct elf64_ehdr *eh = (struct elf64_ehdr *)scratch;
    if (eh->e_magic != ELF_MAGIC)
        return -1;

    int tid = sys_newtask(path);
    if (tid < 0)
        return -1;

    for (int i = 0; i < eh->e_phnum; i++) {
        struct elf64_phdr *ph =
            (struct elf64_phdr *)(scratch + eh->e_phoff +
                                  (unsigned long)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;
        if (ph->p_offset + ph->p_filesz > (unsigned long)n)
            return -1;

        struct vmload seg;
        seg.va     = ph->p_vaddr;
        seg.src    = (unsigned long)(scratch + ph->p_offset);
        seg.filesz = ph->p_filesz;
        seg.memsz  = ph->p_memsz;
        seg.perm   = ((ph->p_flags & PF_R) ? PTE_R : 0) |
                     ((ph->p_flags & PF_W) ? PTE_W : 0) |
                     ((ph->p_flags & PF_X) ? PTE_X : 0);
        if (sys_vmload(tid, &seg) < 0)
            return -1;
    }

    /* The argument block goes into the stack the kernel already mapped, so
       this vmload only copies. */
    struct startinfo si;
    si.entry = eh->e_entry;
    si.sp    = ARG_BASE - 32;          /* below the block, 16-byte aligned */
    si.a0    = (uint64)argc;
    si.a1    = ARG_BASE;

    if (argc > 0) {
        char blk[ARG_MAX];
        int len = build_args(blk, argc, argv);
        if (len < 0)
            return -1;
        struct vmload as;
        as.va = ARG_BASE; as.src = (unsigned long)blk;
        as.filesz = (uint64)len; as.memsz = (uint64)len;
        as.perm = PTE_R | PTE_W;
        if (sys_vmload(tid, &as) < 0)
            return -1;
    }

    if (sys_start(tid, &si) < 0)
        return -1;
    return tid;
}

int spawn(const char *path, int argc, char *const argv[])
{
    int n = 0;
    char *img = read_whole(path, &n);
    if (!img)
        return -1;
    int tid = n > 0 ? spawn_image(path, img, n, argc, argv) : -1;
    free(img);                  /* the pages are in the child now, or nowhere */
    return tid;
}

/* ---- the boot-time demo: load one program and report what it did -------- */


static void say_num(const char *label, unsigned long v, const char *tail)
{
    char n[24];
    int k = uutoa(v, n);
    n[k] = 0;
    uputs(label); uputs(n); uputs(tail);
}

void loader_main(void)
{
    unsigned long go;
    sys_recv(&go, (int)sizeof(go));

    uputs("\n--- loader (U-mode) ------------------------------------\n");
    {   /* three tasks have faulted and been retired by now */
        int mem[2] = { 0, 0 };
        sys_meminfo(mem);
        say_num("  free pages ", (unsigned long)mem[0],
                " — reclaimed from the tasks the MMU stopped\n");
    }

    char *argv[3];
    argv[0] = (char *)"/BIN/HELLO.ELF";
    argv[1] = (char *)"alpha";
    argv[2] = (char *)"beta";

    uputs("$ exec /BIN/HELLO.ELF alpha beta\n");
    int tid = spawn("/BIN/HELLO.ELF", 3, argv);
    if (tid < 0)
        uputs("  exec failed\n");
    else
        say_num("  started as task ", (unsigned long)tid, "\n");

    /* Finished. Blocking rather than spinning: a task that yields for ever is
       not idle, it is busy, and every message in the system pays for it. */
    unsigned long done;
    sys_recv(&done, (int)sizeof(done));
}
