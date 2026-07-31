/* ls.c — what a directory answers when you read it, made presentable.

   There is no listing call anywhere in this system: a directory is a file
   whose bytes are its listing, and those bytes are regular — `d 0 DOCS`,
   `- 105 README.TXT`. Two fields and then the name to the end of the line,
   which is a shape a program can read without rules. Turning it into
   something a person wants to look at is this program's whole job, and that
   is where presentation belongs. */
#include "lib.h"

/* Names already printed, so that a mount point which the serving directory
   also knows about is not shown twice. /dev is the case: the console server
   lists `console`, and a sandbox has it bound there as well. */
static char seen[512];
static int  seen_n;

static int already(const char *name)
{
    for (int i = 0; i < seen_n; ) {
        int j = 0;
        while (seen[i + j] && seen[i + j] == name[j]) j++;
        if (!seen[i + j] && !name[j])
            return 1;
        while (i < seen_n && seen[i]) i++;
        i++;
    }
    return 0;
}

static void remember(const char *name)
{
    for (int j = 0; name[j] && seen_n < (int)sizeof(seen) - 2; j++)
        seen[seen_n++] = name[j];
    if (seen_n < (int)sizeof(seen) - 1)
        seen[seen_n++] = 0;
}

/* One "<type> <size> <name>" line, made presentable. */
static void show(char *line)
{
    if (line[0] != 'd' && line[0] != '-') {
        say(line); say("\n");          /* not ours: pass it through */
        return;
    }
    int isdir = line[0] == 'd';
    const char *p = line + 2;
    const char *size = p;
    while (*p && *p != ' ')
        p++;
    int slen = (int)(p - size);
    if (*p == ' ')
        p++;
    if (already(p))
        return;
    remember(p);
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

static void one(const char *path)
{
    seen_n = 0;
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
            show(line);
        }
    }
    vfs_close(fd);

    /* And the names the namespace put here, which the server that answered
       for this directory has never heard of. */
    char extra[512];
    int  n = vfs_mounts_in(path, extra, (int)sizeof(extra));
    k = 0;
    for (int i = 0; i < n; i++) {
        if (extra[i] != '\n') {
            if (k < (int)sizeof(line) - 1)
                line[k++] = extra[i];
            continue;
        }
        line[k] = 0;
        k = 0;
        show(line);
    }
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
