/* mc.c — two panels, in colour, written against curses.

   The program used to build its own screen: an eight-kilobyte buffer of
   escape sequences, a hand-rolled column counter that knew UTF-8, an escape
   parser for the arrow keys, and a telnet negotiation to get character mode.
   All of that was right, and none of it was about files. It now lives in
   prog/curses.h, where the next full-screen program can have it for an
   #include, and what is left here is the part that is about files.

   The move was not free of consequence — it made the program *faster*. The
   library keeps a copy of what the terminal is showing and sends only the
   cells that differ, and one character written is one message costing about
   ninety microseconds. Moving the cursor down one row changes two rows of
   twenty-four, so that is what goes out.

   Nothing in it is privileged. It opens /dev/console and reads and writes it;
   it opens directories and reads them. That is the same five calls `cat`
   uses.

   usage: /BIN/MC.ELF [left] [right]                                       */
#include "curses.h"
#include "spawn.h"

#define MAXENT 48
#define NAMEW  64
#define SIZEW  8

struct ent {
    char          name[NAMEW];
    unsigned long size;
    int           isdir;
};

struct panel {
    char       path[VFS_PATH_MAX];
    struct ent e[MAXENT];
    int        n, sel, top;
    WINDOW    *w;
};

static struct panel pan[2];
static int active;

/* Colour is a pair number and a name, once, instead of an escape sequence
   everywhere. Bright is A_BOLD over the same pair, which is what a terminal
   means by it. */
enum { P_PANEL = 1, P_FRAME, P_PROG, P_SEL, P_HEAD, P_HOT, P_NUM, P_STAT };

#define C_PANEL COLOR_PAIR(P_PANEL)
#define C_FRAME (COLOR_PAIR(P_FRAME) | A_BOLD)
#define C_DIR   (COLOR_PAIR(P_PANEL) | A_BOLD)
#define C_PROG  (COLOR_PAIR(P_PROG) | A_BOLD)
#define C_SEL   COLOR_PAIR(P_SEL)
#define C_HEAD  (COLOR_PAIR(P_HEAD) | A_BOLD)
#define C_BAR   COLOR_PAIR(P_SEL)
#define C_HOT   (COLOR_PAIR(P_HOT) | A_BOLD)
#define C_NUM   COLOR_PAIR(P_NUM)
#define C_STAT  COLOR_PAIR(P_STAT)

static void colours(void)
{
    start_color();
    init_pair(P_PANEL, COLOR_WHITE,  COLOR_BLUE);
    init_pair(P_FRAME, COLOR_CYAN,   COLOR_BLUE);
    init_pair(P_PROG,  COLOR_GREEN,  COLOR_BLUE);
    init_pair(P_SEL,   COLOR_BLACK,  COLOR_CYAN);
    init_pair(P_HEAD,  COLOR_YELLOW, COLOR_BLUE);
    init_pair(P_HOT,   COLOR_YELLOW, COLOR_CYAN);
    init_pair(P_NUM,   COLOR_BLACK,  COLOR_WHITE);
    init_pair(P_STAT,  COLOR_WHITE,  COLOR_BLACK);
}

/* ---- reading a directory ---------------------------------------------- */

/* One line of a listing. Two fields and then the name to the end of the line,
   spaces and all, which is a shape that needs no rules — and why the format
   was changed before this program was written. */
static void take(struct panel *p, const char *line)
{
    if ((line[0] != 'd' && line[0] != '-') || p->n >= MAXENT)
        return;
    struct ent e;
    e.isdir = line[0] == 'd';
    const char *q = line + 2;
    unsigned long sz = 0;
    while (*q >= '0' && *q <= '9')
        sz = sz * 10 + (unsigned long)(*q++ - '0');
    e.size = sz;
    if (*q == ' ')
        q++;
    int j = 0;
    while (*q && j < NAMEW - 1)
        e.name[j++] = *q++;
    e.name[j] = 0;
    /* A directory's entry for itself is on the disk and is noise in a panel;
       the one for its parent is how you leave. */
    if (!e.name[0] || (e.name[0] == '.' && e.name[1] == 0))
        return;
    /* A mount point the server also knows about is one name, not two. */
    for (int i = 0; i < p->n; i++)
        if (peq(p->e[i].name, e.name))
            return;
    /* pcpy, not an assignment: a struct copy becomes a call to memcpy, and
       there is no libc under a program on this disk. */
    pcpy(&p->e[p->n++], &e, (int)sizeof(e));
}

