#pragma once
#include "lib.h"

/* curses.h — the screen as a data structure instead of a stream of bytes.

   Every full-screen program written here so far has done the same three
   things by hand: build a buffer of escape sequences, send it, and hope the
   terminal was the size it assumed. That is the job a library has done since
   1980, and the interface it does it behind is old enough to be worth copying
   exactly rather than inventing: this is a subset of X/Open curses, with the
   real names, the real constants and the real octal key codes.

   Two ideas are worth having on their own, and both are here.

   **The virtual screen.** A program does not write to the terminal; it writes
   into an array of cells. `doupdate` then compares that array against a
   second one holding what the terminal is believed to be showing, and sends
   only the difference. This is not a nicety here. One character written is
   one message and a message costs about ninety microseconds, so the cost of a
   screen is measured in bytes on the wire — and moving the cursor down one
   line in a file panel changes two rows out of twenty-four. The library is
   faster than the hand-written code it replaces *because* it knows more, not
   less.

   **A cell carries its attributes.** `chtype` is a character and its colour
   in one integer, so the colour of a thing is a property of the thing rather
   than of the moment it was drawn. The update loop emits an SGR sequence only
   when the attribute actually changes from one cell to the next, which is
   where the rest of the saving comes from.

   What is deliberately not here:

   - A window is a *view*, not a page. `newwin` returns a pen with a margin —
     clipping and an origin — and all windows draw into the one virtual
     screen. Real curses gives every window its own cell array, which is what
     makes overlapping windows able to remember what was underneath them;
     that costs a screen of memory per window, there is no malloc here, and
     the programs that exist do not overlap anything. So `wrefresh` on a
     window is not "paint this over that": it is "I am done, update the
     screen". `panel(3)` is the library that does stacking properly and it is
     not this one.
   - `printw`. There is no vsnprintf in this system.
   - `nodelay`/`halfdelay`. The console server answers a read when a key
     arrives and there is no way to ask it whether one is waiting — blocking
     is a server declining to answer yet, and it does not do partial refusals.
   - Wide characters as a separate type. `chtype` holds a code point rather
     than a byte, so box drawing and UTF-8 text work; `getch` still returns
     bytes, which is what the narrow interface promises.

   And one thing that is here and is not in the original: this library knows
   what a telnet connection is. Real curses learns the terminal from terminfo
   and the discipline from termios. Neither exists here, so `initscr` asks the
   namespace what `/dev/console` resolves to, and if the answer names a TCP
   connection it negotiates character mode itself, and asks the far end how
   big it is — in two languages, because two kinds of terminal answer. */

/* ---- types and the small constants ------------------------------------ */

#define ERR   (-1)
#define OK    0
#define TRUE  1
#define FALSE 0

#if !defined(__cplusplus) && (!defined(__STDC_VERSION__) || \
                              __STDC_VERSION__ < 202311L)
typedef int bool;
#endif

/* A character and how it looks, in thirty-two bits. The original spends
   eight on the character because it predates Unicode by a decade; this one
   spends twenty-one, because a box corner is U+250C and drawing a frame is
   the first thing anybody asks a screen library for. */
typedef unsigned int chtype;
typedef unsigned int attr_t;

#define A_CHARTEXT   0x001fffffu
#define A_NORMAL     0x00000000u
#define A_BOLD       0x00200000u
#define A_UNDERLINE  0x00400000u
#define A_REVERSE    0x00800000u
#define A_BLINK      0x01000000u
#define A_DIM        0x02000000u
#define A_COLOR      0xfc000000u
#define A_ATTRIBUTES (~A_CHARTEXT)

#define COLOR_PAIR(n)  (((chtype)(n) << 26) & A_COLOR)
#define PAIR_NUMBER(a) (int)(((chtype)(a) & A_COLOR) >> 26)
#define COLOR_PAIRS    64

#define COLOR_BLACK   0
#define COLOR_RED     1
#define COLOR_GREEN   2
#define COLOR_YELLOW  3
#define COLOR_BLUE    4
#define COLOR_MAGENTA 5
#define COLOR_CYAN    6
#define COLOR_WHITE   7
#define COLORS        8

/* The alternate character set. In the original these are the VT100
   line-drawing glyphs, reached by switching the terminal into a different
   font; here they are the code points the terminal already knows, and the
   names are kept because a program written against them ports both ways. */
