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

static void draw_keys(void)
{
    static const char *lab[] = { "Help", "Menu", "View", "Edit", "Copy",
                                 "RenMov", "Mkdir", "Delete", "PullDn",
                                 "Quit" };
    int y, x;
    move(LINES - 1, 0);
    for (int i = 0; i < 10; i++) {
        char n[4];
        num_into(n, (unsigned long)(i + 1));
        attrset(C_NUM);
        addstr(n);
        attrset(C_BAR);
        addstr(lab[i]);
        addch(' ');
        getyx(stdscr, y, x);
        (void)y;
        if (x >= COLS)
            break;
    }
    attrset(C_BAR);
    fill_to(stdscr, COLS);
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

/* ---- actions ----------------------------------------------------------- */

static void view(const char *path)
{
    wbkgdset(stdscr, ' ' | A_NORMAL);
    wattrset(stdscr, A_NORMAL);
    werase(stdscr);
    attrset(C_BAR);
    move(0, 0);
    addstr(" ");
    addnstr(path, COLS - 24);
    addstr("  — any key to return ");
    fill_to(stdscr, COLS);
    attrset(A_NORMAL);
    move(1, 0);

    int fd = vfs_open(path);
    if (fd >= 0) {
        for (;;) {
            char buf[VFS_DATA_MAX];
            int n = vfs_read(fd, buf, VFS_DATA_MAX);
            if (n <= 0)
                break;
            /* Off the bottom of the window is dropped by the library, so a
               long file simply stops rather than scrolling; a viewer that
               pages is a different program. */
            for (int i = 0; i < n; i++)
                addch((unsigned char)buf[i]);
        }
        vfs_close(fd);
    }
    refresh();
    getch();
    curses_touchall();              /* the whole screen has to come back */
}

/* A line of text, typed into the status row. The terminal is in character
   mode, so this does its own echo and its own backspace — which is what a
   line discipline is, and there is not one here. */
static int prompt(const char *label, char *out, int cap)
{
    int k = 0;
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
                } else {
                    char full[VFS_PATH_MAX];
                    join(full, p->path, e->name);
                    view(full);
                }
            }
        } else if (k == 'v' || k == KEY_F(3)) {
            if (p->n && !p->e[p->sel].isdir) {
                char full[VFS_PATH_MAX];
                join(full, p->path, p->e[p->sel].name);
                view(full);
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