static void load(struct panel *p)
{
    p->n = 0;
    p->sel = 0;
    p->top = 0;

    /* The way out belongs to the path, not to the directory.

       FAT16 keeps ".." as a real entry on the disk, so it appeared in those
       panels and nowhere else: /proc and /net are rendered by servers that
       have no reason to invent one, and a panel showing one of them had no
       way up but the left arrow. Every path except the root has a parent —
       that is arithmetic on the name — so the entry is put there first and
       the duplicate a real directory offers is dropped by `take`. */
    if (!(p->path[0] == '/' && p->path[1] == 0))
        take(p, "d 0 ..");

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
            take(p, line);
        }
    }
    vfs_close(fd);

    /* And the names the namespace grafted into this directory, which the
       server that answered for it has never heard of. */
    char extra[512];
    int n = vfs_mounts_in(p->path, extra, (int)sizeof(extra));
    k = 0;
    for (int i = 0; i < n; i++) {
        if (extra[i] != '\n') {
            if (k < (int)sizeof(line) - 1)
                line[k++] = extra[i];
            continue;
        }
        line[k] = 0;
        k = 0;
        take(p, line);
    }
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
    if (k > 1)
        k--;                    /* the slash goes too, unless it is the root */
    if (k < 1)
        k = 1;
    p->path[k] = 0;
    load(p);
}

/* ---- drawing ----------------------------------------------------------- */