#define ACS_ULCORNER ((chtype)0x250c)
#define ACS_LLCORNER ((chtype)0x2514)
#define ACS_URCORNER ((chtype)0x2510)
#define ACS_LRCORNER ((chtype)0x2518)
#define ACS_LTEE     ((chtype)0x251c)
#define ACS_RTEE     ((chtype)0x2524)
#define ACS_BTEE     ((chtype)0x2534)
#define ACS_TTEE     ((chtype)0x252c)
#define ACS_HLINE    ((chtype)0x2500)
#define ACS_VLINE    ((chtype)0x2502)
#define ACS_PLUS     ((chtype)0x253c)
#define ACS_DIAMOND  ((chtype)0x25c6)
#define ACS_CKBOARD  ((chtype)0x2592)
#define ACS_BOARD    ((chtype)0x2591)
#define ACS_BLOCK    ((chtype)0x2588)
#define ACS_DEGREE   ((chtype)0x00b0)
#define ACS_PLMINUS  ((chtype)0x00b1)
#define ACS_BULLET   ((chtype)0x00b7)
#define ACS_LARROW   ((chtype)0x2190)
#define ACS_UARROW   ((chtype)0x2191)
#define ACS_RARROW   ((chtype)0x2192)
#define ACS_DARROW   ((chtype)0x2193)

/* The key codes are the original's, in the original's octal. They begin above
   0400 so that they cannot collide with a byte, which is the whole reason
   getch returns an int. */
#define KEY_CODE_YES  0400
#define KEY_DOWN      0402
#define KEY_UP        0403
#define KEY_LEFT      0404
#define KEY_RIGHT     0405
#define KEY_HOME      0406
#define KEY_BACKSPACE 0407
#define KEY_F0        0410
#define KEY_F(n)      (KEY_F0 + (n))
#define KEY_DL        0510
#define KEY_IL        0511
#define KEY_DC        0512
#define KEY_IC        0513
#define KEY_NPAGE     0522
#define KEY_PPAGE     0523
#define KEY_ENTER     0527
#define KEY_END       0550
#define KEY_RESIZE    0632

/* How big a screen this can describe. Bigger terminals are used up to here
   and the rest is left alone, which is wrong but visibly wrong; the cost of
   raising it is eight bytes a cell. */
#define CURSES_MAXLINES 48
#define CURSES_MAXCOLS  160
#define CURSES_MAXWIN   8

typedef struct _win_st {
    int    _begy, _begx;        /* where it sits on the screen */
    int    _maxy, _maxx;        /* how big it is, in lines and columns */
    int    _cury, _curx;        /* the cursor, relative to the window */
    chtype _attrs;              /* what the next character will be drawn in */
    chtype _bkgd;               /* what erasing leaves behind */
    int    _keypad;
    int    _inuse;
} WINDOW;

/* ---- the state, which a real curses would keep in a SCREEN ------------- */

static int LINES = 24, COLS = 80;

static chtype  cur__new[CURSES_MAXLINES * CURSES_MAXCOLS];  /* being drawn */
static chtype  cur__phy[CURSES_MAXLINES * CURSES_MAXCOLS];  /* on the glass */
static WINDOW  cur__win[CURSES_MAXWIN];
static WINDOW *stdscr;

static int cur__fd = -1;        /* /dev/console, open for the whole session */
static int cur__telnet;         /* it is a TCP connection, so telnet applies */
static int cur__started;
static int cur__visible = 1;    /* whether the hardware cursor is shown */
static int cur__wy, cur__wx;    /* where refresh was told to leave it */

/* What the terminal is believed to be in the middle of: an attribute that
   need not be repeated and a cursor that need not be moved. Both start as
   "unknown", which is what forces the first of each to be sent. */
static chtype cur__attr = 0xffffffffu;
static int    cur__py = -1, cur__px = -1;

static struct { short fg, bg; } cur__pair[COLOR_PAIRS];

static int cur__pushed = -1;    /* one character of ungetch */

/* ---- bytes out --------------------------------------------------------- */

static char cur__ob[2048];
static int  cur__ol;

static void cur__flush(void)
{
    int off = 0;
    while (off < cur__ol) {
        int n = cur__ol - off;
        if (n > VFS_DATA_MAX)
            n = VFS_DATA_MAX;
        int k = vfs_write(cur__fd, cur__ob + off, n);
        if (k < 0)
            break;
        if (k == 0) { sys_yield(); continue; }
        off += k;
    }
    cur__ol = 0;
}

static void cur__ch(char c)
{
    if (cur__ol == (int)sizeof(cur__ob))
        cur__flush();
    cur__ob[cur__ol++] = c;
}

