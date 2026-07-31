/* mc.c — two panels, in colour, over whatever the console happens to be.

   Nothing in this program is privileged and nothing in it is new. It opens
   /dev/console and reads and writes it; it opens directories and reads them.
   That is the same five calls `cat` uses. What makes it a full-screen program
   rather than a scrolling one is that a terminal is *also* just bytes: cursor
   positions and colours are escape sequences, and they have travelled
   unchanged through every layer this system has — the console server, the TCP
   stack, telnet's framing — because none of those layers has an opinion about
   what the bytes mean.

   Two things did have to be arranged.

   The first is that a terminal reached over telnet is in *line* mode by
   default: the client keeps local echo and sends nothing until Enter, so an
   arrow key would arrive, if at all, only after one. Character mode is two
   telnet options, and this program sends them itself — it can, because
   /dev/console *is* the connection, so the negotiation is just more bytes.
   It undoes them on the way out.

   The second is knowing whether to do that at all. On the serial line there
   is no telnet and those bytes would be printed as rubbish. So the program
   asks the namespace what its own console is: sys_resolve reports not only
   which server answers for a name but what name to ask it about, and if that
   name begins with /net/tcp/ then the console is a connection. That is the
   namespace answering a question about itself, and it is the alternative to
   guessing.

   A note on speed, which shaped the whole file. One character written is one
   message, and a message here costs about ninety microseconds — so a screen
   drawn a character at a time would take a fifth of a second. The screen is
   built in a buffer and written in a handful of calls instead.

   usage: /BIN/MC.ELF [left] [right]                                       */
#include "lib.h"

#define COLS   80
#define ROWS   24
#define PANEW  39                       /* two panels and a gap */
#define LISTH  18                       /* rows of entries */
#define MAXENT 48
#define NAMEW  64

struct ent {
    char          name[NAMEW];
    unsigned long size;
    int           isdir;
};

struct panel {
    char       path[VFS_PATH_MAX];
    struct ent e[MAXENT];
    int        n, sel, top;
};

static struct panel pan[2];
static int active;
static int con = -1;
static int telnet;                      /* the console is a TCP connection */

/* ---- the screen, built before any of it is sent --------------------- */

static char  scr[8192];
static int   scrlen;

static void put(const char *s)
{
    while (*s && scrlen < (int)sizeof(scr))
        scr[scrlen++] = *s++;
}

static void putn(unsigned long v)
{
    char t[24], o[25];
    int k = 0, j = 0;
    if (!v)
        t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) o[j++] = t[--k];
    o[j] = 0;
    put(o);
}

static void at(int row, int col)
{
    put("\x1b[");
    putn((unsigned long)row);
    put(";");
    putn((unsigned long)col);
    put("H");
}

static void flush(void)
{
    int off = 0;
    while (off < scrlen) {
        int n = scrlen - off;
        if (n > VFS_DATA_MAX)
            n = VFS_DATA_MAX;
        int k = vfs_write(con, scr + off, n);
        if (k < 0)
            break;
        if (k == 0) { sys_yield(); continue; }
        off += k;
    }
    scrlen = 0;
}

/* ---- reading a directory -------------------------------------------- */

/* The listing is regular — "<type> <size> <name>" — so this needs no rules:
   two fields, then everything to the end of the line is the name, spaces and
   all. That is why the format was changed before this program was written. */
static void load(struct panel *p)
{
    p->n = 0;
    p->sel = 0;
    p->top = 0;

    int fd = vfs_open(p->path);
    if (fd < 0)
        return;

    char line[200];
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
            if ((line[0] != 'd' && line[0] != '-') || p->n >= MAXENT)
                continue;
            struct ent *e = &p->e[p->n];
            e->isdir = line[0] == 'd';
            const char *q = line + 2;
            unsigned long sz = 0;
            while (*q >= '0' && *q <= '9')
                sz = sz * 10 + (unsigned long)(*q++ - '0');
            e->size = sz;
            if (*q == ' ')
                q++;
            int j = 0;
            while (*q && j < NAMEW - 1)
                e->name[j++] = *q++;
            e->name[j] = 0;
            /* A directory's entry for itself is on the disk and is noise in a
               panel; the one for its parent is how you leave. */
            if (e->name[0] && !(e->name[0] == '.' && e->name[1] == 0))
                p->n++;
        }
    }
    vfs_close(fd);
}

