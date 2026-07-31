/* cat.c — print a file, a report, or a connection.

   It cannot tell which. That is the whole point of the interface these stages
   built, and this is the smallest program that demonstrates it. */
#include "lib.h"

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    if (argc < 2) {
        say("usage: cat <path>...\n");
        sys_exit();
    }
    for (int i = 1; i < argc; i++) {
        int fd = vfs_open(argv[i]);
        if (fd < 0) {
            err("cat", "cannot open", argv[i]);
            continue;
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
    }
    sys_exit();
}
