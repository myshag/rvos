/* mkdir.c — a directory is a file whose contents are entries; making one is
   an ioctl on a name, because there is nothing to open yet. */
#include "lib.h"

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    if (argc < 2) {
        say("usage: mkdir <path>...\n");
        sys_exit();
    }
    for (int i = 1; i < argc; i++)
        if (vfs_ioctl_path(argv[i], IOCTL_MKDIR) < 0)
            err("mkdir", "refused", argv[i]);
    sys_exit();
}
