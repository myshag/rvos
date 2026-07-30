/* vfs.c — the namespace: a mount table mapping path prefixes to server tasks.
   Resolution is longest-prefix-wins, so binding "/" to the filesystem gives a
   catch-all while "/dev/" and "/proc/" carve out subtrees served by other
   modules. Entries are added at runtime with vfs_bind(), which is what makes
   the namespace data rather than policy: a new module is bound in while the
   system runs and the kernel never learns it exists. */
#include "vfs.h"
#include "util.h"

struct mount {
    char prefix[VFS_PREFIX_MAX];
    int  server;
};

static struct mount mtab[VFS_NMOUNT];
static int nmount;

int vfs_bind(const char *prefix, int server_task)
{
    if (nmount >= VFS_NMOUNT)
        return -1;
    strcpy(mtab[nmount].prefix, prefix);
    mtab[nmount].server = server_task;
    nmount++;
    return 0;
}

int vfs_route(const char *path)
{
    int best = -1;
    size_t bestlen = 0;

    for (int i = 0; i < nmount; i++) {
        if (!str_has_prefix(path, mtab[i].prefix))
            continue;
        size_t l = strlen(mtab[i].prefix);
        if (best < 0 || l > bestlen) {
            best = mtab[i].server;
            bestlen = l;
        }
    }
    return best;
}

int vfs_dump_mounts(char *out, int cap)
{
    int o = 0;
    for (int i = 0; i < nmount && o < cap - 32; i++) {
        int l = (int)strlen(mtab[i].prefix);
        memcpy(out + o, mtab[i].prefix, (size_t)l);
        o += l;
        memcpy(out + o, " -> task ", 9);
        o += 9;
        o += utoa((unsigned long)mtab[i].server, out + o);
        out[o++] = '\n';
    }
    return o;
}
