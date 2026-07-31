/* ls.c — what a directory answers when you read it, made presentable.

   There is no listing call anywhere in this system: a directory is a file
   whose bytes are its listing, and those bytes are regular — `d 0 DOCS`,
   `- 105 README.TXT`. Two fields and then the name to the end of the line,
   which is a shape a program can read without rules. Turning it into
   something a person wants to look at is this program's whole job, and that
   is where presentation belongs. */
#include "lib.h"

static void one(const char *path)
{
    int fd = vfs_open(path);
    if (fd < 0) {
        err("ls", "cannot open", path);
        return;
    }
    char line[160];
    int  k = 0;
    for (;;) {
        char buf[VFS_DATA_MAX];
        int n = vfs_read(fd, buf, VFS_DATA_MAX);
        if (n <= 0)
            break;
        for (int i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                if (k < (int)sizeof(line) - 1)
                    line[k++] = buf[i];
                continue;
            }
            line[k] = 0;
            k = 0;
            /* "<type> <size> <name>" */
            if (line[0] != 'd' && line[0] != '-') {
                say(line); say("\n");   /* not ours: pass it through */
                continue;
            }
            int isdir = line[0] == 'd';
            const char *p = line + 2;
            const char *size = p;
            while (*p && *p != ' ')
                p++;
            int slen = (int)(p - size);
            if (*p == ' ')
                p++;
            say(p);
            if (isdir) {
                say("/\n");
            } else {
                say("  (");
                for (int j = 0; j < slen; j++) {
                    char c[2] = { size[j], 0 };
                    say(c);
                }
                say(" bytes)\n");
            }
        }
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
