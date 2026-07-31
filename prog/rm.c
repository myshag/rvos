/* rm.c — removal goes through ioctl, which is the call for what does not fit
   read and write. A directory goes only if it is empty. */
#include "lib.h"

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    if (argc < 2) {
        say("usage: rm <path>...\n");
        sys_exit();
    }
    for (int i = 1; i < argc; i++) {
        int fd = vfs_open(argv[i]);
        if (fd < 0) {
            err("rm", "no such file", argv[i]);
            continue;
        }
        if (vfs_ioctl(fd, IOCTL_REMOVE) < 0)
            err("rm", "refused", argv[i]);
        vfs_close(fd);
    }
    sys_exit();
}