/* "/DOCS" + "NOTE.TXT" -> "/DOCS/NOTE.TXT", and "/" + anything has no extra
   slash to give. */
static void join(char *out, const char *dir, const char *name)
{
    int k = 0;
    while (dir[k] && k < VFS_PATH_MAX - 2) {
        out[k] = dir[k];
        k++;
    }
    if (k == 0 || out[k - 1] != '/')
        out[k++] = '/';
    while (*name && k < VFS_PATH_MAX - 1)
        out[k++] = *name++;
    out[k] = 0;
}

static void go_up(struct panel *p)
{
    int k = 0;
    while (p->path[k])
        k++;
    if (k > 1 && p->path[k - 1] == '/')
        k--;
    while (k > 0 && p->path[k - 1] != '/')
        k--;
    if (k < 1)
        k = 1;
    p->path[k] = 0;
    load(p);
}

/* ---- drawing --------------------------------------------------------- */

#define C_RESET  "\x1b[0m"
#define C_DIR    "\x1b[1;34m"
#define C_PROG   "\x1b[1;32m"
#define C_HEAD   "\x1b[1;44;37m"
#define C_SEL    "\x1b[7m"
#define C_KEYS   "\x1b[1;46;30m"

static int is_prog(const char *n)
{
    int k = 0;
    while (n[k]) k++;
    return k > 4 && n[k - 4] == '.' && n[k - 3] == 'E' &&
           n[k - 2] == 'L' && n[k - 1] == 'F';
}

/* Pad to a width in *characters*, counting UTF-8 lead bytes only: a
   continuation byte is not a column. */
static int cells(const char *s, int max)
{
    int n = 0;
    for (int i = 0; s[i] && n < max; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            n++;
    return n;
}

static void draw_panel(int idx, int col)
{
    struct panel *p = &pan[idx];

    at(1, col);
    put(C_HEAD);
    int w = 0;
    put(" ");
    w++;
    for (const char *q = p->path; *q && w < PANEW - 1; q++) {
        char c[2] = { *q, 0 };
        put(c);
        if (((unsigned char)*q & 0xC0) != 0x80)
            w++;
    }
    while (w < PANEW) { put(" "); w++; }
    put(C_RESET);

    for (int r = 0; r < LISTH; r++) {
        at(2 + r, col);
        int i = p->top + r;
        if (i >= p->n) {
            for (int k = 0; k < PANEW; k++)
                put(" ");
            continue;
        }
        struct ent *e = &p->e[i];
        int selected = (i == p->sel && idx == active);
        if (selected)
            put(C_SEL);
        else if (e->isdir)
            put(C_DIR);
        else if (is_prog(e->name))
            put(C_PROG);

        put(e->isdir ? "/" : " ");
        int used = 1;
        int nw = cells(e->name, PANEW - 12);
        for (int k = 0, shown = 0; e->name[k] && shown < nw; k++) {
            char c[2] = { e->name[k], 0 };
            put(c);
            if (((unsigned char)e->name[k] & 0xC0) != 0x80)
                shown++;
        }
        used += nw;
        char num[24];
        int  nl = 0;
        {
            unsigned long v = e->size;
            char t[24];
            int  tk = 0;
            if (!v) t[tk++] = '0';
            while (v) { t[tk++] = (char)('0' + v % 10); v /= 10; }
            while (tk) num[nl++] = t[--tk];
            num[nl] = 0;
        }
        int pad = PANEW - used - nl - 1;
        for (int k = 0; k < pad; k++)
            put(" ");
        if (e->isdir)
            put(" ");
        else
            put(num);
        put(" ");
        put(C_RESET);
    }
}

static void draw(const char *msg)
{
    scrlen = 0;
    put("\x1b[H");
    draw_panel(0, 1);
    draw_panel(1, PANEW + 2);

    at(ROWS - 2, 1);
    put("\x1b[K");
    struct panel *p = &pan[active];
    if (msg) {
        put(msg);
    } else if (p->n) {
        put(p->path);
        if (p->path[0] && p->path[1])
            put("/");
        put(p->e[p->sel].name);
    }

    at(ROWS, 1);
    put(C_KEYS);
    put(" arrows move  tab panel  enter open  v view  c copy  k delete  "
        "r reread  q quit ");
    put(C_RESET);
    at(ROWS - 1, 1);
    flush();
}

/* ---- input ----------------------------------------------------------- */

#define K_UP    1000
#define K_DOWN  1001
#define K_RIGHT 1002
#define K_LEFT  1003
#define K_EOF   (-1)

static int rawbyte(void)
{
    char c;
    int n = vfs_read(con, &c, 1);
    if (n <= 0)
        return K_EOF;
    return (unsigned char)c;
}

/* One key. Escape sequences become one code; telnet's own bytes are eaten
   here, because over a connection they arrive mixed into the same stream and
   this program is the one reading it. */
static int getkey(void)
{
    for (;;) {
        int c = rawbyte();
        if (c == K_EOF)
            return K_EOF;
        if (c == 255) {                 /* IAC */
            int b = rawbyte();
            if (b == K_EOF)
                return K_EOF;
            if (b == 255)
                return 255;
            if (b >= 251 && b <= 254) { /* WILL/WONT/DO/DONT + option */
                if (rawbyte() == K_EOF)
                    return K_EOF;
                continue;
            }
            if (b == 250) {             /* a subnegotiation, to its end */
                int prev = 0;
                for (;;) {
                    int k = rawbyte();
                    if (k == K_EOF)
                        return K_EOF;
                    if (prev == 255 && k == 240)
                        break;
                    prev = k;
                }
            }
            continue;
        }
        if (c != 27)
            return c;
        int b = rawbyte();
        if (b == K_EOF)
            return K_EOF;
        if (b != '[' && b != 'O')
            return 27;
        int d = rawbyte();
        if (d == K_EOF)
            return K_EOF;
        switch (d) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        default:
            /* Anything else with a numeric tail: swallow to its final byte. */
            while (d >= '0' && d <= '9')
                d = rawbyte();
            continue;
        }
    }
}

