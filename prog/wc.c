/* wc.c — lines, words and bytes, counted as they stream past. */
#include "lib.h"

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    if (argc < 2) {
        say("usage: wc <path>...\n");
        sys_exit();
    }
    for (int i = 1; i < argc; i++) {
        int fd = vfs_open(argv[i]);
        if (fd < 0) {
            err("wc", "cannot open", argv[i]);
            continue;
        }
        unsigned long lines = 0, words = 0, bytes = 0;
        int in_word = 0;
        for (;;) {
            char buf[VFS_DATA_MAX];
            int n = vfs_read(fd, buf, VFS_DATA_MAX);
            if (n <= 0)
                break;
            for (int k = 0; k < n; k++) {
                bytes++;
                if (buf[k] == '\n')
                    lines++;
                if (buf[k] == ' ' || buf[k] == '\n' || buf[k] == '\t') {
                    in_word = 0;
                } else if (!in_word) {
                    in_word = 1;
                    words++;
                }
            }
        }
        vfs_close(fd);
        sayn(lines); say(" ");
        sayn(words); say(" ");
        sayn(bytes); say("  ");
        say(argv[i]); say("\n");
    }
    sys_exit();
}