static int is_prog(const char *n)
{
    int k = 0;
    while (n[k]) k++;
    return k > 4 && n[k - 4] == '.' && n[k - 3] == 'E' &&
           n[k - 2] == 'L' && n[k - 1] == 'F';
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

/* Fill to column `to` with whatever the current attribute is. A row that
   stops short leaves the terminal's own background showing through and the
   panel stops looking like a panel. */
static void fill_to(WINDOW *w, int to)
{
    int y, x;
    getyx(w, y, x);
    (void)y;
    while (x++ < to)
        waddch(w, ' ');
}

/* How many list rows a panel of this height has: its own frame takes two and
   the column titles take one. This used to be computed from the screen
   height, arrived at one row too many, and the last entry was drawn and then
   painted over by the bottom frame — a window that knows its own size cannot
   make that mistake. */
static int listh(struct panel *p) { return p->w->_maxy - 3; }

static void draw_panel(int idx)
{
    struct panel *p = &pan[idx];
    WINDOW *w = p->w;
    int inner = w->_maxx - 2;
    int nw = inner - SIZEW - 1;

    wbkgdset(w, ' ' | C_PANEL);
    wattrset(w, C_PANEL);
    werase(w);
    wattrset(w, C_FRAME);
    box(w, 0, 0);

    /* The path is let into the top frame and the selected name into the
       bottom, which is where MC puts them and is two lines saved. */
    wattrset(w, C_PANEL);
    mvwaddch(w, 0, 2, ' ');
    waddnstr(w, p->path, inner - 4);
    waddch(w, ' ');

    wattrset(w, C_HEAD);
    mvwaddstr(w, 1, 1, " Name");
    fill_to(w, 1 + nw);
    waddch(w, ' ');
    waddstr(w, "Size");
    fill_to(w, inner + 1);

    for (int r = 0; r < listh(p); r++) {
        int i = p->top + r;
        wattrset(w, C_PANEL);
        wmove(w, 2 + r, 1);
        if (i >= p->n) {
            fill_to(w, inner + 1);
            continue;
        }
        struct ent *e = &p->e[i];
        int selected = (i == p->sel && idx == active);
        wattrset(w, selected ? C_SEL
                             : e->isdir ? C_DIR
                                        : is_prog(e->name) ? C_PROG : C_PANEL);
        waddch(w, e->isdir ? '/' : ' ');
        waddnstr(w, e->name, nw - 1);
        fill_to(w, 1 + nw);
        waddch(w, ' ');
        if (e->isdir) {
            waddstr(w, "DIR");
        } else {
            char num[24];
            num_into(num, e->size);
            int l = 0;
            while (num[l]) l++;
            for (int j = l; j < SIZEW; j++)
                waddch(w, ' ');
            waddstr(w, num);
        }
        fill_to(w, inner + 1);
    }

    if (p->n) {
        wattrset(w, C_PANEL);
        mvwaddch(w, w->_maxy - 1, 2, ' ');
        waddnstr(w, p->e[p->sel].name, inner - 4);
        waddch(w, ' ');
    }
    wnoutrefresh(w);
}

/* " Left  File  Command  Options  Right", with the letter you press in
   yellow — which is how the original tells you there is a menu without
   spending a line explaining it. */
static void draw_menu(void)
{
    static const char *items[] = { "Left", "File", "Command", "Options",
                                   "Right", 0 };
    attrset(C_BAR);
    move(0, 0);
    for (int i = 0; items[i]; i++) {
        addstr("  ");
        attrset(C_HOT);
        addch((chtype)items[i][0]);
        attrset(C_BAR);
        addstr(items[i] + 1);
    }
    fill_to(stdscr, COLS);
}

/* The strip along the bottom. It is the only documentation a full-screen
   program gets to show, so it lists what the keys actually do here and not
   what they do in the program this one is imitating. */
struct fkey { const char *n, *l; };

static void draw_strip(const struct fkey *k, int n)
{
    int y, x;
    move(LINES - 1, 0);
    for (int i = 0; i < n; i++) {
        attrset(C_NUM);
        addstr(k[i].n);
        attrset(C_BAR);
        addstr(k[i].l);
        addch(' ');
        getyx(stdscr, y, x);
        (void)y;
        if (x >= COLS)
            break;
    }
    attrset(C_BAR);
    fill_to(stdscr, COLS);
}

static void draw_keys(void)
{
    static const struct fkey k[] = {
        {"1","Help"}, {"2","Menu"}, {"3","View"}, {"4","Edit"}, {"5","Copy"},
        {"6","RenMov"}, {"7","Mkdir"}, {"8","Delete"}, {"9","PullDn"},
        {"10","Quit"} };
    draw_strip(k, 10);
}

static void status(const char *msg)
{
    attrset(C_STAT);
    move(LINES - 3, 0);
    addnstr(msg ? msg
                : "Hint: everything here is a file, including this.", COLS);
    fill_to(stdscr, COLS);

    move(LINES - 2, 0);
    addnstr(pan[active].path, COLS - 2);
    addstr("$ ");
    int y, x;
    getyx(stdscr, y, x);
    fill_to(stdscr, COLS);
    move(y, x);                     /* where a command line would be typed */
}

static void draw(const char *msg)
{
    wbkgdset(stdscr, ' ' | C_STAT);
    wattrset(stdscr, C_STAT);
    draw_menu();
    draw_panel(0);
    draw_panel(1);
    status(msg);
    draw_keys();
    wnoutrefresh(stdscr);
    /* The one call that sends anything, and it sends only what changed. */
    doupdate();
}

/* The windows are made to fit the screen, and the screen can change size
   under the program: telnet says so in a subnegotiation and any terminal can
   be asked. Both arrive as KEY_RESIZE. */
static void layout(void)
{
    int h = LINES - 4;              /* menu above, three rows below */
    int lw = COLS / 2;
    if (h < 4) h = 4;
    for (int i = 0; i < 2; i++) {
        if (pan[i].w)
            delwin(pan[i].w);
        pan[i].w = newwin(h, i ? COLS - lw : lw, 1, i ? lw : 0);
        pan[i].w->_keypad = 1;
    }
    for (int i = 0; i < 2; i++) {
        int lh = listh(&pan[i]);
        if (pan[i].sel >= pan[i].top + lh)
            pan[i].top = pan[i].sel - lh + 1;
    }
}

/* ---- typing a line ------------------------------------------------------ */

/* A line of text, typed into the status row. The terminal is in character
   mode, so this does its own echo and its own backspace — which is what a
   line discipline is, and there is not one here. */
static int prompt(const char *label, char *out, int cap)
{
    int k = 0;
    while (out[k])
        k++;
    curs_set(1);
    for (;;) {
        attrset(C_STAT);
        move(LINES - 3, 0);
        addnstr(label, COLS);
        addnstr(out, COLS - 2);
        int y, x;
        getyx(stdscr, y, x);
        fill_to(stdscr, COLS);
        move(y, x);
        refresh();

        int c = getch();
        if (c == ERR || c == 27) { curs_set(0); return -1; }
        if (c == 13 || c == 10 || c == KEY_ENTER) { curs_set(0); return k; }
        if ((c == 8 || c == 127 || c == KEY_BACKSPACE) && k > 0) {
            out[--k] = 0;
            continue;
        }
        if (c >= 32 && c < 256 && k < cap - 1) {
            out[k++] = (char)c;
            out[k] = 0;
        }
    }
}

/* ---- the viewer and the editor ------------------------------------------

   MC runs its viewer and its editor as separate programs. This cannot: a
   program loaded from the disk has no way to start another, because spawn
   lives in the shared user text of the kernel image and a disk program is not
   linked against it. So they live here — and they are one piece of code, of
   which the viewer is the half that does not change anything. F4 hands the
   other half over.

   The model is the filesystem's own: the whole file in one buffer. That is
   what the server does with it anyway, so an editor built on anything
   cleverer would be pretending to a file interface this system does not have
   — there is no seek and no partial write-back. An edit is therefore a
   memmove and a rebuild of the line index, which for sixteen kilobytes at the
   measured 861 MIPS is about twenty microseconds. A gap buffer would be a
   data structure carried for a saving nobody can perceive.                */

static char *ed_buf;            /* the file, and as much again to type into */
static int  ed_cap;
static int  ed_len;
static int  *ed_ls;             /* where each line starts */
static int  ed_lcap;
static int  ed_nl;
static int  ed_pos;             /* the cursor, as an offset into the file */
static int  ed_top;             /* first line shown */
static int  ed_hoff;            /* horizontal scroll, in screen columns */
static int  ed_dirty;

/* Room for `want` bytes of text and `lines` line starts. Both used to be
   fixed arrays, and the fixed one that mattered was the text: a file bigger
   than it opened read-only, because the alternative was saving a copy that
   had been silently cut off. Neither is fixed now. */
static int ed_room(int want, int lines)
{
    if (want > ed_cap) {
        int cap = ed_cap ? ed_cap : 1024;
        while (cap < want)
            cap += cap / 2 + 1;
        char *p = realloc(ed_buf, (unsigned long)cap);
        if (!p)
            return -1;
        ed_buf = p;
        ed_cap = cap;
    }
    if (lines > ed_lcap) {
        int cap = ed_lcap ? ed_lcap : 256;
        while (cap < lines)
            cap *= 2;
        int *p = realloc(ed_ls, (unsigned long)cap * sizeof(int));
        if (!p)
            return -1;
        ed_ls = p;
        ed_lcap = cap;
    }
    return 0;
}

/* A line ends where the next one starts. A file that ends in a newline has a
   last line that is empty, which is what it is and what an editor shows. */
static void ed_index(void)
{
    ed_nl = 0;
    if (ed_room(0, 1) < 0)
        return;
    ed_ls[ed_nl++] = 0;
    for (int i = 0; i < ed_len; i++)
        if (ed_buf[i] == '\n') {
            if (ed_room(0, ed_nl + 1) < 0)
                return;
            ed_ls[ed_nl++] = i + 1;
        }
}

static int ed_end(int line)
{
    return line + 1 < ed_nl ? ed_ls[line + 1] - 1 : ed_len;
}

static int ed_lineof(int pos)
{
    int lo = 0, hi = ed_nl - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (ed_ls[mid] <= pos) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* UTF-8 again, and deliberately not borrowed from the library: curses decodes
   the strings it is handed, and this walks a buffer that has no terminator
   and no promise of being text at all.

   That last part is the whole difficulty. A viewer is pointed at an
   executable sooner or later, and in an executable a byte over 0x7f is a
   byte, not the start of anything. Decoding it as though it were swallows the
   two or three bytes after it — so the display stops lining up with the file,
   which is worse than ugly. A sequence therefore counts as a character only
   if its continuation bytes are really continuation bytes; anything else is
   one byte, shown as a dot. */
static int ulen(int i)
{
    unsigned char c = (unsigned char)ed_buf[i];
    int n = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
    if (n == 1)
        return 1;
    if (i + n > ed_len)
        return 1;
    for (int k = 1; k < n; k++)
        if (((unsigned char)ed_buf[i + k] & 0xc0) != 0x80)
            return 1;
    return n;
}

static unsigned int uval(int i, int n)
{
    unsigned char c = (unsigned char)ed_buf[i];
    if (n == 1)
        return c;
    unsigned int v = (unsigned int)(c & (0xff >> (n + 1)));
    for (int k = 1; k < n; k++)
        v = (v << 6) | ((unsigned char)ed_buf[i + k] & 0x3f);
    return v;
}

static int ed_next(int pos)
{
    if (pos >= ed_len) return ed_len;
    int n = ulen(pos);
    return pos + n > ed_len ? ed_len : pos + n;
}

static int ed_prev(int pos)
{
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)ed_buf[pos] & 0xc0) == 0x80)
        pos--;
    return pos;
}

