/* fat16.c — a small FAT16, read and write, with directories. Block device is a flat region of
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

static int usable_entry(const uint8 *d)
{
    if (d[0] == 0x00 || d[0] == 0xE5) return 0;   /* free / deleted */
    if (d[11] == 0x0F)                return 0;   /* long-name entry */
    if (d[11] & 0x08)                 return 0;   /* volume label */
    return 1;
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

/* Find an entry by its on-disk 11-byte name. Also reports the first slot that
   could hold a new one, so a caller that means to create something does not
   have to walk the directory twice. */
static int dir_find(uint16 dir, const char *want11, uint32 *sec_out,
                    int *off_out, int *existing)
{
    uint32 free_sec = 0;
    int    free_off = -1;
    uint32 lba;

    for (uint32 n = 0; dir_sector(dir, n, &lba) == 0; n++) {
        uint8 buf[SECSZ];
        if (blk_read(lba, buf) < 0)
            return -1;
        for (int e = 0; e < SECSZ; e += 32) {
            uint8 *d = buf + e;
            if (d[0] == 0x00 || d[0] == 0xE5) {
                if (free_off < 0) {
                    free_sec = lba;
                    free_off = e;
                }
                if (d[0] == 0x00)
                    goto done;          /* nothing beyond here is in use */
                continue;
            }
            if (d[11] == 0x0F || (d[11] & 0x08))
                continue;               /* long-name fragment, or the label */
            if (umemcmp(d, want11, 11) == 0) {
                if (sec_out) *sec_out = lba;
                if (off_out) *off_out = e;
                if (existing) *existing = 1;
                return 0;
            }
        }
    }
done:
    if (!existing)
        return -1;                      /* the caller only wanted a match */
    if (free_off < 0)
        return -1;                      /* full, and no room to grow it */
    if (sec_out) *sec_out = free_sec;
    if (off_out) *off_out = free_off;
    *existing = 0;
    return 0;
}

/* Walk a path down to the directory that should contain its last component.
   "/DOCS/NOTE.TXT" leaves `dir` at DOCS and `leaf` as NOTE.TXT; "/DOCS/" and
   "/DOCS" leave `dir` at DOCS with no leaf at all, which is how a caller
   learns it was handed a directory rather than a file. */
static int resolve(const char *path, uint16 *dir, char leaf[11], int *has_leaf)
{
    if (!fs.ok)
        return -1;
    *dir = 0;
    *has_leaf = 0;

    const char *p = path;
    while (*p == '/')
        p++;

    while (*p) {
        char comp[64];
        int n = 0;
        while (*p && *p != '/' && n < 63)
            comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/')
            p++;

        if (!*p) {                      /* the last component */
            to_83(comp, leaf);
            *has_leaf = 1;
            return 0;
        }

        /* An interior component has to be a directory that exists. */
        char want[11];
        to_83(comp, want);
        uint32 sec;
        int off;
        if (dir_find(*dir, want, &sec, &off, 0) < 0)
            return -1;
        uint8 buf[SECSZ];
        if (blk_read(sec, buf) < 0)
            return -1;
        if (!(buf[off + 11] & 0x10))
            return -1;                  /* a file cannot be walked through */
        *dir = rd16(buf + off + 26);
    }
    return 0;                           /* the path named a directory */
}

/* If the leaf names a directory, hand back its cluster: that is what makes
   "/DOCS" and "/DOCS/" the same thing. */
static int as_dir(const char *path, uint16 *dir)
{
    char leaf[11];
    int  has_leaf;
    if (resolve(path, dir, leaf, &has_leaf) < 0)
        return -1;
    if (!has_leaf)
        return 0;                       /* already a directory */
    uint32 sec;
    int off;
    if (dir_find(*dir, leaf, &sec, &off, 0) < 0)
        return -1;
    uint8 buf[SECSZ];
    if (blk_read(sec, buf) < 0)
        return -1;
    if (!(buf[off + 11] & 0x10))
        return -1;                      /* it is a file */
    *dir = rd16(buf + off + 26);
    return 0;
}

int fat16_list(const char *path, struct dirent *out, int max)
{
    uint16 dir;
    if (as_dir(path, &dir) < 0)
        return -1;

    int n = 0;
    uint32 lba;
    for (uint32 i = 0; dir_sector(dir, i, &lba) == 0 && n < max; i++) {
        uint8 buf[SECSZ];
        if (blk_read(lba, buf) < 0)
            return n;
        for (int e = 0; e < SECSZ && n < max; e += 32) {
            uint8 *d = buf + e;
            if (d[0] == 0x00)
                return n;                          /* end of directory */
            if (!usable_entry(d))
                continue;
            fmt_name(d, out[n].name);
            out[n].size   = rd32(d + 28);
            out[n].is_dir = (d[11] & 0x10) != 0;
            n++;
        }
    }
    return n;
}