/* ---- actions --------------------------------------------------------- */

static void view(const char *path)
{
    scrlen = 0;
    put("\x1b[2J\x1b[H");
    put(C_HEAD);
    put(" ");
    put(path);
    put(" — any key to return ");
    put(C_RESET);
    put("\r\n");
    flush();

    int fd = vfs_open(path);
    if (fd >= 0) {
        int total = 0;
        for (;;) {
            char buf[VFS_DATA_MAX];
            int n = vfs_read(fd, buf, VFS_DATA_MAX);
            if (n <= 0 || total > 1400)
                break;
            /* A file may hold bare newlines; a terminal wants both. */
            scrlen = 0;
            for (int i = 0; i < n; i++) {
                if (buf[i] == '\n')
                    put("\r");
                char c[2] = { buf[i], 0 };
                put(c);
            }
            flush();
            total += n;
        }
        vfs_close(fd);
    }
    getkey();
}

static void confirm_delete(struct panel *p)
{
    if (!p->n)
        return;
    char full[VFS_PATH_MAX];
    join(full, p->path, p->e[p->sel].name);

    draw("delete? press y");
    if (getkey() != 'y') {
        draw(0);
        return;
    }
    int fd = vfs_open(full);
    if (fd >= 0) {
        int r = vfs_ioctl(fd, IOCTL_REMOVE);
        vfs_close(fd);
        load(p);
        draw(r < 0 ? "delete refused" : "deleted");
        return;
    }
    draw("cannot open it");
}

static void copy_over(void)
{
    struct panel *from = &pan[active], *to = &pan[active ^ 1];
    if (!from->n || from->e[from->sel].isdir) {
        draw("only files, and only one at a time");
        return;
    }
    char src[VFS_PATH_MAX], dst[VFS_PATH_MAX];
    join(src, from->path, from->e[from->sel].name);
    join(dst, to->path, from->e[from->sel].name);
    draw("copying…");
    int n = pcopy(src, dst);
    load(to);
    draw(n < 0 ? "copy failed" : "copied");
}

