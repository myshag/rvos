/* mv.c — a copy and a removal.

   There is no rename in the filesystem, and renaming within one directory
   would be a smaller operation than this: rewriting eleven bytes of one entry.
   Doing it as copy-and-delete is honest about what is implemented rather than
   fast, and it is the only version that also works across directories. */
#include "lib.h"

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    if (argc < 3) {
        say("usage: mv <src> <dst>\n");
        sys_exit();
    }
    if (pcopy(argv[1], argv[2]) < 0) {
        err("mv", "cannot copy", argv[1]);
        sys_exit();
    }
    int fd = vfs_open(argv[1]);
    if (fd >= 0) {
        if (vfs_ioctl(fd, IOCTL_REMOVE) < 0)
            err("mv", "copied, but cannot remove", argv[1]);
        vfs_close(fd);
    }
    sys_exit();
}