/* Two ways of counting along a line, and both are needed. Characters are what
   moving up and down should preserve; screen columns are where the cursor
   actually has to be drawn, and a tab is one of the first and eight of the
   second. */
static int ed_chars(int line, int pos)
{
    int n = 0;
    for (int i = ed_ls[line]; i < pos && i < ed_end(line); i = ed_next(i))
        n++;
    return n;
}

static int ed_scol(int line, int pos)
{
    int col = 0;
    for (int i = ed_ls[line]; i < pos && i < ed_end(line); ) {
        if (ed_buf[i] == '\t') { col = (col / 8 + 1) * 8; i++; continue; }
        col++;
        i = ed_next(i);
    }
    return col;
}

static int ed_at(int line, int chars)
{
    int i = ed_ls[line], e = ed_end(line);
    while (chars-- > 0 && i < e)
        i = ed_next(i);
    return i;
}

static int ed_load(const char *path)
{
    ed_len = ed_pos = ed_top = ed_hoff = 0;
    ed_dirty = 0;
    int fd = vfs_open(path);
    if (fd < 0)
        return -1;
    for (;;) {
        if (ed_room(ed_len + 4096, 0) < 0)
            break;
        int n = vfs_read(fd, ed_buf + ed_len, ed_cap - ed_len);
        if (n <= 0)
            break;
        ed_len += n;
    }
    vfs_close(fd);
    ed_index();
    return 0;
}

