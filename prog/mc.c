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

/* Measured if the terminal will say, and assumed if it will not. Assuming
   was not a harmless simplification: on a narrower screen the right panel ran
   past the edge, wrapped, and overwrote the left panel's names on the line
   below — a display that is wrong rather than merely cramped. */
#define MAXENT 48
#define NAMEW  64

static int term_w = 80, term_h = 24;

#define PANEW (((term_w) - 1) / 2)      /* two panels and a gap between */
#define LISTH ((term_h) - 6)

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

/* ---- drawing ----------------------------------------------------------
   The look is Midnight Commander's, and copying it is not decoration: a panel
   with a frame, a header and a footer is three more things the eye can find,
   and the function-key strip along the bottom is the only documentation a
   full-screen program gets to show.

   Colour here is a *background* as much as a foreground. Every row is painted
   to its full width, because a row that stops short leaves the terminal's own
   background showing through and the panel stops looking like a panel. */

#define C_RESET  "\x1b[0m"
#define B_PANEL  "\x1b[44;37m"          /* white on blue: the panel itself */
#define B_FRAME  "\x1b[44;1;36m"        /* its border */
#define B_DIR    "\x1b[44;1;37m"        /* a directory, bright */
#define B_PROG   "\x1b[44;1;32m"        /* something runnable */
#define B_SEL    "\x1b[46;30m"          /* the cursor: black on cyan */
#define B_HEADC  "\x1b[44;1;33m"        /* the column titles */
#define B_BAR    "\x1b[46;30m"          /* the menu and the key strip */
#define B_HOT    "\x1b[46;1;33m"        /* the letter you press */
#define B_NUM    "\x1b[47;30m"          /* the number on a key */
#define B_STAT   "\x1b[40;37m"

#define TL "┌"
#define TR "┐"
#define BL "└"
#define BR "┘"
#define HZ "─"
#define VT "│"

static int is_prog(const char *n)
{
    int k = 0;
    while (n[k]) k++;
    return k > 4 && n[k - 4] == '.' && n[k - 3] == 'E' &&
           n[k - 2] == 'L' && n[k - 1] == 'F';
}

/* Columns, not bytes: a UTF-8 continuation byte occupies no space on screen,
   so anything that pads has to count lead bytes. */
static int put_trim(const char *s, int max)
{
    int shown = 0;
    for (int i = 0; s[i] && shown < max; i++) {
        char c[2] = { s[i], 0 };
        put(c);
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            shown++;
    }
    return shown;
}

static void pad(int n)
{
    while (n-- > 0)
        put(" ");
}

static void repeat(const char *g, int n)
{
    while (n-- > 0)
        put(g);
}

static void num_into(char *out, unsigned long v)
{
    char t[24];
    int k = 0, j = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) out[j++] = t[--k];
    out[j] = 0;
}

#define SIZEW 8

static void draw_panel(int idx, int col, int w)
{
    struct panel *p = &pan[idx];
    int inner = w - 2;

    /* Top frame, with the path let into it. */
    at(2, col);
    put(B_FRAME);
    put(TL);
    put(HZ);
    put(B_PANEL " ");
    int used = 2 + 1 + put_trim(p->path, inner - 4);
    put(" " B_FRAME);
    used += 1;
    repeat(HZ, w - used - 1);
    put(TR);

    /* Column titles. */
    at(3, col);
    put(B_FRAME VT B_HEADC);
    int nw = inner - SIZEW - 1;
    int k = put_trim(" Name", nw);
    pad(nw - k);
    put(" ");
    k = put_trim("Size", SIZEW);
    pad(SIZEW - k);
    put(B_FRAME VT);

    for (int r = 0; r < LISTH; r++) {
        at(4 + r, col);
        put(B_FRAME VT);
        int i = p->top + r;
        if (i >= p->n) {
            put(B_PANEL);
            pad(inner);
            put(B_FRAME VT);
            continue;
        }
        struct ent *e = &p->e[i];
        int selected = (i == p->sel && idx == active);
        put(selected ? B_SEL : e->isdir ? B_DIR : is_prog(e->name) ? B_PROG
                                                                   : B_PANEL);
        put(e->isdir ? "/" : " ");
        int shown = 1 + put_trim(e->name, nw - 1);
        pad(nw - shown);
        put(" ");
        if (e->isdir) {
            k = put_trim("DIR", SIZEW);
        } else {
            char num[24];
            num_into(num, e->size);
            int l = 0;
            while (num[l]) l++;
            pad(SIZEW - l);
            put(num);
            k = SIZEW;
        }
        pad(SIZEW - k);
        put(B_FRAME VT);
    }

    /* Bottom frame, carrying what the cursor is on — MC's mini-status. */
    at(term_h - 3, col);
    put(B_FRAME);
    put(BL);
    put(HZ);
    if (p->n) {
        put(B_PANEL " ");
        used = 2 + 1 + put_trim(p->e[p->sel].name, inner - 4);
        put(" " B_FRAME);
        used += 1;
    } else {
        used = 2;
    }
    repeat(HZ, w - used - 1);
    put(BR);
}

