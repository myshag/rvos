/* vfs.c — namespaces. A namespace is a mount table: path prefixes bound to
   server tasks, resolved longest-prefix-wins. Two things make it Plan 9
   rather than a global mount list:

     - it is data, not policy: vfs_bind() works on a running system, so a
       module can be attached to the tree after the fact (the kernel never
       learns);
     - it is per task: vfs_ns_clone() snapshots the caller's table into a
       private one (Plan 9's rfork(RFNAMEG)), after which that task can
       rebind paths without anyone else seeing the change. The same path in
       two tasks can then reach two different modules.

   Because a namespace belongs to the *caller*, a server asked about it must
   be told whose to look at — hence vfs_dump_mounts_of(task_id, ...). */
#include "vfs.h"
#include "task.h"
#include "util.h"

struct mount {
    char prefix[VFS_PREFIX_MAX];
    int  server;
};

struct namespace {
    struct mount mnt[VFS_NMOUNT];
    int n;
};

static struct namespace ns_pool[VFS_NNS];
static int nns = 1;                     /* ns_pool[0] is the root namespace */

struct namespace *vfs_root_ns(void)
{
    return &ns_pool[0];
}

/* During kmain() there is no current task yet, so boot-time binds land in the
   root namespace — which is exactly what every task then inherits. */
static struct namespace *cur_ns(void)
{
    if (current && current->ns)
        return current->ns;
    return &ns_pool[0];
}

int vfs_ns_clone(void)
{
    if (!current || nns >= VFS_NNS)
        return -1;
    struct namespace *n = &ns_pool[nns++];
    *n = *cur_ns();                     /* snapshot, then diverge freely */
    current->ns = n;
    return 0;
}

int vfs_bind(const char *prefix, int server_task)
{
    struct namespace *ns = cur_ns();
    if (ns->n >= VFS_NMOUNT)
        return -1;
    strcpy(ns->mnt[ns->n].prefix, prefix);
    ns->mnt[ns->n].server = server_task;
    ns->n++;
    return 0;
}

static int route_in(struct namespace *ns, const char *path)
{
    int best = -1;
    size_t bestlen = 0;

    for (int i = 0; i < ns->n; i++) {
        if (!str_has_prefix(path, ns->mnt[i].prefix))
            continue;
        size_t l = strlen(ns->mnt[i].prefix);
        if (best < 0 || l > bestlen) {
            best = ns->mnt[i].server;
            bestlen = l;
        }
    }
    return best;
}

int vfs_route(const char *path)
{
    return route_in(cur_ns(), path);
}

int vfs_dump_mounts_of(int task_id, char *out, int cap)
{
    struct namespace *ns;
    if (task_id >= 0 && task_id < NTASK && tasks[task_id].ns)
        ns = tasks[task_id].ns;
    else
        ns = &ns_pool[0];

    int o = 0;
    for (int i = 0; i < ns->n && o < cap - 32; i++) {
        int l = (int)strlen(ns->mnt[i].prefix);
        memcpy(out + o, ns->mnt[i].prefix, (size_t)l);
        o += l;
        memcpy(out + o, " -> task ", 9);
        o += 9;
        o += utoa((unsigned long)ns->mnt[i].server, out + o);
        out[o++] = '\n';
    }
    return o;
}