static void cur__str(const char *s)
{
    while (*s)
        cur__ch(*s++);
}

static void cur__num(int v)
{
    char t[12];
    int k = 0;
    if (v <= 0) { cur__ch('0'); return; }
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k)
        cur__ch(t[--k]);
}

/* A code point on the wire. The terminal has been speaking UTF-8 since the
   file browser needed a frame; this is where that is decided, once. */
static void cur__utf8(unsigned int v)
{
    if (v < 0x80) {
        cur__ch((char)v);
    } else if (v < 0x800) {
        cur__ch((char)(0xc0 | (v >> 6)));
        cur__ch((char)(0x80 | (v & 0x3f)));
    } else if (v < 0x10000) {
        cur__ch((char)(0xe0 | (v >> 12)));
        cur__ch((char)(0x80 | ((v >> 6) & 0x3f)));
        cur__ch((char)(0x80 | (v & 0x3f)));
    } else {
        cur__ch((char)(0xf0 | (v >> 18)));
        cur__ch((char)(0x80 | ((v >> 12) & 0x3f)));
        cur__ch((char)(0x80 | ((v >> 6) & 0x3f)));
        cur__ch((char)(0x80 | (v & 0x3f)));
    }
}

/* And the other direction, for waddstr: one character out of one to four
   bytes. A malformed byte is passed through as itself rather than rejected —
   this is a screen library, not a validator. */
static unsigned int cur__decode(const char *s, int *used)
{
    unsigned char c = (unsigned char)s[0];
    int n = 1;
    unsigned int v = c;
    if (c >= 0xf0)      { n = 4; v = c & 0x07u; }
    else if (c >= 0xe0) { n = 3; v = c & 0x0fu; }
    else if (c >= 0xc0) { n = 2; v = c & 0x1fu; }
    for (int i = 1; i < n; i++) {
        unsigned char k = (unsigned char)s[i];
        if ((k & 0xc0) != 0x80) { *used = 1; return c; }
        v = (v << 6) | (k & 0x3fu);
    }
    *used = n;
    return v;
}

static void cur__goto(int y, int x)
{
    if (cur__py == y && cur__px == x)
        return;
    cur__str("\x1b[");
    cur__num(y + 1);
    cur__ch(';');
    cur__num(x + 1);
    cur__ch('H');
    cur__py = y;
    cur__px = x;
}

/* One SGR sequence for a whole attribute word, always starting from 0. Two
   sequences would be shorter in some cases and much longer to reason about in
   all of them, and this one is sent only when the attribute changes. */
static void cur__setattr(chtype a)
{
    a &= A_ATTRIBUTES;
    if (a == cur__attr)
        return;
    cur__attr = a;
    cur__str("\x1b[0");
    if (a & A_BOLD)      cur__str(";1");
    if (a & A_DIM)       cur__str(";2");
    if (a & A_UNDERLINE) cur__str(";4");
    if (a & A_BLINK)     cur__str(";5");
    if (a & A_REVERSE)   cur__str(";7");
    int p = PAIR_NUMBER(a);
    if (p > 0 && p < COLOR_PAIRS) {
        if (cur__pair[p].fg >= 0) {
            cur__str(";3");
            cur__num(cur__pair[p].fg);
        }
        if (cur__pair[p].bg >= 0) {
            cur__str(";4");
            cur__num(cur__pair[p].bg);
        }
    }
    cur__ch('m');
}

/* ---- windows ----------------------------------------------------------- */

static inline int cur__ok(const WINDOW *w) { return w && w->_inuse; }

static inline WINDOW *newwin(int nlines, int ncols, int begin_y, int begin_x)
{
    if (nlines <= 0) nlines = LINES - begin_y;
    if (ncols  <= 0) ncols  = COLS  - begin_x;
    if (begin_y < 0 || begin_x < 0)
        return 0;
    for (int i = 0; i < CURSES_MAXWIN; i++) {
        WINDOW *w = &cur__win[i];
        if (w->_inuse)
            continue;
        w->_inuse = 1;
        w->_begy = begin_y; w->_begx = begin_x;
        w->_maxy = nlines;  w->_maxx = ncols;
        w->_cury = 0;       w->_curx = 0;
        w->_attrs = A_NORMAL;
        w->_bkgd  = ' ';
        w->_keypad = 0;
        return w;
    }
    return 0;                       /* eight is all there are */
}

static inline int delwin(WINDOW *w)
{
    if (!cur__ok(w) || w == stdscr)
        return ERR;
    w->_inuse = 0;
    return OK;
}

