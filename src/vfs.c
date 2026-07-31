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
    int used;
};

static struct namespace ns_pool[VFS_NNS];   /* ns_pool[0] is the root */

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
    if (!current)
        return -1;
    for (int i = 1; i < VFS_NNS; i++) {
        if (ns_pool[i].used)
            continue;
        ns_pool[i] = *cur_ns();         /* snapshot, then diverge freely */
        ns_pool[i].used = 1;
        current->ns = &ns_pool[i];
        return 0;
    }
    return -1;                          /* every private namespace is taken */
}

int vfs_ns_inuse(void)
{
    int n = 1;                          /* the root is always one of them */
    for (int i = 1; i < VFS_NNS; i++)
        n += ns_pool[i].used;
    return n;
}

/* Release any private namespace no live task is pointing at. Called when a
   task is retired, which is the only moment one can become unreferenced.

   A sweep rather than a reference count, deliberately. A count has to be
   right in every place a namespace pointer is copied — task_create,
   task_new_empty's inheritance, ns_clone itself — and one missed increment
   is a slot that never comes back or, worse, one freed while in use. The
   sweep has to be right once, and the table is four tasks wide.

   "Live" here has to mean what alloc_slot means by it: a task under
   construction has state T_UNUSED and a page table, and its namespace is very
   much still spoken for. */
void vfs_ns_gc(void)
{
    for (int i = 1; i < VFS_NNS; i++) {
        if (!ns_pool[i].used)
            continue;
        int live = 0;
        for (int t = 0; t < NTASK && !live; t++)
            if ((tasks[t].state != T_UNUSED || tasks[t].pt) &&
                tasks[t].ns == &ns_pool[i])
                live = 1;
        if (!live)
            ns_pool[i].used = 0;
    }
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Where a new entry for `prefix` goes, under Plan 9's three flags.

   MREPL throws away whatever was at that name and puts this there — the
   default, and what makes the shell rebind on every connection without
   filling the table. MBEFORE and MAFTER leave what is there and join it: the
   name then has several answers, tried in order, which is a union.

   Order in this table used to mean nothing, and the previous stage said so
   while filling an unmounted hole with the last entry. That stopped being
   true the moment a name could have more than one answer, and the unmount
   below shifts now. A property that was safe to rely on can be repealed by a
   feature. */
static struct mount *insert_at(struct namespace *ns, const char *prefix,
                               int flags)
{
    if (flags == MREPL) {
        for (int i = 0; i < ns->n; ) {
            if (streq(ns->mnt[i].prefix, prefix)) {
                for (int k = i; k + 1 < ns->n; k++)
                    ns->mnt[k] = ns->mnt[k + 1];
                ns->n--;
            } else {
                i++;
            }
        }
    }
    if (ns->n >= VFS_NMOUNT)
        return 0;

    int at = ns->n;                     /* MREPL, or nothing there yet */
    if (flags == MAFTER) {
        for (int i = ns->n - 1; i >= 0; i--)
            if (streq(ns->mnt[i].prefix, prefix)) {
                at = i + 1;
                break;
            }
    } else if (flags == MBEFORE) {
        for (int i = 0; i < ns->n; i++)
            if (streq(ns->mnt[i].prefix, prefix)) {
                at = i;
                break;
            }
    }
    for (int k = ns->n; k > at; k--)
        ns->mnt[k] = ns->mnt[k - 1];
    ns->n++;
    return &ns->mnt[at];
}

int vfs_mount(const char *prefix, int server_task, int flags)
{
    struct mount *m = insert_at(cur_ns(), prefix, flags);
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
int vfs_bind(const char *old, const char *new, int flags)
{
    struct mount *m = insert_at(cur_ns(), new, flags);
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
    size_t l = strlen(prefix);
    if (l == 0)
        return 0;

    /* A server mounted at "/proc/" owns the directory as well as everything
       in it, and "/proc" is what a person types. Without this the name
       existed only with the slash on the end, which nothing else in the
       system requires and no listing showed. */
    if (prefix[l - 1] == '/' && strlen(path) == l - 1 &&
        str_has_prefix(prefix, path))
        return 1;

    if (!str_has_prefix(path, prefix))
        return 0;
    if (prefix[l - 1] == '/')
        return 1;
    char c = path[l];
    return c == 0 || c == '/';
}

/* Plan 9's unmount(nil, old): take the name back — all of it, however many
   things have been joined there. Entries are shifted rather than swapped,
   because with unions the order of the table is the order they are searched
   in and moving one past another would silently reorder a union. */
int vfs_unmount(const char *name)
{
    struct namespace *ns = cur_ns();
    int found = 0;
    for (int i = 0; i < ns->n; ) {
        if (streq(ns->mnt[i].prefix, name)) {
            for (int k = i; k + 1 < ns->n; k++)
                ns->mnt[k] = ns->mnt[k + 1];
            ns->n--;
            found = 1;
        } else {
            i++;
        }
    }
    return found ? 0 : -1;
}

/* The longest prefix still decides *which* name matched; every entry holding
   that same name is then a member of the union, searched in table order. */
static struct mount *nth_match(struct namespace *ns, const char *path, int nth)
{
    size_t bestlen = 0;
    int    any = 0;
    for (int i = 0; i < ns->n; i++) {
        if (!prefix_matches(path, ns->mnt[i].prefix))
            continue;
        size_t l = strlen(ns->mnt[i].prefix);
        if (!any || l > bestlen) {
            bestlen = l;
            any = 1;
        }
    }
    if (!any)
        return 0;

    int k = 0;
    for (int i = 0; i < ns->n; i++) {
        if (!prefix_matches(path, ns->mnt[i].prefix))
            continue;
        if (strlen(ns->mnt[i].prefix) != bestlen)
            continue;
        if (k++ == nth)
            return &ns->mnt[i];
    }
    return 0;                           /* the union has no nth member */
}

/* Resolve a name to the server that answers for it, and to the name that
   server should be asked about — which is not the same name if a bind was
   crossed on the way. */
#define RESOLVE_HOPS 4

int vfs_resolve(const char *path, char *out, int cap, int nth)
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
        /* The choice among a union's members is made once, at the name the
           caller actually asked for. Anything reached by following a bind
           from there takes that name's first answer — nesting unions inside
           unions is a generality this does not need and could not explain. */
        struct mount *m = nth_match(ns, buf, hop == 0 ? nth : 0);
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
    /* One line about the system rather than the caller, because the pool is
       small enough that running out is a thing that happens. */
    if (o < cap - 40) {
        memcpy(out + o, "-- namespaces ", 14);
        o += 14;
        o += utoa((unsigned long)vfs_ns_inuse(), out + o);
        memcpy(out + o, " of ", 4);
        o += 4;
        o += utoa((unsigned long)VFS_NNS, out + o);
        memcpy(out + o, " in use\n", 8);
        o += 8;
    }
    return o;
}
