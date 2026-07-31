/* kmain.c — Stage 9: address-space isolation.

   Every task now owns an Sv39 page table. They share the kernel image and the
   console, because kernel code must run when a trap lands, but each stack is
   private and the free-page arena is mapped nowhere. The FAT16 image goes
   into the filesystem server's space alone, so "only the fs module can reach
   the disk" stops being a convention and becomes something the hardware
   enforces — as the snooper task finds out.

   The visible cost is in IPC: a message can no longer be a pointer, since an
   address means nothing in another address space, so the kernel copies. */
#include "uart.h"
#include "task.h"
#include "syscall.h"
#include "vfs.h"
#include "servers.h"
#include "pmm.h"
#include "vm.h"
#include "plic.h"
#include "virtio.h"
#include "util.h"

/* Read a whole file through the interface. It arrives in VFS_DATA_MAX-sized
   chunks now — each one a copy the kernel made between two address spaces. */
static void cat(const char *path, const char *label)
{
    int fd = vfs_open(path);
    if (fd < 0) {
        kprintf("  cat %s: no such file\n", path);
        return;
    }
    kprintf("%s\n", label);
    for (;;) {
        char buf[VFS_DATA_MAX + 1];
        int n = vfs_read(fd, buf, VFS_DATA_MAX);
        if (n <= 0)
            break;
        buf[n] = 0;
        kprintf("%s", buf);
    }
    vfs_close(fd);
}

static void say(const char *what)
{
    int fd = vfs_open("/dev/console");
    if (fd < 0) {
        kprintf("  (/dev/console unreachable)\n");
        return;
    }
    vfs_write(fd, what, (int)strlen(what));
    vfs_close(fd);
}

/* ---- namespace demo: same code, private namespace -------------------- */
static void sandbox(void)
{
    uint64 m;
    sys_recv(&m, (int)sizeof(m));

    kprintf("\n--- sandbox (task %d) ----------------------------------\n",
            SANDBOX_TASK_ID);
    vfs_ns_clone();
    vfs_bind("/dev/console", NULL_TASK_ID);
    kprintf("$ vfs_ns_clone(); bind /dev/console -> null\n");
    say("  THIS LINE SHOULD NEVER APPEAR\n");
    kprintf("  write(/dev/console) printed nothing: rebound to null\n");

    /* Same path, same code the shell ran — different answer, because the
       page table reported is whichever task is asking. */
    cat("/proc/pagetable", "\n$ cat /proc/pagetable  -- sandbox's own space");

    sys_send(SHELL_TASK_ID, &m, (int)sizeof(m));
    for (;;)
        yield();
}

/* ---- isolation demo: reach for another module's memory --------------- */
static void snooper(void)
{
    uint64 m;
    sys_recv(&m, (int)sizeof(m));

    kprintf("\n--- snooper (task %d) ----------------------------------\n",
            SNOOPER_TASK_ID);
    kprintf("$ read *(char*)0x%lx  -- FAT16 image, mapped only into fs\n",
            DISK_PA);
    volatile char *disk = (volatile char *)DISK_PA;
    char c = *disk;                  /* load page fault: task is retired */

    kprintf("  UNREACHABLE: read 0x%x — isolation failed\n", (int)c);
    for (;;)
        yield();
}

/* ---- shell ------------------------------------------------------------ */
static void shell(void)
{
    kprintf("\n--- shell (task %d) ------------------------------------\n",
            SHELL_TASK_ID);
    cat("/", "$ cat /                -- fs reaches the disk on our behalf");
    say("  write(/dev/console) routed to the console module\n");

    cat("/proc/pagetable", "\n$ cat /proc/pagetable  -- shell's own space");

    uint64 m = 0;
    sys_send(SANDBOX_TASK_ID, &m, (int)sizeof(m));
    sys_recv(&m, (int)sizeof(m));

    /* Neither of these replies: both are about to be stopped by the MMU. */
    sys_send(SNOOPER_TASK_ID, &m, (int)sizeof(m));
    sys_send(USER_TASK_ID,    &m, (int)sizeof(m));
    sys_send(PEEKER_TASK_ID,  &m, (int)sizeof(m));
    sys_send(LOADER_TASK_ID,  &m, (int)sizeof(m));
    sys_send(SH_TASK_ID,      &m, (int)sizeof(m));
    sys_send(NET_TASK_ID,     &m, (int)sizeof(m));
    sys_send(RSH_TASK_ID,     &m, (int)sizeof(m));

    for (;;)
        yield();
}

void user_main(void);        /* user.c — runs with the U bit set */
void peeker_main(void);      /* user.c — probes another server's memory */
void loader_main(void);      /* loader.c — reads an ELF and spawns it */
void sh_main(void);          /* sh.c — reads a command line and runs it */
void net_server(void);       /* srv_net.c — virtio-net, in user mode */
void rsh_main(void);         /* srv_rsh.c — the same shell, over TCP */

/* Always runnable, so the scheduler never runs out of candidates — without
   it, retiring a faulting task while everyone else is blocked left nothing to
   switch to. */
static void idle(void)
{
    for (;;)
        yield();
}

