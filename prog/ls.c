/* ls.c — what a directory answers when you read it.

   There is no listing call anywhere in this system. A directory is a file
   whose bytes are its listing, so this program is cat with a default. */
#include "lib.h"

static void one(const char *path)
{
    int fd = vfs_open(path);
    if (fd < 0) {
        err("ls", "cannot open", path);
        return;
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

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    if (argc < 2) {
        one("/");
    } else {
        for (int i = 1; i < argc; i++) {
            if (argc > 2) {
                say(argv[i]);
                say(":\n");
            }
            one(argv[i]);
        }
    }
    sys_exit();
}