/* Create truncates and close is what puts the bytes on the disk — the same
   two facts `cp` relies on. A file that was too big to load is never written
   back: saving it would silently cut it off at sixteen kilobytes. */
static int ed_save(const char *path)
{
    int fd = vfs_create(path);
    if (fd < 0)
        return -1;
    int off = 0;
    while (off < ed_len) {
        int n = ed_len - off;
        if (n > VFS_DATA_MAX)
            n = VFS_DATA_MAX;
        int k = vfs_write(fd, ed_buf + off, n);
        if (k < 0) { vfs_close(fd); return -1; }
        if (k == 0) { sys_yield(); continue; }
        off += k;
    }
    if (vfs_close(fd) < 0)
        return -1;
    ed_dirty = 0;
    return 0;
}

static int ed_insert(const char *s, int n)
{
    if (ed_room(ed_len + n, 0) < 0)
        return -1;
    for (int i = ed_len - 1; i >= ed_pos; i--)
        ed_buf[i + n] = ed_buf[i];
    for (int i = 0; i < n; i++)
        ed_buf[ed_pos + i] = s[i];
    ed_len += n;
    ed_pos += n;
    ed_dirty = 1;
    ed_index();
    return 0;
}

static void ed_remove(int at, int n)
{
    if (at < 0 || n <= 0 || at + n > ed_len)
        return;
    for (int i = at; i + n < ed_len; i++)
        ed_buf[i] = ed_buf[i + n];
    ed_len -= n;
    if (ed_pos > ed_len)
        ed_pos = ed_len;
    ed_dirty = 1;
    ed_index();
}