/* ---- supervisor-mode entry ------------------------------------------- */
void smain(void)
{
    uart_init();
    kprintf("\n=============================================\n");
    kprintf("  rvos — educational RISC-V microkernel\n");
    kprintf("  stage 11: the servers are user programs\n");
    kprintf("=============================================\n");

    /* Clear each user program's .bss, and only its .bss: boot.S knows about
       the kernel's alone, and the .data next door arrives initialised. */
    {
        extern char __ufs_bss_start[],   __ufs_bss_end[];
        extern char __uproc_bss_start[], __uproc_bss_end[];
        extern char __uload_bss_start[], __uload_bss_end[];
        extern char __ush_bss_start[],   __ush_bss_end[];
        extern char __ursh_bss_start[],  __ursh_bss_end[];
        extern char __unet_bss_start[],  __unet_bss_end[];
        extern char __umisc_bss_start[], __umisc_bss_end[];
        char *bss[][2] = {
            { __ufs_bss_start,   __ufs_bss_end   },
            { __uproc_bss_start, __uproc_bss_end },
            { __uload_bss_start, __uload_bss_end },
            { __ush_bss_start,   __ush_bss_end   },
            { __ursh_bss_start,  __ursh_bss_end  },
            { __unet_bss_start,  __unet_bss_end  },
            { __umisc_bss_start, __umisc_bss_end },
        };
        for (unsigned i = 0; i < sizeof(bss) / sizeof(bss[0]); i++)
            memset(bss[i][0], 0, (size_t)(bss[i][1] - bss[i][0]));
    }

    pmm_init();
    vm_init();
    kprintf("[boot] Sv39 on, kernel satp=0x%lx, %d pages free\n",
            r_satp(), pmm_free_count());

    trap_init();
    timer_init();
    /* Let user mode read the clock. A protocol stack needs to know not just
       "wake me in 300 ms" but *what time it is now* — several connections have
       deadlines and only one alarm exists to serve them, so the nearest has to
       be computed. Making that a syscall would put a trap on a path taken per
       packet; one CSR bit puts it in a single instruction instead. */
    w_scounteren(SCOUNTEREN_TM);
    plic_init();
    plic_init_hart();
    kprintf("[boot] PLIC up; UART irq %d routed to S-mode\n", UART0_IRQ);

    extern char __ufs_start[], __ufs_end[];
    extern char __uproc_start[], __uproc_end[];
    extern char __umisc_start[], __umisc_end[];
    extern char __uload_start[], __uload_end[];
    extern char __ush_start[], __ush_end[];
    extern char __ursh_start[], __ursh_end[];
    extern char __unet_start[], __unet_end[];

    /* The system services are user programs now. */
    struct task *fs =
        task_create_user("fs", fs_server, __ufs_start, __ufs_end);        /* 0 */
    struct task *con =
        task_create_user("console", console_server,
                         __umisc_start, __umisc_end);                     /* 1 */
    task_create_user("proc", proc_server, __uproc_start, __uproc_end);    /* 2 */
    task_create_user("null", null_server, __umisc_start, __umisc_end);    /* 3 */

    task_create("shell",   shell);                    /* 4 */
    task_create("sandbox", sandbox);                  /* 5 */
    task_create("snooper", snooper);                  /* 6 */
    task_create_user("user", user_main,
                     __umisc_start, __umisc_end);     /* 7 */
    task_create_user("peeker", peeker_main,
                     __umisc_start, __umisc_end);     /* 8 */
    task_create_user("loader", loader_main,
                     __uload_start, __uload_end);     /* 9 */
    task_create_user("sh", sh_main, __ush_start, __ush_end);  /* 10 */
    struct task *net =
        task_create_user("net", net_server, __unet_start, __unet_end); /* 11 */
    task_create("idle",    idle);                     /* 12 */
    /* The shell you reach over the network. It needs spawn(), which lives
       in the shared user text linked into this image, so it is a task here
       rather than a program on the disk. */
    task_create_user("rsh", rsh_main, __ursh_start, __ursh_end); /* 13 */

    /* Devices are handed to their driver and to nobody else — with the U bit,
       so the driver reaches them from user mode without the kernel on the
       data path at all. */
    vm_map_at(fs->pt,  DISK_PA, DISK_PA, DISK_SIZE, PTE_R | PTE_U, 1);
    vm_map_at(con->pt, UART_BASE_PA, UART_BASE_PA, PGSIZE,
              PTE_R | PTE_W | PTE_U, 0);
    /* The virtio transports go to the network driver alone. */
    vm_map_at(net->pt, VIRTIO_MMIO_BASE, VIRTIO_MMIO_BASE,
              VIRTIO_MMIO_STRIDE * VIRTIO_MMIO_SLOTS, PTE_R | PTE_W | PTE_U, 0);

    vfs_bind("/",      FS_TASK_ID);
    vfs_bind("/dev/",  CONSOLE_TASK_ID);
    vfs_bind("/proc/", PROC_TASK_ID);
    vfs_bind("/net/",  NET_TASK_ID);

    kprintf("[boot] 4 user servers, 3 kernel apps, 2 user programs, %d pages.\n",
            pmm_free_count());
    scheduler_start();
}