/* ---- setting up ------------------------------------------------------ */

/* Is this console a TCP connection? Ask the namespace: resolve reports the
   name the serving task should be asked about, and only a connection is
   called /net/tcp/N. */
static int console_is_connection(void)
{
    char real[VFS_PATH_MAX];
    if (sys_resolve("/dev/console", real, (int)sizeof(real), 0) < 0)
        return 0;
    const char *want = "/net/tcp/";
    for (int i = 0; want[i]; i++)
        if (real[i] != want[i])
            return 0;
    return 1;
}

static void raw_mode(int on)
{
    if (!telnet)
        return;
    /* WILL ECHO and WILL SUPPRESS-GO-AHEAD together are what a telnet client
       reads as "the far end is a program, send me every character and do not
       echo them yourself". Withdrawing them puts the shell back the way it
       expects to find things. */
    unsigned char seq[6];
    seq[0] = 255; seq[1] = on ? 251 : 252; seq[2] = 1;   /* ECHO */
    seq[3] = 255; seq[4] = on ? 251 : 252; seq[5] = 3;   /* SGA */
    vfs_write(con, (const char *)seq, 6);
}

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    con = vfs_open("/dev/console");
    if (con < 0) {
        say("mc: no console\n");
        sys_exit();
    }
    telnet = console_is_connection();

    const char *l = argc > 1 ? argv[1] : "/";
    const char *r = argc > 2 ? argv[2] : "/BIN";
    for (int i = 0; l[i] && i < VFS_PATH_MAX - 1; i++) pan[0].path[i] = l[i];
    for (int i = 0; r[i] && i < VFS_PATH_MAX - 1; i++) pan[1].path[i] = r[i];
    load(&pan[0]);
    load(&pan[1]);

    raw_mode(1);
    scrlen = 0;
    put("\x1b[2J\x1b[?25l");             /* clear, and hide the cursor */
    flush();
    draw(0);

    for (;;) {
        int k = getkey();
        struct panel *p = &pan[active];

        if (k == K_EOF || k == 'q')
            break;
        if (k == 9) {                    /* tab */
            active ^= 1;
        } else if (k == K_UP) {
            if (p->sel > 0) p->sel--;
            if (p->sel < p->top) p->top = p->sel;
        } else if (k == K_DOWN) {
            if (p->sel + 1 < p->n) p->sel++;
            if (p->sel >= p->top + LISTH) p->top = p->sel - LISTH + 1;
        } else if (k == K_LEFT) {
            go_up(p);
        } else if (k == 13 || k == 10 || k == K_RIGHT) {
            if (p->n) {
                struct ent *e = &p->e[p->sel];
                if (e->name[0] == '.' && e->name[1] == 0) {
                    /* itself: nothing to do */
                } else if (e->name[0] == '.' && e->name[1] == '.' &&
                           e->name[2] == 0) {
                    go_up(p);
                } else if (e->isdir) {
                    char next[VFS_PATH_MAX];
                    join(next, p->path, e->name);
                    for (int i = 0; i < VFS_PATH_MAX; i++)
                        p->path[i] = next[i];
                    load(p);
                } else {
                    char full[VFS_PATH_MAX];
                    join(full, p->path, e->name);
                    view(full);
                    put("\x1b[2J");
                }
            }
        } else if (k == 'v') {
            if (p->n && !p->e[p->sel].isdir) {
                char full[VFS_PATH_MAX];
                join(full, p->path, p->e[p->sel].name);
                view(full);
                put("\x1b[2J");
            }
        } else if (k == 'c') {
            copy_over();
            continue;
        } else if (k == 'k') {
            confirm_delete(p);
            continue;
        } else if (k == 'r') {
            load(&pan[0]);
            load(&pan[1]);
        }
        draw(0);
    }

    scrlen = 0;
    put("\x1b[?25h" C_RESET "\x1b[2J\x1b[H");
    flush();
    raw_mode(0);
    vfs_close(con);
    sys_exit();
}
