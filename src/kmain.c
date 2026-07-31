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
#include "fdt.h"
#include "pci.h"
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
    vfs_mount("/dev/console", NULL_TASK_ID, MREPL);
    kprintf("$ vfs_ns_clone(); mount /dev/console -> null\n");
    say("  THIS LINE SHOULD NEVER APPEAR\n");
    kprintf("  write(/dev/console) printed nothing: rebound to null\n");

    /* Same path, same code the shell ran — different answer, because the
       page table reported is whichever task is asking. */
    cat("/proc/pagetable", "\n$ cat /proc/pagetable  -- sandbox's own space");

    sys_send(SHELL_TASK_ID, &m, (int)sizeof(m));
    /* Finished. A task with nothing left to do blocks rather than spinning:
       `for (;;) yield()` is not idling, it is *busy*, and every message in
       the system then pays for it in context switches on the way past. That
       cost was invisible until something measured it. */
    uint64 done;
    sys_recv(&done, (int)sizeof(done));
}

/* ---- isolation demo: reach for another module's memory --------------- */
static void snooper(void)
{
    uint64 m;
    sys_recv(&m, (int)sizeof(m));

    kprintf("\n--- snooper (task %d) ----------------------------------\n",
            SNOOPER_TASK_ID);
    /* The disk used to be a window of RAM mapped into fs alone; it is a
       device now, and its registers are mapped into fs alone. The point of
       the demonstration is unchanged and the address is different. */
    kprintf("$ read *(char*)0x%lx  -- virtio registers, mapped only to drivers\n",
            VIRTIO_MMIO_BASE);
    volatile char *disk = (volatile char *)VIRTIO_MMIO_BASE;
    char c = *disk;                  /* load page fault: task is retired */

    kprintf("  UNREACHABLE: read 0x%x — isolation failed\n", (int)c);
    /* Finished. A task with nothing left to do blocks rather than spinning:
       `for (;;) yield()` is not idling, it is *busy*, and every message in
       the system then pays for it in context switches on the way past. That
       cost was invisible until something measured it. */
    uint64 done;
    sys_recv(&done, (int)sizeof(done));
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

    /* Finished. A task with nothing left to do blocks rather than spinning:
       `for (;;) yield()` is not idling, it is *busy*, and every message in
       the system then pays for it in context switches on the way past. That
       cost was invisible until something measured it. */
    uint64 done;
    sys_recv(&done, (int)sizeof(done));
}