int fat16_read(const char *path, void *out, int maxlen)
{
    uint16 dir;
    char   leaf[11];
    int    has_leaf;
    if (resolve(path, &dir, leaf, &has_leaf) < 0 || !has_leaf)
        return -1;

    uint32 sec;
    int    off;
    if (dir_find(dir, leaf, &sec, &off, 0) < 0)
        return -1;
    uint8 dbuf[SECSZ];
    if (blk_read(sec, dbuf) < 0)
        return -1;
    uint8 *d = dbuf + off;
    if (d[11] & 0x10)
        return -1;                      /* a directory is not read this way */

    uint32 size = rd32(d + 28);
    uint16 clus = rd16(d + 26);
    /* A file that does not fit is an error, not a short answer. This used to
       hand back whatever fitted and say nothing, so a caller got a truncated
       file it had no way to recognise as truncated. */
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
    wr16(d + 16, FAT_EPOCH);            /* created */
    wr16(d + 18, FAT_EPOCH);            /* last accessed */
    wr16(d + 24, FAT_EPOCH);            /* last written */
    wr16(d + 26, first);
    wr32(d + 28, size);
}

int fat16_write(const char *path, const void *buf, int len)
{
    uint16 dir;
    char   leaf[11];
    int    has_leaf;
    if (len < 0 || resolve(path, &dir, leaf, &has_leaf) < 0 || !has_leaf)
        return -1;

    uint32 sec;
    int    off, existing;
    if (dir_find(dir, leaf, &sec, &off, &existing) < 0)
        return -1;

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

    uint8 dbuf[SECSZ];
    if (blk_read(sec, dbuf) < 0) {
        fat_free_chain(first);
        return -1;
    }
    uint8 *d = dbuf + off;
    if (existing && (d[11] & 0x10)) {   /* refuse to overwrite a directory */
        fat_free_chain(first);
        return -1;
    }
    uint16 old = existing ? rd16(d + 26) : 0;

    /* Point the entry at the new chain first, and free the old one only after
       that has landed: interrupted anywhere, the volume has a stale entry
       rather than one aimed at clusters already handed to something else. */
    set_entry(d, leaf, 0x20, first, (uint32)len);
    if (blk_write(sec, dbuf) < 0) {
        fat_free_chain(first);
        return -1;
    }
    if (existing && old >= 2)
        fat_free_chain(old);
    return len;
}

int fat16_remove(const char *path)
{
    uint16 dir;
    char   leaf[11];
    int    has_leaf;
    if (resolve(path, &dir, leaf, &has_leaf) < 0 || !has_leaf)
        return -1;

    uint32 sec;
    int    off, existing = 0;
    if (dir_find(dir, leaf, &sec, &off, &existing) < 0 || !existing)
        return -1;

    uint8 dbuf[SECSZ];
    if (blk_read(sec, dbuf) < 0)
        return -1;
    uint8 *d = dbuf + off;
    uint16 first = rd16(d + 26);

    if (d[11] & 0x10) {
        /* A directory only goes if it is empty, and "empty" means nothing but
           the two entries every directory carries about itself. */
        struct dirent e[4];
        int n = 0;
        uint32 lba;
        for (uint32 i = 0; dir_sector(first, i, &lba) == 0 && n < 4; i++) {
            uint8 b[SECSZ];
            if (blk_read(lba, b) < 0)
                break;
            for (int k = 0; k < SECSZ && n < 4; k += 32) {
                if (b[k] == 0x00)
                    goto counted;
                if (!usable_entry(b + k))
                    continue;
                fmt_name(b + k, e[n].name);
                if (e[n].name[0] == '.' &&
                    (e[n].name[1] == 0 || (e[n].name[1] == '.' && e[n].name[2] == 0)))
                    continue;           /* itself and its parent */
                n++;
            }
        }
counted:
        if (n > 0)
            return -1;                  /* not empty */
    }

    d[0] = 0xE5;                        /* the slot is free; the name is not
                                           erased, which is why deleted files
                                           can be recovered on these volumes */
    if (blk_write(sec, dbuf) < 0)
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
    char   leaf[11];
    int    has_leaf;
    if (resolve(path, &dir, leaf, &has_leaf) < 0 || !has_leaf)
        return -1;

    uint32 sec;
    int    off, existing;
    if (dir_find(dir, leaf, &sec, &off, &existing) < 0 || existing)
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
        if (sc == 0) {
            set_entry(b,      ".          ", 0x10, c,   0);
            set_entry(b + 32, "..         ", 0x10, dir, 0);
        }
        if (blk_write(base + sc, b) < 0) {
            fat_set(c, FAT_FREE);
            return -1;
        }
    }

    uint8 dbuf[SECSZ];
    if (blk_read(sec, dbuf) < 0) {
        fat_set(c, FAT_FREE);
        return -1;
    }
    set_entry(dbuf + off, leaf, 0x10, c, 0);
    if (blk_write(sec, dbuf) < 0) {
        fat_set(c, FAT_FREE);
        return -1;
    }
    return 0;
}