static inline int mvwin(WINDOW *w, int y, int x)
{
    if (!cur__ok(w)) return ERR;
    w->_begy = y; w->_begx = x;
    return OK;
}

static inline int wresize(WINDOW *w, int nlines, int ncols)
{
    if (!cur__ok(w)) return ERR;
    w->_maxy = nlines; w->_maxx = ncols;
    return OK;
}

#define getmaxyx(w, y, x) ((void)((y) = (w)->_maxy, (x) = (w)->_maxx))
#define getbegyx(w, y, x) ((void)((y) = (w)->_begy, (x) = (w)->_begx))
#define getyx(w, y, x)    ((void)((y) = (w)->_cury, (x) = (w)->_curx))

/* ---- putting things into the virtual screen ---------------------------- */

static inline int wmove(WINDOW *w, int y, int x)
{
    if (!cur__ok(w) || y < 0 || x < 0 || y >= w->_maxy || x > w->_maxx)
        return ERR;
    w->_cury = y;
    w->_curx = x;
    return OK;
}

static inline int move(int y, int x) { return wmove(stdscr, y, x); }

/* The one place a character actually lands. Everything above is arithmetic
   about where. Off the window, or off the screen, is silently dropped:
   clipping is the service a window is for. */
static inline int waddch(WINDOW *w, chtype ch)
{
    if (!cur__ok(w))
        return ERR;
    unsigned int c = ch & A_CHARTEXT;
    chtype a = (ch & A_ATTRIBUTES) ? (ch & A_ATTRIBUTES) : w->_attrs;

    if (c == '\n') {
        w->_curx = 0;
        if (w->_cury + 1 < w->_maxy)
            w->_cury++;
        return OK;
    }
    if (c == '\r') { w->_curx = 0; return OK; }

    if (w->_curx >= w->_maxx || w->_cury >= w->_maxy)
        return OK;
    int y = w->_begy + w->_cury, x = w->_begx + w->_curx;
    if (y >= 0 && y < LINES && x >= 0 && x < COLS)
        cur__new[y * CURSES_MAXCOLS + x] = c | a;
    w->_curx++;
    return OK;
}

static inline int addch(chtype ch) { return waddch(stdscr, ch); }

static inline int mvwaddch(WINDOW *w, int y, int x, chtype ch)
{
    if (wmove(w, y, x) == ERR) return ERR;
    return waddch(w, ch);
}

static inline int mvaddch(int y, int x, chtype ch)
{
    return mvwaddch(stdscr, y, x, ch);
}

/* n counts characters, not bytes — which is the same distinction the panel
   code used to make for itself, in the one place it can now be made once. */
static inline int waddnstr(WINDOW *w, const char *s, int n)
{
    if (!cur__ok(w) || !s)
        return ERR;
    for (int i = 0; s[i] && (n < 0 || n > 0); ) {
        int used = 1;
        unsigned int c = cur__decode(s + i, &used);
        waddch(w, c);
        i += used;
        if (n > 0)
            n--;
    }
    return OK;
}

static inline int waddstr(WINDOW *w, const char *s)
{
    return waddnstr(w, s, -1);
}

static inline int addstr(const char *s) { return waddnstr(stdscr, s, -1); }
static inline int addnstr(const char *s, int n) { return waddnstr(stdscr, s, n); }

static inline int mvwaddnstr(WINDOW *w, int y, int x, const char *s, int n)
{
    if (wmove(w, y, x) == ERR) return ERR;
    return waddnstr(w, s, n);
}

static inline int mvwaddstr(WINDOW *w, int y, int x, const char *s)
{
    return mvwaddnstr(w, y, x, s, -1);
}

static inline int mvaddstr(int y, int x, const char *s)
{
    return mvwaddnstr(stdscr, y, x, s, -1);
}

static inline int mvaddnstr(int y, int x, const char *s, int n)
{
    return mvwaddnstr(stdscr, y, x, s, n);
}

/* ---- attributes and colour --------------------------------------------- */

static inline int wattrset(WINDOW *w, chtype a)
{
    if (!cur__ok(w)) return ERR;
    w->_attrs = a & A_ATTRIBUTES;
    return OK;
}

static inline int wattron(WINDOW *w, chtype a)
{
    if (!cur__ok(w)) return ERR;
    /* Setting a colour pair replaces the old one rather than mixing with it,
       which is what the original does and the only thing that makes sense. */
    if (a & A_COLOR)
        w->_attrs &= ~A_COLOR;
    w->_attrs |= a & A_ATTRIBUTES;
    return OK;
}