static int ed_find(const char *needle, int from)
{
    int m = 0;
    while (needle[m]) m++;
    if (!m)
        return -1;
    for (int i = from; i + m <= ed_len; i++) {
        int j = 0;
        while (j < m && ed_buf[i + j] == needle[j]) j++;
        if (j == m)
            return i;
    }
    return -1;
}

/* Keep the cursor on the screen, both ways. */
static void ed_follow(int h, int follow_col)
{
    int line = ed_lineof(ed_pos);
    if (line < ed_top) ed_top = line;
    if (line >= ed_top + h) ed_top = line - h + 1;
    if (ed_top < 0) ed_top = 0;
    if (!follow_col)
        return;                 /* the viewer scrolls sideways by itself */
    int col = ed_scol(line, ed_pos);
    if (col < ed_hoff) ed_hoff = col;
    if (col >= ed_hoff + COLS) ed_hoff = col - COLS + 1;
}

static void ed_draw(const char *path, int writable, const char *msg)
{
    int h = LINES - 2;
    int line = ed_lineof(ed_pos);

    attrset(C_BAR);
    move(0, 0);
    addstr(writable ? " Edit " : " View ");
    addnstr(path, COLS - 30);
    if (ed_dirty)
        addstr(" *");
    addstr("   ");
    char n[24];
    num_into(n, (unsigned long)(line + 1));
    addstr(n);
    addstr("/");
    num_into(n, (unsigned long)ed_nl);
    addstr(n);
    if (msg) {
        addstr("   ");
        addstr(msg);
    }
    fill_to(stdscr, COLS);

    int cy = 0, cx = 0;
    for (int r = 0; r < h; r++) {
        int y = ed_top + r;
        attrset(C_PANEL);
        move(r + 1, 0);
        if (y < ed_nl) {
            int col = 0, e = ed_end(y);
            for (int i = ed_ls[y]; i < e; ) {
                int adv = 1;
                unsigned int c;
                if (ed_buf[i] == '\t') {
                    adv = (col / 8 + 1) * 8 - col;
                    c = ' ';
                    i++;
                } else {
                    int n2 = ulen(i);
                    c = uval(i, n2);
                    /* A control byte is not a character. Showing it as one is
                       how a viewer of a binary file scrolls the screen
                       sideways and rings the bell. */
                    if (c < 32 || c == 127 || (n2 == 1 && c > 126))
                        c = '.';
                    i += n2;
                }
                for (int k = 0; k < adv; k++, col++)
                    if (col >= ed_hoff && col - ed_hoff < COLS)
                        addch(c == ' ' ? ' ' : (k ? ' ' : c));
                if (col - ed_hoff >= COLS)
                    break;
            }
            if (y == line) {
                cy = r + 1;
                cx = ed_scol(y, ed_pos) - ed_hoff;
            }
        }
        fill_to(stdscr, COLS);
    }

    static const struct fkey view_keys[] = {
        {"3","Quit"}, {"4","Edit"}, {"7","Search"}, {"10","Quit"} };
    static const struct fkey edit_keys[] = {
        {"2","Save"}, {"7","Search"}, {"10","Quit"} };
    if (writable)
        draw_strip(edit_keys, 3);
    else
        draw_strip(view_keys, 4);

    if (writable && cx >= 0 && cx < COLS)
        move(cy, cx);
    else
        move(0, 0);
    refresh();
}

/* One loop for both. Returns non-zero if the file was written, which is the
   panel's cue to read the directory again: the size in it just changed. */