/* " Left  File  Command  Options  Right", with the letter you press in
   yellow — which is how the original tells you there is a menu without
   spending a line explaining it. */
static void draw_menu(void)
{
    static const char *items[] = { "Left", "File", "Command", "Options",
                                   "Right", 0 };
    at(1, 1);
    put(B_BAR);
    int used = 0;
    for (int i = 0; items[i]; i++) {
        put("  ");
        used += 2;
        put(B_HOT);
        char c[2] = { items[i][0], 0 };
        put(c);
        put(B_BAR);
        used += 1 + put_trim(items[i] + 1, term_w - used - 3);
        if (used > term_w - 4)
            break;
    }
    pad(term_w - used);
}

static void draw_keys(void)
{
    static const char *lab[] = { "Help", "Menu", "View", "Edit", "Copy",
                                 "RenMov", "Mkdir", "Delete", "PullDn",
                                 "Quit" };
    at(term_h, 1);
    int used = 0;
    for (int i = 0; i < 10; i++) {
        char n[4];
        num_into(n, (unsigned long)(i + 1));
        int nl = 0;
        while (n[nl]) nl++;
        int labl = 0;
        while (lab[i][labl]) labl++;
        if (used + nl + labl + 1 > term_w)
            break;
        put(B_NUM);
        put(n);
        put(B_BAR);
        put_trim(lab[i], labl);
        used += nl + labl;
        if (used < term_w) { put(B_BAR " "); used++; }
    }
    put(B_BAR);
    pad(term_w - used);
    put(C_RESET);
}

static void draw(const char *msg)
{
    scrlen = 0;
    put("\x1b[H");
    draw_menu();

    int lw = term_w / 2;
    draw_panel(0, 1, lw);
    draw_panel(1, lw + 1, term_w - lw);

    /* Hint, then the line a command line would live on. */
    at(term_h - 2, 1);
    put(B_STAT);
    struct panel *p = &pan[active];
    int used = 0;
    if (msg) {
        used = put_trim(msg, term_w);
    } else {
        used = put_trim("Hint: everything here is a file, including this.",
                        term_w);
    }
    pad(term_w - used);

    at(term_h - 1, 1);
    put(B_STAT);
    used = put_trim(p->path, term_w - 2);
    used += put_trim("$ ", 2);
    pad(term_w - used);

    draw_keys();
    at(term_h - 1, used + 1);
    flush();
}

/* ---- input ----------------------------------------------------------- */

#define K_UP    1000
#define K_DOWN  1001
#define K_RIGHT 1002
#define K_LEFT  1003
#define K_EOF    (-1)
#define K_RESIZE 1004
#define K_F      1100   /* K_F + n is Fn */

static int rawbyte(void)
{
    char c;
    int n = vfs_read(con, &c, 1);
    if (n <= 0)
        return K_EOF;
    return (unsigned char)c;
}

static void set_size(int w, int h)
{
    if (w >= 40 && w <= 300)
        term_w = w;
    if (h >= 10 && h <= 100)
        term_h = h;
}