static inline int wattroff(WINDOW *w, chtype a)
{
    if (!cur__ok(w)) return ERR;
    w->_attrs &= ~(a & A_ATTRIBUTES);
    return OK;
}

static inline int attrset(chtype a) { return wattrset(stdscr, a); }
static inline int attron(chtype a)  { return wattron(stdscr, a); }
static inline int attroff(chtype a) { return wattroff(stdscr, a); }

static inline int wbkgdset(WINDOW *w, chtype ch)
{
    if (!cur__ok(w)) return ERR;
    w->_bkgd = ch;
    return OK;
}

static inline int has_colors(void) { return TRUE; }

static inline int start_color(void)
{
    for (int i = 0; i < COLOR_PAIRS; i++) {
        cur__pair[i].fg = -1;
        cur__pair[i].bg = -1;
    }
    return OK;
}

static inline int init_pair(short pair, short f, short b)
{
    if (pair <= 0 || pair >= COLOR_PAIRS)
        return ERR;
    cur__pair[pair].fg = f;
    cur__pair[pair].bg = b;
    return OK;
}

/* ---- erasing, lines, frames -------------------------------------------- */

static inline int werase(WINDOW *w)
{
    if (!cur__ok(w)) return ERR;
    chtype fill = (w->_bkgd & A_CHARTEXT) |
                  ((w->_bkgd & A_ATTRIBUTES) ? (w->_bkgd & A_ATTRIBUTES)
                                             : w->_attrs);
    for (int y = 0; y < w->_maxy; y++)
        for (int x = 0; x < w->_maxx; x++) {
            int ay = w->_begy + y, ax = w->_begx + x;
            if (ay >= 0 && ay < LINES && ax >= 0 && ax < COLS)
                cur__new[ay * CURSES_MAXCOLS + ax] = fill;
        }
    w->_cury = w->_curx = 0;
    return OK;
}

static inline int erase(void) { return werase(stdscr); }
static inline int wclear(WINDOW *w) { return werase(w); }
static inline int clear(void) { return werase(stdscr); }

static inline int wclrtoeol(WINDOW *w)
{
    if (!cur__ok(w)) return ERR;
    int y = w->_cury, x = w->_curx;
    chtype fill = (w->_bkgd & A_CHARTEXT) | w->_attrs;
    for (int i = w->_curx; i < w->_maxx; i++) {
        w->_curx = i;
        waddch(w, fill);
    }
    w->_cury = y; w->_curx = x;
    return OK;
}

static inline int clrtoeol(void) { return wclrtoeol(stdscr); }

static inline int whline(WINDOW *w, chtype ch, int n)
{
    if (!cur__ok(w)) return ERR;
    int y = w->_cury, x = w->_curx;
    for (int i = 0; i < n; i++)
        waddch(w, ch ? ch : ACS_HLINE);
    w->_cury = y; w->_curx = x;
    return OK;
}

static inline int wvline(WINDOW *w, chtype ch, int n)
{
    if (!cur__ok(w)) return ERR;
    int y = w->_cury, x = w->_curx;
    for (int i = 0; i < n && y + i < w->_maxy; i++)
        mvwaddch(w, y + i, x, ch ? ch : ACS_VLINE);
    w->_cury = y; w->_curx = x;
    return OK;
}

static inline int mvwhline(WINDOW *w, int y, int x, chtype ch, int n)
{
    if (wmove(w, y, x) == ERR) return ERR;
    return whline(w, ch, n);
}

static inline int mvwvline(WINDOW *w, int y, int x, chtype ch, int n)
{
    if (wmove(w, y, x) == ERR) return ERR;
    return wvline(w, ch, n);
}

static inline int hline(chtype ch, int n) { return whline(stdscr, ch, n); }
static inline int vline(chtype ch, int n) { return wvline(stdscr, ch, n); }