void user_main(void);        /* user.c — runs with the U bit set */
void peeker_main(void);      /* user.c — probes another server's memory */
void loader_main(void);      /* loader.c — reads an ELF and spawns it */
void sh_main(void);          /* sh.c — reads a command line and runs it */
void net_server(void);       /* srv_net.c — virtio-net, in user mode */
void rsh_main(void);         /* srv_rsh.c — the same shell, over TCP */
void bench_main(void);       /* bench.c — what a message costs */
void pong_main(void);        /* bench.c — the other end of it */

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
        extern char __udev_bss_start[],  __udev_bss_end[];
        extern char __uload_bss_start[], __uload_bss_end[];
        extern char __ush_bss_start[],   __ush_bss_end[];
        extern char __ursh_bss_start[],  __ursh_bss_end[];
        extern char __unet_bss_start[],  __unet_bss_end[];
        extern char __umisc_bss_start[], __umisc_bss_end[];
        char *bss[][2] = {
            { __ufs_bss_start,   __ufs_bss_end   },
            { __uproc_bss_start, __uproc_bss_end },
            { __udev_bss_start,  __udev_bss_end  },
            { __uload_bss_start, __uload_bss_end },
            { __ush_bss_start,   __ush_bss_end   },
            { __ursh_bss_start,  __ursh_bss_end  },
            { __unet_bss_start,  __unet_bss_end  },
            { __umisc_bss_start, __umisc_bss_end },
        };
        for (unsigned i = 0; i < sizeof(bss) / sizeof(bss[0]); i++)
            memset(bss[i][0], 0, (size_t)(bss[i][1] - bss[i][0]));
    }

    /* Before anything is mapped: ask the machine what it is. Every address
       this kernel used to know by heart is in here. */
    if (fdt_init() == 0) {
        uint64 base, size;
        int irq = 0;
        kprintf("[boot] device tree at 0x%lx\n", dtb_pa);
        if (fdt_reg_n("serial@", 0, &base, &size, &irq) == 0)
            kprintf("       serial   0x%lx irq %d\n", base, irq);
        if (fdt_reg("plic@", &base, &size) == 0)
            kprintf("       plic     0x%lx + 0x%lx\n", base, size);
        if (fdt_reg("pci@", &base, &size) == 0)
            kprintf("       pci ecam 0x%lx + 0x%lx\n", base, size);
        kprintf("       virtio slots: %d\n", fdt_count("virtio_mmio@"));
    } else {
        kprintf("[boot] no device tree; the constants stand\n");
    }

    pmm_init();
    vm_init();
    kprintf("[boot] Sv39 on, kernel satp=0x%lx, %d pages free\n",
            r_satp(), pmm_free_count());

    /* Now that the window is mapped, ask the bus. */
    {
        int n = pci_init();
        if (n > 0) {
            kprintf("[boot] pci: %d functions on the bus\n", n);
            for (int i = 0; i < n; i++) {
                char line[80];
                pci_summary(i, line, (int)sizeof(line));
                kprintf("       %s\n", line);
            }
        } else {
            kprintf("[boot] pci: nothing on the bus\n");
        }
    }

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
    /* Two tasks whose only purpose is to time one message between them. Both
       exit when they are done, so they cost two slots and nothing else. */
    task_create_user("bench", bench_main, __umisc_start, __umisc_end); /* 14 */
    task_create_user("pong",  pong_main,  __umisc_start, __umisc_end); /* 15 */

    /* Devices are handed to their driver and to nobody else — with the U bit,
       so the driver reaches them from user mode without the kernel on the
       data path at all. */
    /* The filesystem server drives the disk itself now, so what it needs is
       the device window rather than a window of RAM somebody filled for it.

       Both driver tasks get the whole virtio-mmio range, which is a real loss
       and worth saying so: they can reach each other's registers. Handing each
       one only its own slot means the kernel deciding which slot that is,
       which means the kernel knowing what a virtio-net is — and the price of
       that is the thing this design has been avoiding all along. The device
       tree is the way out, and it is not here. */
    vm_map_at(fs->pt, VIRTIO_MMIO_BASE, VIRTIO_MMIO_BASE,
              VIRTIO_MMIO_STRIDE * VIRTIO_MMIO_SLOTS, PTE_R | PTE_W | PTE_U, 0);
    vm_map_at(con->pt, UART_BASE_PA, UART_BASE_PA, PGSIZE,
              PTE_R | PTE_W | PTE_U, 0);
    /* The virtio transports go to the network driver alone. */
    vm_map_at(net->pt, VIRTIO_MMIO_BASE, VIRTIO_MMIO_BASE,
              VIRTIO_MMIO_STRIDE * VIRTIO_MMIO_SLOTS, PTE_R | PTE_W | PTE_U, 0);

    /* Created last so that no task id above shifts: every other server is
       named by a constant that is its position in this list. This one is
       named by the pointer it was just handed, which is the better way and
       would be a pleasant afternoon's work to make general. */
    struct task *devt;
    {
        extern char __udev_start[], __udev_end[];
        devt = task_create_user("dev", dev_server, __udev_start, __udev_end);
    }

    vfs_mount("/",      FS_TASK_ID, MREPL);
    vfs_mount("/proc/", PROC_TASK_ID, MREPL);
    vfs_mount("/net/",  NET_TASK_ID, MREPL);
    /* Two servers answer under /dev and there is no union between them: the
       console is mounted at the exact name and the device server at the
       prefix, and resolution takes the longest match. So /dev/console is
       still the console, and everything else under /dev is the board
       describing itself. */
    vfs_mount("/dev/", devt ? devt->id : CONSOLE_TASK_ID, MREPL);
    vfs_mount("/dev/console", CONSOLE_TASK_ID, MREPL);
    /* And the same server publishes what the others say about themselves. */
    vfs_mount("/doc/", devt ? devt->id : PROC_TASK_ID, MREPL);

    kprintf("[boot] 4 user servers, 3 kernel apps, 2 user programs, %d pages.\n",
            pmm_free_count());
    scheduler_start();
}