/* One key. Three other things arrive in the same stream and are swallowed
   here: telnet's negotiations, telnet's window-size subnegotiation, and the
   terminal's answer to "where is the cursor" — which is how a screen that
   speaks no telnet still says how wide it is. */
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
            if (b == 250) {             /* SB … IAC SE */
                int opt = rawbyte();
                int p[8], np = 0, prev = 0;
                for (;;) {
                    int k = rawbyte();
                    if (k == K_EOF)
                        return K_EOF;
                    if (prev == 255 && k == 240)
                        break;
                    if (k != 255 && np < 8)
                        p[np++] = k;
                    prev = k;
                }
                if (opt == 31 && np >= 4)   /* NAWS: width then height */
                    set_size((p[0] << 8) | p[1], (p[2] << 8) | p[3]);
                continue;
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

        /* Collect the parameters, then act on the final byte. */
        int p[4] = { 0, 0, 0, 0 }, np = 0;
        int d;
        for (;;) {
            d = rawbyte();
            if (d == K_EOF)
                return K_EOF;
            if (d >= '0' && d <= '9') {
                if (np < 4)
                    p[np] = p[np] * 10 + (d - '0');
                continue;
            }
            if (d == ';') {
                if (np < 3)
                    np++;
                continue;
            }
            break;
        }
        switch (d) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'R':                       /* the cursor is at row;col */
            set_size(p[1], p[0]);
            return K_RESIZE;
        /* A strip along the bottom promising F3 and F5 has to mean it. xterm
           sends the first four function keys as ESC O P Q R S and the rest as
           ESC [ n ~; other terminals send the whole run in the second form.
           F3 is missing from the first list on purpose — ESC O R and the
           cursor report end in the same letter, and the report is the one
           this program cannot do without. Terminals that send F3 the other
           way still get it. */
        case 'P': return K_F + 1;
        case 'Q': return K_F + 2;
        case 'S': return K_F + 4;
        case '~':
            switch (p[0]) {
            case 11: return K_F + 1;
            case 12: return K_F + 2;
            case 13: return K_F + 3;
            case 14: return K_F + 4;
            case 15: return K_F + 5;
            case 17: return K_F + 6;
            case 18: return K_F + 7;
            case 19: return K_F + 8;
            case 20: return K_F + 9;
            case 21: return K_F + 10;
            default: continue;
            }
        default:
            continue;
        }
    }
}

/* ---- actions --------------------------------------------------------- */

static void view(const char *path)
{
    scrlen = 0;
    put("\x1b[2J\x1b[H");
    put(B_BAR);
    put(" ");
    put(path);
    put("  — any key to return ");
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

/* A line of text, typed into the status row. The terminal is in character
   mode by now, so this program does its own echo and its own backspace —
   which is what a line discipline is, and there is not one here. */
static int prompt(const char *label, char *out, int cap)
{
    int k = 0;
    for (;;) {
        scrlen = 0;
        at(term_h - 2, 1);
        put(B_STAT);
        int used = put_trim(label, term_w);
        used += put_trim(out, term_w - used - 1);
        pad(term_w - used);
        at(term_h - 2, used + 1);
        flush();

        int c = getkey();
        if (c == K_EOF || c == 27)
            return -1;
        if (c == 13 || c == 10)
            return k;
        if ((c == 8 || c == 127) && k > 0) {
            out[--k] = 0;
            continue;
        }
        if (c >= 32 && c < 256 && k < cap - 1) {
            out[k++] = (char)c;
            out[k] = 0;
        }
    }
}

static void do_mkdir(struct panel *p)
{
    char name[64];
    name[0] = 0;
    if (prompt(" new directory: ", name, (int)sizeof(name)) <= 0) {
        draw(0);
        return;
    }
    char full[VFS_PATH_MAX];
    join(full, p->path, name);
    int r = vfs_ioctl_path(full, IOCTL_MKDIR);
    load(p);
    draw(r < 0 ? " mkdir refused" : " made it");
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
    if (telnet)
        put("\xff\xfd\x1f");            /* IAC DO NAWS: tell me your size */
    /* And the way that works on any terminal, telnet or not: drive the cursor
       past the far corner, where it stops at the real one, and ask where it
       is. Neither question blocks — if nothing answers, 80x24 stands. */
    put("\x1b[999;999H\x1b[6n\x1b[H");
    flush();
    draw(0);

    for (;;) {
        int k = getkey();
        struct panel *p = &pan[active];

        if (k == K_EOF || k == 'q' || k == K_F + 10)
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
        } else if (k == 'v' || k == K_F + 3) {
            if (p->n && !p->e[p->sel].isdir) {
                char full[VFS_PATH_MAX];
                join(full, p->path, p->e[p->sel].name);
                view(full);
                put("\x1b[2J");
            }
        } else if (k == 'c' || k == K_F + 5) {
            copy_over();
            continue;
        } else if (k == 'k' || k == K_F + 8) {
            confirm_delete(p);
            continue;
        } else if (k == K_F + 7) {
            do_mkdir(p);
            continue;
        } else if (k == 'r') {
            load(&pan[0]);
            load(&pan[1]);
        } else if (k == K_RESIZE) {
            scrlen = 0;
            put("\x1b[2J");              /* the shape changed under us */
            flush();
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