static inline int wborder(WINDOW *w, chtype ls, chtype rs, chtype ts,
                          chtype bs, chtype tl, chtype tr, chtype bl,
                          chtype br)
{
    if (!cur__ok(w)) return ERR;
    if (!ls) ls = ACS_VLINE;
    if (!rs) rs = ACS_VLINE;
    if (!ts) ts = ACS_HLINE;
    if (!bs) bs = ACS_HLINE;
    if (!tl) tl = ACS_ULCORNER;
    if (!tr) tr = ACS_URCORNER;
    if (!bl) bl = ACS_LLCORNER;
    if (!br) br = ACS_LRCORNER;

    int h = w->_maxy, wd = w->_maxx;
    for (int x = 1; x < wd - 1; x++) {
        mvwaddch(w, 0, x, ts);
        mvwaddch(w, h - 1, x, bs);
    }
    for (int y = 1; y < h - 1; y++) {
        mvwaddch(w, y, 0, ls);
        mvwaddch(w, y, wd - 1, rs);
    }
    mvwaddch(w, 0, 0, tl);
    mvwaddch(w, 0, wd - 1, tr);
    mvwaddch(w, h - 1, 0, bl);
    mvwaddch(w, h - 1, wd - 1, br);
    return OK;
}

static inline int box(WINDOW *w, chtype verch, chtype horch)
{
    return wborder(w, verch, verch, horch, horch, 0, 0, 0, 0);
}

/* ---- the update: the only place bytes are sent ------------------------- */

/* How many unchanged cells are worth writing over rather than skipping. A
   cursor move is about six bytes, so anything shorter than that is cheaper
   to redraw than to jump over — the original curses computes this from the
   terminal's own capabilities, which is a luxury of having terminfo. */
#define CURSES_GAP 6

static inline int doupdate(void)
{
    if (!cur__started)
        return ERR;
    for (int y = 0; y < LINES; y++) {
        const chtype *nw = cur__new + y * CURSES_MAXCOLS;
        chtype *ph = cur__phy + y * CURSES_MAXCOLS;

        /* The last cell of the last line is not written, ever.

           Terminals disagree about what happens when a character lands in the
           final column: some leave the cursor there with the wrap pending
           until the next character, which is harmless, and some wrap at once
           — which on the bottom line means the screen scrolls. Everything
           afterwards is then one row from where this library believes it is,
           and since it sends only differences, it never finds out. The
           picture that produces is a screen that looks doubled.

           The original curses has the same hole and closes it the same way:
           the corner is left blank unless the terminal advertises a way to
           insert a character there. This one always leaves it, and claims it
           was written so that no later update tries again. */
        int last = (y == LINES - 1) ? COLS - 1 : COLS;
        if (y == LINES - 1 && COLS > 0)
            ph[COLS - 1] = nw[COLS - 1];

        int x = 0;
        while (x < last) {
            if (nw[x] == ph[x]) { x++; continue; }

            /* Find where this run of differences ends, allowing a few
               identical cells inside it. */
            int end = x + 1, gap = 0;
            for (int j = x + 1; j < last; j++) {
                if (nw[j] != ph[j]) { end = j + 1; gap = 0; }
                else if (++gap > CURSES_GAP) break;
            }

            cur__goto(y, x);
            for (int j = x; j < end; j++) {
                cur__setattr(nw[j]);
                cur__utf8(nw[j] & A_CHARTEXT);
                ph[j] = nw[j];
            }
            cur__px = end;
            /* A character written in the last column of any other line leaves
               the cursor where terminals disagree about. Forget where it is,
               and the next thing sent will be an absolute move. */
            if (end >= COLS)
                cur__py = -1;
            x = end;
        }
    }

    if (cur__visible)
        cur__goto(cur__wy, cur__wx);
    cur__flush();
    return OK;
}

static inline int wnoutrefresh(WINDOW *w)
{
    if (!cur__ok(w)) return ERR;
    /* All windows share one virtual screen, so there is nothing to copy —
       only the note of where this window would like the cursor left. */
    cur__wy = w->_begy + w->_cury;
    cur__wx = w->_begx + w->_curx;
    if (cur__wy >= LINES) cur__wy = LINES - 1;
    if (cur__wx >= COLS)  cur__wx = COLS - 1;
    return OK;
}

static inline int wrefresh(WINDOW *w)
{
    if (wnoutrefresh(w) == ERR) return ERR;
    return doupdate();
}

static inline int refresh(void) { return wrefresh(stdscr); }

/* Throw away what is believed about the terminal, so the next update writes
   the whole screen. What the original spells clearok(curscr, TRUE). */
static inline int curses_touchall(void)
{
    for (int i = 0; i < CURSES_MAXLINES * CURSES_MAXCOLS; i++)
        cur__phy[i] = 0xffffffffu;
    cur__attr = 0xffffffffu;
    cur__py = cur__px = -1;
    return OK;
}

/* ---- input -------------------------------------------------------------- */

