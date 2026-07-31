/* fat16.c — a small FAT16: paths, directories, long names, read and write. Block device is a flat region of
   a real disk, one sector at a time, through the virtio-blk driver next to it
   in the same task. It used to be a memcpy from a window of guest RAM that
   QEMU had filled with the image; the shape of the code barely changed, which
   is the whole argument for having had a blk_read() at all.

   The one structural oddity of FAT16 is that the root directory is not a
   directory. Every other one is an ordinary file — a cluster chain whose
   contents happen to be 32-byte entries — but the root is a fixed run of
   sectors outside the data area, with no chain and no entry describing it.
   Everything below is written against "the n-th sector of a directory", which
   is the one place that difference has to be known, and nowhere else. */
#include "fat16.h"
#include "blk.h"
#include "ulib.h"

#define SECSZ     BLK_SECSZ



static struct {
    uint16 bytes_per_sec;
    uint8  sec_per_clus;
    uint16 reserved;
    uint8  num_fats;
    uint16 root_entries;
    uint16 sec_per_fat;
    uint32 fat_start;
    uint32 root_start;
    uint32 root_sectors;
    uint32 data_start;
    int    ok;
} fs;

static uint16 rd16(const uint8 *p) { return p[0] | (p[1] << 8); }
static uint32 rd32(const uint8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32)p[3] << 24); }
static char   up(char c)           { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

int fat16_init(void)
{
    uint8 sec[SECSZ];
    blk_read(0, sec);
    if (sec[510] != 0x55 || sec[511] != 0xAA)
        return -1;                              /* no boot signature */

    fs.bytes_per_sec = rd16(sec + 11);
    fs.sec_per_clus  = sec[13];
    fs.reserved      = rd16(sec + 14);
    fs.num_fats      = sec[16];
    fs.root_entries  = rd16(sec + 17);
    fs.sec_per_fat   = rd16(sec + 22);

    fs.fat_start    = fs.reserved;
    fs.root_start   = fs.reserved + (uint32)fs.num_fats * fs.sec_per_fat;
    fs.root_sectors = ((uint32)fs.root_entries * 32 + SECSZ - 1) / SECSZ;
    fs.data_start   = fs.root_start + fs.root_sectors;
    fs.ok = (fs.bytes_per_sec == SECSZ && fs.sec_per_clus > 0);
    return fs.ok ? 0 : -1;
}

/* Next cluster in the chain (FAT16 entry = 2 bytes; never straddles a sector). */
static void wr16(uint8 *p, uint16 v) { p[0] = (uint8)v; p[1] = (uint8)(v >> 8); }
static void wr32(uint8 *p, uint32 v)
{
    p[0] = (uint8)v;         p[1] = (uint8)(v >> 8);
    p[2] = (uint8)(v >> 16); p[3] = (uint8)(v >> 24);
}

static uint16 fat_next(uint16 clus)
{
    uint32 off = (uint32)clus * 2;
    uint8  buf[SECSZ];
    blk_read(fs.fat_start + off / SECSZ, buf);
    return rd16(buf + (off % SECSZ));
}

/* Format an 8.3 directory entry's raw name field into "NAME.EXT". */
static void fmt_name(const uint8 *d, char out[13])
{
    int k = 0;
    for (int i = 0; i < 8 && d[i] != ' '; i++)
        out[k++] = d[i];
    int has_ext = 0;
    for (int i = 8; i < 11; i++)
        if (d[i] != ' ') has_ext = 1;
    if (has_ext) {
        out[k++] = '.';
        for (int i = 8; i < 11 && d[i] != ' '; i++)
            out[k++] = d[i];
    }
    out[k] = 0;
}

/* Turn "readme.txt" into the padded 11-byte on-disk field "README  TXT". */
static void to_83(const char *in, char out[11])
{
    for (int i = 0; i < 11; i++)
        out[i] = ' ';
    int i = 0, o = 0;
    while (in[i] && in[i] != '.' && o < 8)
        out[o++] = up(in[i++]);
    while (in[i] && in[i] != '.')
        i++;
    if (in[i] == '.') {
        i++;
        int e = 8;
        while (in[i] && e < 11)
            out[e++] = up(in[i++]);
    }
}


/* ---- directories --------------------------------------------------------
   `dir` is a first cluster, and 0 means the root — a number no real directory
   can have, since clusters 0 and 1 are reserved by the format. */

static int dir_sector(uint16 dir, uint32 n, uint32 *lba)
{
    if (dir == 0) {
        if (n >= fs.root_sectors)
            return -1;
        *lba = fs.root_start + n;
        return 0;
    }
    uint32 per = fs.sec_per_clus;
    uint16 c = dir;
    for (uint32 skip = n / per; skip; skip--) {
        c = fat_next(c);
        if (c < 2 || c >= 0xFFF8)
            return -1;                  /* past the end of the chain */
    }
    *lba = fs.data_start + (uint32)(c - 2) * per + (n % per);
    return 0;
}

/* ---- long names ---------------------------------------------------------
   An 8.3 name is eleven bytes of an alphabet chosen in 1981. A long name is
   stored beside it, in the entries *immediately before* the short one, each
   carrying thirteen UTF-16 code units and marked with attribute 0x0F — a
   combination (read-only, hidden, system, volume label) that no real file has
   and that every driver written before 1996 already skipped. That is the
   whole trick: the extension is invisible to code that does not know about
   it, which is why this filesystem could grow one without a new version.

   Three details are easy to get wrong and all three are load-bearing:

     - the entries are stored in reverse. The one marked 0x40 comes first on
       disk and carries the *last* chunk of the name;
     - a checksum of the eleven-byte short name is repeated in every long
       entry, so a driver that renames a file the old way leaves a chain that
       no longer matches and is correctly ignored rather than believed;
     - unused character slots after the terminator are 0xFFFF, not 0x0000.

   Anything above the basic plane — an emoji, say — is a surrogate pair, which
   is how a format designed around UCS-2 carries a code point invented after
   it. */

#define LFN_ATTR   0x0F
#define LFN_LAST   0x40
#define LFN_CHARS  13
#define LFN_MAXUNITS 128
#define SLOTS_PER_SEC (SECSZ / 32)

/* One sector of a directory, remembered. Slot-at-a-time access makes every
   walk linear and the code half the size; the cache is what keeps that from
   costing sixteen reads per sector. */
static uint32 dcache_lba;
static uint8  dcache[SECSZ];
static int    dcache_ok;

static int slot_ptr(uint16 dir, uint32 i, uint8 **ent)
{
    uint32 lba;
    if (dir_sector(dir, i / SLOTS_PER_SEC, &lba) < 0)
        return -1;
    if (!dcache_ok || dcache_lba != lba) {
        if (blk_read(lba, dcache) < 0)
            return -1;
        dcache_lba = lba;
        dcache_ok  = 1;
    }
    *ent = dcache + (i % SLOTS_PER_SEC) * 32;
    return 0;
}

static int slot_put(uint16 dir, uint32 i, const uint8 *ent)
{
    uint8 *p;
    if (slot_ptr(dir, i, &p) < 0)
        return -1;
    umemcpy(p, ent, 32);
    return blk_write(dcache_lba, dcache);
}

/* Every long entry repeats this, computed over the short name it belongs to. */
static uint8 sfn_sum(const uint8 *n11)
{
    uint8 s = 0;
    for (int i = 0; i < 11; i++)
        s = (uint8)(((s & 1) << 7) + (s >> 1) + n11[i]);
    return s;
}

/* ---- UTF-8 and UTF-16 --------------------------------------------------- */

static int to_utf16(const char *in, uint16 *out, int max)
{
    int n = 0;
    while (*in) {
        unsigned c = (unsigned char)*in++;
        int extra;
        if (c < 0x80)             { extra = 0; }
        else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
        else return -1;                         /* not UTF-8 */
        while (extra--) {
            unsigned k = (unsigned char)*in++;
            if ((k & 0xC0) != 0x80)
                return -1;
            c = (c << 6) | (k & 0x3F);
        }
        if (c >= 0x10000) {                     /* a surrogate pair */
            if (n + 2 > max)
                return -1;
            c -= 0x10000;
            out[n++] = (uint16)(0xD800 + (c >> 10));
            out[n++] = (uint16)(0xDC00 + (c & 0x3FF));
        } else {
            if (n + 1 > max)
                return -1;
            out[n++] = (uint16)c;
        }
    }
    return n;
}

static int to_utf8(const uint16 *in, int units, char *out, int cap)
{
    int n = 0;
    for (int i = 0; i < units; i++) {
        unsigned c = in[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < units &&
            in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (in[++i] - 0xDC00);
        }
        if (c < 0x80) {
            if (n + 1 >= cap) return -1;
            out[n++] = (char)c;
        } else if (c < 0x800) {
            if (n + 2 >= cap) return -1;
            out[n++] = (char)(0xC0 | (c >> 6));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            if (n + 3 >= cap) return -1;
            out[n++] = (char)(0xE0 | (c >> 12));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (n + 4 >= cap) return -1;
            out[n++] = (char)(0xF0 | (c >> 18));
            out[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[n] = 0;
    return n;
}

/* The thirteen units of one long entry live at three disjoint offsets, which
   is what is left of a struct that had to fit around fields it could not use:
   bytes 26 and 27 are the first-cluster field, and must read as zero so that
   an old driver following it goes nowhere. */
static const int lfn_off[LFN_CHARS] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };

static void lfn_get(const uint8 *d, uint16 *out)
{
    for (int k = 0; k < LFN_CHARS; k++)
        out[k] = (uint16)(d[lfn_off[k]] | (d[lfn_off[k] + 1] << 8));
}

static void lfn_put(uint8 *d, const uint16 *in)
{
    for (int k = 0; k < LFN_CHARS; k++) {
        d[lfn_off[k]]     = (uint8)in[k];
        d[lfn_off[k] + 1] = (uint8)(in[k] >> 8);
    }
}

/* ---- walking a directory ------------------------------------------------ */

struct dscan {
    uint16 dir;
    uint32 i;
    uint16 units[LFN_MAXUNITS];
    int    nunits;
    int    have;                /* a chain is being collected */
    uint8  sum;
    uint32 first;               /* slot the chain started at */
};

static void dscan_start(struct dscan *s, uint16 dir)
{
    s->dir = dir; s->i = 0; s->nunits = 0; s->have = 0; s->first = 0;
}

/* -> the slot index of the next short entry, or -1 at the end of the
   directory. `name` gets the long name if one was attached and survived its
   checksum, otherwise the 8.3 name. `ent` gets a copy, because the next call
   moves the sector under it. */
static int dscan_next(struct dscan *s, char *name, int cap, uint8 *ent,
                      uint32 *chain_start)
{
    for (;;) {
        uint8 *d;
        if (slot_ptr(s->dir, s->i, &d) < 0)
            return -1;
        if (d[0] == 0x00)
            return -1;                          /* nothing beyond here */

        if (d[0] == 0xE5) {
            s->have = 0;
            s->i++;
            continue;
        }
        if (d[11] == LFN_ATTR) {
            int seq = d[0] & 0x3F;
            if (d[0] & LFN_LAST) {
                s->have   = 1;
                s->sum    = d[13];
                s->nunits = 0;
                s->first  = s->i;
                for (int k = 0; k < LFN_MAXUNITS; k++)
                    s->units[k] = 0;
            }
            if (s->have && seq >= 1 && seq * LFN_CHARS <= LFN_MAXUNITS) {
                lfn_get(d, s->units + (seq - 1) * LFN_CHARS);
                if (seq * LFN_CHARS > s->nunits)
                    s->nunits = seq * LFN_CHARS;
            } else {
                s->have = 0;                    /* too long for us to hold */
            }
            s->i++;
            continue;
        }
        if (d[11] & 0x08) {                     /* the volume label */
            s->have = 0;
            s->i++;
            continue;
        }

        uint32 idx = s->i++;
        umemcpy(ent, d, 32);
        if (chain_start)
            *chain_start = s->have ? s->first : idx;

        if (s->have && s->sum == sfn_sum(ent)) {
            int u = 0;
            while (u < s->nunits && s->units[u] != 0 && s->units[u] != 0xFFFF)
                u++;
            if (to_utf8(s->units, u, name, cap) < 0)
                fmt_name(ent, name);            /* would not fit: fall back */
        } else {
            fmt_name(ent, name);                /* no chain, or it was stale */
        }
        s->have = 0;
        return (int)idx;
    }
}

/* ASCII-case-insensitive, because that is the case-folding a FAT volume
   promises and no more. Two names differing only outside ASCII are two names. */
static int name_eq(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = up(*a), y = up(*b);
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
}

/* -> the slot of the short entry for `name`, or -1. */
static int dir_lookup(uint16 dir, const char *name, uint8 *ent, uint32 *chain)
{
    struct dscan s;
    char got[FAT_NAME_MAX];
    dscan_start(&s, dir);
    for (;;) {
        uint8 e[32];
        uint32 c;
        int idx = dscan_next(&s, got, (int)sizeof(got), e, &c);
        if (idx < 0)
            return -1;
        if (name_eq(got, name)) {
            if (ent) umemcpy(ent, e, 32);
            if (chain) *chain = c;
            return idx;
        }
    }
}

/* A run of `count` consecutive slots that are free or deleted. Consecutive
   matters: a long name and the entry it belongs to have to be adjacent, and
   in that order. */
static int dir_run(uint16 dir, int count, uint32 *start)
{
    uint32 run = 0, begin = 0;
    for (uint32 i = 0; ; i++) {
        uint8 *d;
        if (slot_ptr(dir, i, &d) < 0)
            return -1;                  /* the directory cannot grow */
        if (d[0] == 0x00 || d[0] == 0xE5) {
            if (run == 0)
                begin = i;
            if (++run >= (uint32)count) {
                *start = begin;
                return 0;
            }
        } else {
            run = 0;
        }
    }
}

/* ---- making a short name ------------------------------------------------
   Every file still has one, because every reader expects one. It is derived
   rather than chosen: the printable ASCII of the long name, upper-cased, cut
   to six characters, and then ~1, ~2 … until it is unique. Anything the
   short alphabet cannot hold becomes an underscore, which is what makes
   ПРИВЕТ.TXT and ПРИМЕР.TXT need the tail to tell them apart. */
static void short_name_for(uint16 dir, const char *name, char out[11])
{
    const char *dot = 0;
    for (const char *p = name; *p; p++)
        if (*p == '.')
            dot = p;

    char base[8];
    int  b = 0;
    for (const char *p = name; *p && (!dot || p < dot) && b < 6; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '.')
            continue;
        base[b++] = (c < 0x80) ? up((char)c) : '_';
    }
    if (b == 0)
        base[b++] = '_';

    char ext[3];
    int  e = 0;
    if (dot)
        for (const char *p = dot + 1; *p && e < 3; p++) {
            unsigned char c = (unsigned char)*p;
            ext[e++] = (c < 0x80) ? up((char)c) : '_';
        }

    for (int n = 1; n <= 99; n++) {
        for (int i = 0; i < 11; i++)
            out[i] = ' ';
        int k = 0;
        for (; k < b && k < (n < 10 ? 6 : 5); k++)
            out[k] = base[k];
        out[k++] = '~';
        if (n >= 10)
            out[k++] = (char)('0' + n / 10);
        out[k++] = (char)('0' + n % 10);
        for (int i = 0; i < e; i++)
            out[8 + i] = ext[i];

        /* Unique among the short names actually on disk. */
        struct dscan s;
        char got[FAT_NAME_MAX];
        dscan_start(&s, dir);
        int clash = 0;
        for (;;) {
            uint8 ent[32];
            int idx = dscan_next(&s, got, (int)sizeof(got), ent, 0);
            if (idx < 0)
                break;
            if (umemcmp(ent, out, 11) == 0) {
                clash = 1;
                break;
            }
        }
        if (!clash)
            return;
    }
}

/* Does this name already fit the old alphabet exactly? Then it needs no long
   entries at all, and a volume full of ordinary names looks exactly as it did
   before this stage. */
static int fits_83(const char *name, char out[11])
{
    int i = 0, b = 0, e = 0;
    for (; name[i] && name[i] != '.'; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 0x80 || c == ' ')
            return 0;
        if (c >= 'a' && c <= 'z')
            return 0;                           /* lower case needs a long name */
        b++;
    }
    if (b == 0 || b > 8)
        return 0;
    if (name[i] == '.') {
        for (i++; name[i]; i++) {
            unsigned char c = (unsigned char)name[i];
            if (c >= 0x80 || c == ' ' || c == '.')
                return 0;
            if (c >= 'a' && c <= 'z')
                return 0;
            e++;
        }
        if (e == 0 || e > 3)
            return 0;
    }
    to_83(name, out);
    return 1;
}

/* ---- paths -------------------------------------------------------------- */

/* Walk a path down to the directory that should contain its last component.
   "/DOCS/NOTE.TXT" leaves `dir` at DOCS and `leaf` as NOTE.TXT; "/DOCS/" and
   "/DOCS" leave `dir` at DOCS with no leaf at all, which is how a caller
   learns it was handed a directory rather than a file. */
static int resolve(const char *path, uint16 *dir, char *leaf, int cap,
                   int *has_leaf)
{
    if (!fs.ok)
        return -1;
    *dir = 0;
    *has_leaf = 0;

    const char *p = path;
    while (*p == '/')
        p++;

    while (*p) {
        char comp[FAT_NAME_MAX];
        int n = 0;
        while (*p && *p != '/' && n < FAT_NAME_MAX - 1)
            comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/')
            p++;

        if (!*p) {                      /* the last component */
            int k = 0;
            while (comp[k] && k < cap - 1) {
                leaf[k] = comp[k];
                k++;
            }
            leaf[k] = 0;
            *has_leaf = 1;
            return 0;
        }

        uint8 ent[32];
        if (dir_lookup(*dir, comp, ent, 0) < 0)
            return -1;
        if (!(ent[11] & 0x10))
            return -1;                  /* a file cannot be walked through */
        *dir = rd16(ent + 26);
    }
    return 0;                           /* the path named a directory */
}

/* If the leaf names a directory, hand back its cluster: that is what makes
   "/DOCS" and "/DOCS/" the same thing. */
static int as_dir(const char *path, uint16 *dir)
{
    char leaf[FAT_NAME_MAX];
    int  has_leaf;
    if (resolve(path, dir, leaf, (int)sizeof(leaf), &has_leaf) < 0)
        return -1;
    if (!has_leaf)
        return 0;
    uint8 ent[32];
    if (dir_lookup(*dir, leaf, ent, 0) < 0)
        return -1;
    if (!(ent[11] & 0x10))
        return -1;                      /* it is a file */
    *dir = rd16(ent + 26);
    return 0;
}

int fat16_list(const char *path, struct dirent *out, int max)
{
    uint16 dir;
    if (as_dir(path, &dir) < 0)
        return -1;

    struct dscan s;
    dscan_start(&s, dir);
    int n = 0;
    while (n < max) {
        uint8 ent[32];
        if (dscan_next(&s, out[n].name, FAT_NAME_MAX, ent, 0) < 0)
            break;
        out[n].size   = rd32(ent + 28);
        out[n].is_dir = (ent[11] & 0x10) != 0;
        n++;
    }
    return n;
}

/* How big it is, without reading it. The buffer used to be a fixed array and
   the answer did not matter; it is allocated now, and the allocator has to be
   told a number before the bytes exist. */
int fat16_size(const char *path)
{
    uint16 dir;
    char   leaf[FAT_NAME_MAX];
    int    has_leaf;
    if (resolve(path, &dir, leaf, (int)sizeof(leaf), &has_leaf) < 0 || !has_leaf)
        return -1;
    uint8 d[32];
    if (dir_lookup(dir, leaf, d, 0) < 0)
        return -1;
    if (d[11] & 0x10)
        return -1;                      /* a directory has no size of its own */
    return (int)rd32(d + 28);
}

int fat16_read(const char *path, void *out, int maxlen)
{
    uint16 dir;
    char   leaf[FAT_NAME_MAX];
    int    has_leaf;
    if (resolve(path, &dir, leaf, (int)sizeof(leaf), &has_leaf) < 0 || !has_leaf)
        return -1;

    uint8 d[32];
    if (dir_lookup(dir, leaf, d, 0) < 0)
        return -1;
    if (d[11] & 0x10)
        return -1;                      /* a directory is not read this way */

    uint32 size = rd32(d + 28);
    uint16 clus = rd16(d + 26);
    /* A file that does not fit is an error, not a short answer. */
    if (size > (uint32)maxlen)
        return -1;

    int cap = (int)size, got = 0;
    while (clus >= 2 && clus < 0xFFF8 && got < cap) {
        uint32 base = fs.data_start + (uint32)(clus - 2) * fs.sec_per_clus;
        for (uint32 sc = 0; sc < fs.sec_per_clus && got < cap; sc++) {
            uint8 cb[SECSZ];
            if (blk_read(base + sc, cb) < 0)
                return got;
            int chunk = SECSZ;
            if (chunk > cap - got)
                chunk = cap - got;
            umemcpy((uint8 *)out + got, cb, chunk);
            got += chunk;
        }
        clus = fat_next(clus);
    }
    return got;
}

/* ---- writing ------------------------------------------------------------
   What makes writing a filesystem different from reading one: a read that
   goes wrong returns the wrong bytes, and a write that goes wrong leaves a
   volume that no longer describes itself. Three structures have to agree —
   the directory entry, the allocation chain, and the data — and the FAT is
   duplicated on the volume precisely because it is the one nobody can
   reconstruct. So every change to it is made to both copies.

   The model is whole-file, matching the read side: a file is replaced, not
   edited in place. That is a real limit and it buys the absence of an entire
   class of bug, since there is no partial state to be interrupted in. */

#define FAT_FREE  0x0000
#define FAT_EOC   0xFFFF

static void fat_set(uint16 clus, uint16 val)
{
    uint32 off = (uint32)clus * 2;
    for (uint32 f = 0; f < fs.num_fats; f++) {
        uint32 sec = fs.fat_start + f * fs.sec_per_fat + off / SECSZ;
        uint8  buf[SECSZ];
        if (blk_read(sec, buf) < 0)
            return;
        wr16(buf + (off % SECSZ), val);
        blk_write(sec, buf);
    }
}

static uint16 fat_alloc_from(uint16 from)
{
    uint32 n = fs.sec_per_fat * SECSZ / 2;
    for (uint32 c = from < 2 ? 2 : from; c < n; c++) {
        uint32 off = c * 2;
        uint8  buf[SECSZ];
        if (blk_read(fs.fat_start + off / SECSZ, buf) < 0)
            return 0;
        if (rd16(buf + (off % SECSZ)) == FAT_FREE)
            return (uint16)c;
    }
    return 0;
}

static void fat_free_chain(uint16 clus)
{
    while (clus >= 2 && clus < 0xFFF8) {
        uint16 next = fat_next(clus);
        fat_set(clus, FAT_FREE);
        clus = next;
    }
}

/* There is no clock this filesystem trusts, so every file gets the same date
   — but it has to be a *valid* one. Zeroes decode as day zero of month zero,
   and a tool reading the volume shows 1980-00-00, which is not "no date" but
   a wrong one. 0x0021 is 1980-01-01, the epoch of the format itself. */
#define FAT_EPOCH 0x0021

static void set_entry(uint8 *d, const char *name11, uint8 attr,
                      uint16 first, uint32 size)
{
    umemcpy(d, name11, 11);
    d[11] = attr;
    for (int i = 12; i < 26; i++)
        d[i] = 0;
    wr16(d + 16, FAT_EPOCH);
    wr16(d + 18, FAT_EPOCH);
    wr16(d + 24, FAT_EPOCH);
    wr16(d + 26, first);
    wr32(d + 28, size);
}

/* Put a new name in a directory: the long entries, in reverse, then the short
   one they belong to. A name the old alphabet can hold exactly needs no long
   entries at all, so a volume of ordinary names looks as it always did. */
static int dir_create(uint16 dir, const char *name, uint8 attr,
                      uint16 first, uint32 size)
{
    char   n11[11];
    uint16 units[LFN_MAXUNITS];
    int    nu = 0, nlfn = 0;

    if (!fits_83(name, n11)) {
        nu = to_utf16(name, units, LFN_MAXUNITS);
        if (nu <= 0)
            return -1;                  /* not UTF-8, or too long to hold */
        short_name_for(dir, name, n11);
        nlfn = (nu + LFN_CHARS - 1) / LFN_CHARS;
    }

    uint32 start;
    if (dir_run(dir, nlfn + 1, &start) < 0)
        return -1;                      /* no room, and it cannot grow */

    uint8 sum = sfn_sum((const uint8 *)n11);
    for (int k = 0; k < nlfn; k++) {
        int seq = nlfn - k;             /* on disk they run backwards */
        uint8 e[32];
        for (int i = 0; i < 32; i++)
            e[i] = 0;
        e[0]  = (uint8)(seq | (k == 0 ? LFN_LAST : 0));
        e[11] = LFN_ATTR;
        e[13] = sum;
        uint16 chunk[LFN_CHARS];
        for (int i = 0; i < LFN_CHARS; i++) {
            int u = (seq - 1) * LFN_CHARS + i;
            /* The terminator, then 0xFFFF to the end of the entry. */
            chunk[i] = (u < nu) ? units[u] : (u == nu ? 0x0000 : 0xFFFF);
        }
        lfn_put(e, chunk);
        if (slot_put(dir, start + (uint32)k, e) < 0)
            return -1;
    }

    uint8 sh[32];
    for (int i = 0; i < 32; i++)
        sh[i] = 0;
    set_entry(sh, n11, attr, first, size);
    if (slot_put(dir, start + (uint32)nlfn, sh) < 0)
        return -1;
    return (int)(start + (uint32)nlfn);
}

int fat16_write(const char *path, const void *buf, int len)
{
    uint16 dir;
    char   leaf[FAT_NAME_MAX];
    int    has_leaf;
    if (len < 0 ||
        resolve(path, &dir, leaf, (int)sizeof(leaf), &has_leaf) < 0 || !has_leaf)
        return -1;

    uint8 old_ent[32];
    int   slot = dir_lookup(dir, leaf, old_ent, 0);
    if (slot >= 0 && (old_ent[11] & 0x10))
        return -1;                      /* refuse to overwrite a directory */

    uint32 clus_size = (uint32)fs.sec_per_clus * SECSZ;
    uint32 need = ((uint32)len + clus_size - 1) / clus_size;

    uint16 first = 0, prev = 0, search = 2;
    const uint8 *p = buf;
    uint32 done_bytes = 0;

    for (uint32 i = 0; i < need; i++) {
        uint16 c = fat_alloc_from(search);
        if (c == 0) {
            fat_free_chain(first);      /* out of room: leave nothing behind */
            return -1;
        }
        /* Claim it before looking for the next, or the next search finds the
           same free entry again. */
        fat_set(c, FAT_EOC);
        search = c + 1;
        if (prev)
            fat_set(prev, c);
        else
            first = c;
        prev = c;

        uint32 base = fs.data_start + (uint32)(c - 2) * fs.sec_per_clus;
        for (uint32 sc = 0; sc < fs.sec_per_clus; sc++) {
            uint8 sector[SECSZ];
            for (int k = 0; k < SECSZ; k++)
                sector[k] = 0;          /* the tail of the last cluster */
            uint32 chunk = (uint32)len - done_bytes;
            if (chunk > SECSZ)
                chunk = SECSZ;
            if (chunk > 0) {
                umemcpy(sector, p + done_bytes, chunk);
                done_bytes += chunk;
            }
            if (blk_write(base + sc, sector) < 0) {
                fat_free_chain(first);
                return -1;
            }
        }
    }

    /* Point the name at the new chain, and free the old one only after that
       has landed: interrupted anywhere, the volume has a stale entry rather
       than one aimed at clusters already handed to something else. */
    if (slot >= 0) {
        uint16 old = rd16(old_ent + 26);
        uint8  e[32];
        umemcpy(e, old_ent, 32);
        wr16(e + 26, first);
        wr32(e + 28, (uint32)len);
        if (slot_put(dir, (uint32)slot, e) < 0) {
            fat_free_chain(first);
            return -1;
        }
        if (old >= 2)
            fat_free_chain(old);
    } else if (dir_create(dir, leaf, 0x20, first, (uint32)len) < 0) {
        fat_free_chain(first);
        return -1;
    }
    return len;
}

/* Free a name: the short entry and the long entries in front of it. Leaving
   the long ones behind would be worse than untidy — a later file could take
   the short slot and inherit somebody else's name. */
static int dir_erase(uint16 dir, uint32 chain, uint32 slot)
{
    for (uint32 i = chain; i <= slot; i++) {
        uint8 *d;
        if (slot_ptr(dir, i, &d) < 0)
            return -1;
        uint8 e[32];
        umemcpy(e, d, 32);
        e[0] = 0xE5;                    /* the slot is free; the name is not
                                           erased, which is why deleted files
                                           can be recovered on these volumes */
        if (slot_put(dir, i, e) < 0)
            return -1;
    }
    return 0;
}

int fat16_remove(const char *path)
{
    uint16 dir;
    char   leaf[FAT_NAME_MAX];
    int    has_leaf;
    if (resolve(path, &dir, leaf, (int)sizeof(leaf), &has_leaf) < 0 || !has_leaf)
        return -1;

    uint8  ent[32];
    uint32 chain;
    int slot = dir_lookup(dir, leaf, ent, &chain);
    if (slot < 0)
        return -1;
    uint16 first = rd16(ent + 26);

    if (ent[11] & 0x10) {
        /* A directory goes only if it is empty, and empty means nothing but
           the two entries every directory carries about itself. */
        struct dscan s;
        char got[FAT_NAME_MAX];
        dscan_start(&s, first);
        for (;;) {
            uint8 e[32];
            if (dscan_next(&s, got, (int)sizeof(got), e, 0) < 0)
                break;
            if (got[0] == '.' && (got[1] == 0 || (got[1] == '.' && got[2] == 0)))
                continue;
            return -1;                  /* not empty */
        }
    }

    if (dir_erase(dir, chain, (uint32)slot) < 0)
        return -1;
    fat_free_chain(first);
    return 0;
}

/* A directory is a file whose contents are entries, and which begins by
   describing itself and its parent. The parent of a directory in the root is
   written as cluster 0 — the root has no entry anywhere to point at. */
int fat16_mkdir(const char *path)
{
    uint16 dir;
    char   leaf[FAT_NAME_MAX];
    int    has_leaf;
    if (resolve(path, &dir, leaf, (int)sizeof(leaf), &has_leaf) < 0 || !has_leaf)
        return -1;
    if (dir_lookup(dir, leaf, 0, 0) >= 0)
        return -1;                      /* taken */

    uint16 c = fat_alloc_from(2);
    if (c == 0)
        return -1;
    fat_set(c, FAT_EOC);

    uint32 base = fs.data_start + (uint32)(c - 2) * fs.sec_per_clus;
    for (uint32 sc = 0; sc < fs.sec_per_clus; sc++) {
        uint8 b[SECSZ];
        for (int k = 0; k < SECSZ; k++)
            b[k] = 0;
        if (blk_write(base + sc, b) < 0) {
            fat_set(c, FAT_FREE);
            return -1;
        }
    }
    dcache_ok = 0;                      /* the sector under it just changed */

    uint8 e[32];
    for (int i = 0; i < 32; i++)
        e[i] = 0;
    set_entry(e, ".          ", 0x10, c, 0);
    if (slot_put(c, 0, e) < 0) { fat_set(c, FAT_FREE); return -1; }
    for (int i = 0; i < 32; i++)
        e[i] = 0;
    set_entry(e, "..         ", 0x10, dir, 0);
    if (slot_put(c, 1, e) < 0) { fat_set(c, FAT_FREE); return -1; }

    if (dir_create(dir, leaf, 0x10, c, 0) < 0) {
        fat_set(c, FAT_FREE);
        return -1;
    }
    return 0;
}
