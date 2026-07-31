/* ps.c — the task table, which the proc server renders on request. This
   program knows no more about tasks than cat does. */
#include "lib.h"

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    (void)argc; (void)argv;
    int fd = vfs_open("/proc/tasks");
    if (fd < 0) {
        err("ps", "no /proc/tasks", 0);
        sys_exit();
    }
    for (;;) {
        char buf[VFS_DATA_MAX + 1];
        int n = vfs_read(fd, buf, VFS_DATA_MAX);
        if (n <= 0)
            break;
        buf[n] = 0;
        say(buf);
    }
    vfs_close(fd);
    sys_exit();
}