static inline int cur__byte(void)
{
    char c;
    int n = vfs_read(cur__fd, &c, 1);
    if (n <= 0)
        return ERR;
    return (unsigned char)c;
}

static inline int ungetch(int ch) { cur__pushed = ch; return OK; }

static void cur__resize(int w, int h)
{
    if (w >= 20 && w <= CURSES_MAXCOLS)  COLS  = w;
    if (h >= 5  && h <= CURSES_MAXLINES) LINES = h;
    if (stdscr) {
        stdscr->_maxy = LINES;
        stdscr->_maxx = COLS;
    }
    curses_touchall();
}

/* One key.

   Four other things arrive in the same stream and are dealt with here rather
   than by the program: telnet's option negotiations, telnet's window-size
   subnegotiation, the terminal's answer to "where is the cursor", and the
   escape sequences that arrow and function keys are made of. A program that
   calls getch sees a key or a KEY_ code; that is the entire point of the
   function, and of keypad(). */
static inline int wgetch(WINDOW *w)
{
    if (cur__pushed >= 0) {
        int c = cur__pushed;
        cur__pushed = -1;
        return c;
    }
    int decode = cur__ok(w) ? w->_keypad : 1;

    for (;;) {
        int c = cur__byte();
        if (c == ERR)
            return ERR;

        if (c == 255) {                     /* IAC */
            int b = cur__byte();
            if (b == ERR) return ERR;
            if (b == 255) return 255;       /* a literal 0xff */
            if (b >= 251 && b <= 254) {     /* WILL/WONT/DO/DONT + option */
                if (cur__byte() == ERR) return ERR;
                continue;
            }
            if (b == 250) {                 /* SB … IAC SE */
                int opt = cur__byte(), p[8], np = 0, prev = 0;
                for (;;) {
                    int k = cur__byte();
                    if (k == ERR) return ERR;
                    if (prev == 255 && k == 240) break;
                    if (k != 255 && np < 8) p[np++] = k;
                    prev = k;
                }
                if (opt == 31 && np >= 4) { /* NAWS: width then height */
                    cur__resize((p[0] << 8) | p[1], (p[2] << 8) | p[3]);
                    return KEY_RESIZE;
                }
                continue;
            }
            continue;
        }

        if (c != 27 || !decode)
            return c;

        int b = cur__byte();
        if (b == ERR) return ERR;
        if (b != '[' && b != 'O')
            return 27;

        int p[4] = { 0, 0, 0, 0 }, np = 0, d;
        for (;;) {
            d = cur__byte();
            if (d == ERR) return ERR;
            if (d >= '0' && d <= '9') {
                if (np < 4) p[np] = p[np] * 10 + (d - '0');
                continue;
            }
            if (d == ';') { if (np < 3) np++; continue; }
            break;
        }

        switch (d) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case 'R':                           /* the cursor is at row;col */
            cur__resize(p[1], p[0]);
            return KEY_RESIZE;
        /* xterm sends the first four function keys as ESC O P Q R S and the
           rest as ESC [ n ~; other terminals send the whole run in the second
           form. F3 is missing from the first list on purpose: ESC O R and the
           cursor report end in the same letter, and the report is the one no
           program here can do without. */
        case 'P': return KEY_F(1);
        case 'Q': return KEY_F(2);
        case 'S': return KEY_F(4);
        case '~':
            switch (p[0]) {
            case 1:  return KEY_HOME;
            case 2:  return KEY_IC;
            case 3:  return KEY_DC;
            case 4:  return KEY_END;
            case 5:  return KEY_PPAGE;
            case 6:  return KEY_NPAGE;
            case 11: return KEY_F(1);
            case 12: return KEY_F(2);
            case 13: return KEY_F(3);
            case 14: return KEY_F(4);
            case 15: return KEY_F(5);
            case 17: return KEY_F(6);
            case 18: return KEY_F(7);
            case 19: return KEY_F(8);
            case 20: return KEY_F(9);
            case 21: return KEY_F(10);
            case 23: return KEY_F(11);
            case 24: return KEY_F(12);
            default: continue;
            }
        default:
            continue;
        }
    }
}

static inline int getch(void) { return wgetch(stdscr); }

static inline int keypad(WINDOW *w, int on)
{
    if (!cur__ok(w)) return ERR;
    w->_keypad = on;
    return OK;
}

/* ---- modes, and the terminal this happens to have ---------------------- */

/* Is the console a TCP connection? Ask the namespace: resolve reports the
   name the serving task should be asked about, and only a connection is
   called /net/tcp/N. This is what terminfo would be answering if there were
   a terminal database, and it is a better answer than one: it is not a guess
   about what the far end probably is. */