static int edit_file(const char *path, int writable)
{
    if (ed_load(path) < 0) {
        draw("cannot open it");
        return 0;
    }
    int h = LINES - 2, saved = 0;
    const char *msg = 0;
    char pattern[64];
    pattern[0] = 0;

    curs_set(writable ? 1 : 0);
    for (;;) {
        ed_follow(h, writable);
        ed_draw(path, writable, msg);
        msg = 0;

        int k = getch();
        int line = ed_lineof(ed_pos);

        if (k == ERR || k == KEY_F(10) ||
            (!writable && (k == 'q' || k == 27 || k == KEY_F(3))))
            break;

        switch (k) {
        case KEY_UP:
            if (line > 0)
                ed_pos = ed_at(line - 1, ed_chars(line, ed_pos));
            continue;
        case KEY_DOWN:
            if (line + 1 < ed_nl)
                ed_pos = ed_at(line + 1, ed_chars(line, ed_pos));
            continue;
        /* Sideways means two different things. With a cursor on the screen
           it means the cursor; without one — the viewer hides it — moving
           something invisible looks like nothing happening, so it means the
           window. */
        case KEY_LEFT:
            if (writable) ed_pos = ed_prev(ed_pos);
            else if ((ed_hoff -= 8) < 0) ed_hoff = 0;
            continue;
        case KEY_RIGHT:
            if (writable) ed_pos = ed_next(ed_pos);
            else ed_hoff += 8;
            continue;
        case KEY_HOME:  ed_pos = ed_ls[line]; continue;
        case KEY_END:   ed_pos = ed_end(line); continue;
        /* A page moves the text by a page, not merely the cursor: the eye
           has to land somewhere it recognises. */
        case KEY_PPAGE: {
            int col = ed_chars(line, ed_pos);
            ed_pos = ed_at(line - h < 0 ? 0 : line - h, col);
            ed_top -= h;
            if (ed_top < 0) ed_top = 0;
            continue;
        }
        case KEY_NPAGE: {
            int col = ed_chars(line, ed_pos);
            ed_pos = ed_at(line + h >= ed_nl ? ed_nl - 1 : line + h, col);
            ed_top += h;
            if (ed_top > ed_nl - 1) ed_top = ed_nl - 1;
            continue;
        }
        case KEY_F(7): {
            if (prompt(" search: ", pattern, (int)sizeof(pattern)) <= 0) {
                curs_set(writable ? 1 : 0);
                continue;
            }
            curs_set(writable ? 1 : 0);
            int at = ed_find(pattern, ed_pos + 1);
            if (at < 0)
                at = ed_find(pattern, 0);      /* round the end, once */
            if (at < 0)
                msg = "not found";
            else
                ed_pos = at;
            continue;
        }
        case KEY_F(4):
            if (!writable) {
                writable = 1;
                curs_set(1);
            }
            continue;
        case KEY_F(2):
            if (writable)
                msg = ed_save(path) < 0 ? "could not write it" : "saved";
            if (writable && !ed_dirty)
                saved = 1;
            continue;
        }

        if (!writable)
            continue;

        if (k == 13 || k == 10 || k == KEY_ENTER) {
            ed_insert("\n", 1);
        } else if (k == 8 || k == 127 || k == KEY_BACKSPACE) {
            if (ed_pos > 0) {
                int p = ed_prev(ed_pos);
                int n = ed_pos - p;
                ed_pos = p;
                ed_remove(p, n);
            }
        } else if (k == KEY_DC) {
            if (ed_pos < ed_len)
                ed_remove(ed_pos, ed_next(ed_pos) - ed_pos);
        } else if (k == 9) {
            ed_insert("\t", 1);
        } else if (k >= 32 && k < 256) {
            char c = (char)k;
            if (ed_insert(&c, 1) < 0)
                msg = "no room left";
        }
    }

    if (writable && ed_dirty) {
        /* Leaving with changes is a question, not a decision. */
        ed_draw(path, writable, "save? y / n");
        int k = getch();
        if (k == 'y' || k == 'Y')
            saved = ed_save(path) == 0;
    }
    curs_set(0);
    curses_touchall();
    free(ed_buf);  ed_buf = 0;  ed_cap = 0;
    free(ed_ls);   ed_ls  = 0;  ed_lcap = 0;
    return saved;
}

/* ---- running one ---------------------------------------------------------

   The panels can start a program now, which the README said for several
   stages they could not. The sentence was that spawn lives in the shared user
   text of the kernel image — true, and the conclusion drawn from it was
   wrong: what is out of reach is the function, not the three syscalls under
   it, and a disk program can carry its own loader the way it carries its own
   libc. prog/spawn.h is that.

   The screen is the whole difficulty. The child inherits this task's
   namespace, so /dev/console means the same connection for both, and a
   program that prints a line knows nothing about panels. So the panels get
   out of the way, the program runs, and the screen comes back when a key
   says it may — which is what MC does, and for the same reason. */
