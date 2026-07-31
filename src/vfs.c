/* vfs.c — namespaces, with Plan 9's two operations kept apart.

     mount(prefix, server)   put a server behind a name
     bind(old, new)          make `new` mean whatever `old` means

   Until now there was only the first, and it was called bind, which was a
   lie that cost something: a name could be pointed at a *server* but never at
   another *name*, so "send this program's output to that connection" had to
   be arranged out of band — the network server special-cased /dev/console and
   kept a table of which task belonged to which connection. With a real bind
   that whole mechanism is one line in a namespace, and it is gone.

   Resolution is longest-prefix-wins, and a bind rewrites the head of the path
   and resolves again, up to a small limit — because `bind /a /b; bind /b /a`
   is a thing a person can type.

   Two things make this Plan 9 rather than a global mount list:

     - it is data, not policy: both work on a running system, so a
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

enum { MNT_SERVER, MNT_BIND };

struct mount {
    char prefix[VFS_PREFIX_MAX];
    int  kind;
    int  server;                    /* MNT_SERVER: who answers */
    char target[VFS_PATH_MAX];      /* MNT_BIND:   the name it stands for */
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

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* An entry for this exact name, reused if there is one. Plan 9's default is
   MREPL — a second bind on the same point replaces the first rather than
   stacking — and it is what makes rebinding on every connection work without
   filling the table. */
static struct mount *slot_for(struct namespace *ns, const char *prefix)
{
    for (int i = 0; i < ns->n; i++)
        if (streq(ns->mnt[i].prefix, prefix))
            return &ns->mnt[i];
    if (ns->n >= VFS_NMOUNT)
        return 0;
    return &ns->mnt[ns->n++];
}

int vfs_mount(const char *prefix, int server_task)
{
    struct mount *m = slot_for(cur_ns(), prefix);
    if (!m)
        return -1;
    strcpy(m->prefix, prefix);
    m->kind      = MNT_SERVER;
    m->server    = server_task;
    m->target[0] = 0;
    return 0;
}

/* bind(old, new): `new` now means whatever `old` means. The order is Plan 9's
   and it is the thing everybody gets backwards — the *second* argument is the
   name that changes. */
int vfs_bind(const char *old, const char *new)
{
    struct mount *m = slot_for(cur_ns(), new);
    if (!m)
        return -1;
    strcpy(m->prefix, new);
    m->kind   = MNT_BIND;
    m->server = -1;
    strcpy(m->target, old);
    return 0;
}

/* A prefix has to end where a component ends, or "/mnt" would claim
   "/mnt2/x" and rewrite it to "2/x". Prefixes written with a trailing slash —
   "/dev/", and "/" itself — already end at a boundary by construction. */
static int prefix_matches(const char *path, const char *prefix)
{
    if (!str_has_prefix(path, prefix))
        return 0;
    size_t l = strlen(prefix);
    if (l == 0)
        return 0;
    if (prefix[l - 1] == '/')
        return 1;
    char c = path[l];
    return c == 0 || c == '/';
}

static struct mount *longest_match(struct namespace *ns, const char *path)
{
    struct mount *best = 0;
    size_t bestlen = 0;
    for (int i = 0; i < ns->n; i++) {
        if (!prefix_matches(path, ns->mnt[i].prefix))
            continue;
        size_t l = strlen(ns->mnt[i].prefix);
        if (!best || l > bestlen) {
            best = &ns->mnt[i];
            bestlen = l;
        }
    }
    return best;
}

/* Resolve a name to the server that answers for it, and to the name that
   server should be asked about — which is not the same name if a bind was
   crossed on the way. */
#define RESOLVE_HOPS 4

int vfs_resolve(const char *path, char *out, int cap)
{
    char buf[VFS_PATH_MAX];
    int n = 0;
    while (path[n] && n < VFS_PATH_MAX - 1) {
        buf[n] = path[n];
        n++;
    }
    buf[n] = 0;

    struct namespace *ns = cur_ns();
    for (int hop = 0; hop < RESOLVE_HOPS; hop++) {
        struct mount *m = longest_match(ns, buf);
        if (!m)
            return -1;                          /* nothing bound over it */
        if (m->kind == MNT_SERVER) {
            int i = 0;
            while (buf[i] && i < cap - 1) {
                out[i] = buf[i];
                i++;
            }
            out[i] = 0;
            return m->server;
        }
        /* A bind: swap the head of the path for what it stands for, and go
           round again — the target itself has to be resolved. */
        char next[VFS_PATH_MAX];
        int k = 0;
        for (const char *t = m->target; *t && k < VFS_PATH_MAX - 1; t++)
            next[k++] = *t;
        const char *rest = buf + strlen(m->prefix);
        /* `bind / /mnt` then "/mnt/README.TXT": the target ends with a slash
           and the remainder begins with one, and "//README.TXT" is a name the
           filesystem has never heard of. */
        if (k > 0 && next[k - 1] == '/' && *rest == '/')
            rest++;
        for (; *rest && k < VFS_PATH_MAX - 1; rest++)
            next[k++] = *rest;
        next[k] = 0;
        strcpy(buf, next);
    }
    return -1;                                  /* a loop of binds */
}

int vfs_dump_mounts_of(int task_id, char *out, int cap)
{
    struct namespace *ns;
    struct task *t = task_by_id(task_id);
    if (t && t->ns)
        ns = t->ns;
    else
        ns = &ns_pool[0];

    int o = 0;
    for (int i = 0; i < ns->n && o < cap - 32; i++) {
        int l = (int)strlen(ns->mnt[i].prefix);
        memcpy(out + o, ns->mnt[i].prefix, (size_t)l);
        o += l;
        if (ns->mnt[i].kind == MNT_BIND) {
            memcpy(out + o, " -> ", 4);
            o += 4;
            int tl = (int)strlen(ns->mnt[i].target);
            memcpy(out + o, ns->mnt[i].target, (size_t)tl);
            o += tl;
        } else {
            memcpy(out + o, " -> task ", 9);
            o += 9;
            o += utoa((unsigned long)ns->mnt[i].server, out + o);
        }
        out[o++] = '\n';
    }
    return o;
}