static inline int cur__is_connection(void)
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

/* WILL ECHO and WILL SUPPRESS-GO-AHEAD together are what a telnet client
   reads as "the far end is a program: send me every character and do not echo
   it yourself". That is cbreak() and noecho() in one negotiation, because a
   telnet client is the line discipline this system does not have. */
static inline void cur__rawmode(int on)
{
    if (!cur__telnet)
        return;
    cur__ch((char)255); cur__ch((char)(on ? 251 : 252)); cur__ch((char)1);
    cur__ch((char)255); cur__ch((char)(on ? 251 : 252)); cur__ch((char)3);
    cur__flush();
}

static inline int cbreak(void)   { cur__rawmode(1); return OK; }
static inline int noecho(void)   { return OK; }   /* the same negotiation */
static inline int echo(void)     { return OK; }
static inline int nocbreak(void) { cur__rawmode(0); return OK; }
static inline int raw(void)      { return cbreak(); }
static inline int noraw(void)    { return nocbreak(); }

static inline int curs_set(int visibility)
{
    int was = cur__visible;
    cur__visible = visibility != 0;
    cur__str(cur__visible ? "\x1b[?25h" : "\x1b[?25l");
    cur__flush();
    return was;
}

static inline int beep(void) { cur__ch((char)7); cur__flush(); return OK; }

static inline int napms(int ms)
{
    unsigned long until = pticks() + (unsigned long)ms * 10000UL;
    while (pticks() < until)
        sys_yield();
    return OK;
}

static inline int isendwin(void) { return !cur__started; }

/* Open the screen. Everything a terminal has to be told, and everything it
   has to be asked, happens here — including the two questions about size,
   whose answers arrive later through getch like any other key. Neither can
   block: unanswered, 80x24 stands, which is what a serial line does. */
static inline WINDOW *initscr(void)
{
    cur__fd = vfs_open("/dev/console");
    if (cur__fd < 0)
        return 0;
    cur__telnet = cur__is_connection();

    for (int i = 0; i < CURSES_MAXWIN; i++)
        cur__win[i]._inuse = 0;
    start_color();

    stdscr = newwin(LINES, COLS, 0, 0);
    if (!stdscr)
        return 0;
    stdscr->_keypad = 1;
    cur__started = 1;

    for (int i = 0; i < CURSES_MAXLINES * CURSES_MAXCOLS; i++)
        cur__new[i] = ' ';
    curses_touchall();

    cur__str("\x1b[2J");
    cur__attr = 0xffffffffu;
    cur__py = cur__px = -1;
    if (cur__telnet)
        cur__str("\xff\xfd\x1f");           /* IAC DO NAWS: tell me your size */
    /* And the way that works on any terminal at all: drive the cursor past
       the far corner, where it stops at the real one, and ask where it
       ended up. */
    cur__str("\x1b[999;999H\x1b[6n\x1b[H");
    cur__flush();
    /* The screen was just cleared, so what it holds is known exactly. */
    for (int i = 0; i < CURSES_MAXLINES * CURSES_MAXCOLS; i++)
        cur__phy[i] = ' ' | A_NORMAL;
    cur__attr = A_NORMAL;
    return stdscr;
}

/* Step off the screen without giving up the terminal: for running somebody
   else's program, which will write to the same console and knows nothing
   about any of this. The connection stays open and the negotiation stays in
   force — only the picture is surrendered. curses_resume takes it back by
   admitting it knows nothing about what is on the glass. */
static inline void curses_suspend(void)
{
    cur__str("\x1b[0m\x1b[?25h\x1b[2J\x1b[H");
    cur__flush();
    cur__attr = 0xffffffffu;
    cur__py = cur__px = -1;
}

static inline void curses_resume(void)
{
    curses_touchall();
    cur__str(cur__visible ? "\x1b[?25h" : "\x1b[?25l");
    cur__flush();
}

/* Give the terminal back the way it was found: cursor showing, no colour, no
   negotiation outstanding, and a blank screen — the shell that started this
   program is about to print its prompt into it. */
static inline int endwin(void)
{
    if (!cur__started)
        return ERR;
    cur__str("\x1b[0m\x1b[?25h\x1b[2J\x1b[H");
    cur__flush();
    cur__rawmode(0);
    vfs_close(cur__fd);
    cur__fd = -1;
    cur__started = 0;
    return OK;
}