static void run_program(struct panel *p)
{
    char full[VFS_PATH_MAX];
    join(full, p->path, p->e[p->sel].name);
    char *av[1];
    av[0] = full;

    curses_suspend();
    int tid = prun(full, 1, av);
    if (tid < 0) {
        say("mc: cannot run ");
        say(full);
        say("\n");
    } else {
        sys_wait(tid);
    }
    say("\n-- any key --");
    getch();
    curses_resume();
    load(p);
}

/* ---- what the panel does with a file ------------------------------------ */

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
    if (getch() != 'y') {
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

/* ---- ------------------------------------------------------------------- */

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    if (!initscr()) {
        say("mc: no console\n");
        sys_exit();
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    colours();

    const char *l = argc > 1 ? argv[1] : "/";
    const char *r = argc > 2 ? argv[2] : "/BIN";
    for (int i = 0; l[i] && i < VFS_PATH_MAX - 1; i++) pan[0].path[i] = l[i];
    for (int i = 0; r[i] && i < VFS_PATH_MAX - 1; i++) pan[1].path[i] = r[i];
    load(&pan[0]);
    load(&pan[1]);
    layout();
    draw(0);

    for (;;) {
        int k = getch();
        struct panel *p = &pan[active];
        int lh = listh(p);

        if (k == ERR || k == 'q' || k == KEY_F(10))
            break;
        if (k == 9) {                    /* tab */
            active ^= 1;
        } else if (k == KEY_UP) {
            if (p->sel > 0) p->sel--;
            if (p->sel < p->top) p->top = p->sel;
        } else if (k == KEY_DOWN) {
            if (p->sel + 1 < p->n) p->sel++;
            if (p->sel >= p->top + lh) p->top = p->sel - lh + 1;
        } else if (k == KEY_PPAGE) {
            p->sel -= lh;
            if (p->sel < 0) p->sel = 0;
            p->top = p->sel;
        } else if (k == KEY_NPAGE) {
            p->sel += lh;
            if (p->sel >= p->n) p->sel = p->n ? p->n - 1 : 0;
            if (p->sel >= p->top + lh) p->top = p->sel - lh + 1;
        } else if (k == KEY_HOME) {
            p->sel = p->top = 0;
        } else if (k == KEY_END) {
            p->sel = p->n ? p->n - 1 : 0;
            p->top = p->sel - lh + 1;
            if (p->top < 0) p->top = 0;
        } else if (k == KEY_LEFT) {
            go_up(p);
        } else if (k == 13 || k == 10 || k == KEY_ENTER || k == KEY_RIGHT) {
            if (p->n) {
                struct ent *e = &p->e[p->sel];
                if (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == 0) {
                    go_up(p);
                } else if (e->isdir) {
                    char next[VFS_PATH_MAX];
                    join(next, p->path, e->name);
                    for (int i = 0; i < VFS_PATH_MAX; i++)
                        p->path[i] = next[i];
                    load(p);
                } else if (is_prog(e->name)) {
                    run_program(p);         /* Enter runs it, F3 reads it */
                } else {
                    char full[VFS_PATH_MAX];
                    join(full, p->path, e->name);
                    if (edit_file(full, 0))
                        load(p);
                }
            }
        } else if (k == 'v' || k == KEY_F(3) || k == 'e' || k == KEY_F(4)) {
            if (p->n && !p->e[p->sel].isdir) {
                char full[VFS_PATH_MAX];
                join(full, p->path, p->e[p->sel].name);
                if (edit_file(full, k == 'e' || k == KEY_F(4)))
                    load(p);
            }
        } else if (k == 'c' || k == KEY_F(5)) {
            copy_over();
            continue;
        } else if (k == 'k' || k == KEY_F(8)) {
            confirm_delete(p);
            continue;
        } else if (k == KEY_F(7)) {
            do_mkdir(p);
            continue;
        } else if (k == 'r') {
            load(&pan[0]);
            load(&pan[1]);
        } else if (k == KEY_RESIZE) {
            layout();
        }
        draw(0);
    }

    endwin();
    sys_exit();
}
