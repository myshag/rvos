/* fat16.c — minimal read-only FAT16. Parses the BPB, walks the root directory
   and follows cluster chains through the FAT. Block device is a flat region of
   guest RAM at DISK_BASE (the image QEMU loaded with -device loader). */
#include "fat16.h"
#include "ulib.h"

#define DISK_BASE 0x84000000UL
#define SECSZ     512

static void blk_read(uint32 lba, void *buf)
{
    umemcpy(buf, (const void *)(DISK_BASE + (uint64)lba * SECSZ), SECSZ);
}

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

int fat16_list_root(struct dirent *out, int max)
{
    if (!fs.ok) return -1;
    int n = 0;
    for (uint32 s = 0; s < fs.root_sectors && n < max; s++) {
        uint8 buf[SECSZ];
        blk_read(fs.root_start + s, buf);
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

int fat16_read(const char *name, void *out, int maxlen)
{
    if (!fs.ok) return -1;
    char want[11];
    to_83(name, want);

    for (uint32 s = 0; s < fs.root_sectors; s++) {
        uint8 buf[SECSZ];
        blk_read(fs.root_start + s, buf);
        for (int e = 0; e < SECSZ; e += 32) {
            uint8 *d = buf + e;
            if (d[0] == 0x00)
                return -1;
            if (!usable_entry(d) || (d[11] & 0x10))
                continue;                          /* skip dirs */
            if (umemcmp(d, want, 11) != 0)
                continue;

            uint32 size = rd32(d + 28);
            uint16 clus = rd16(d + 26);
            /* A file that does not fit is an error, not a short answer. This
               used to hand back whatever fitted and say nothing, so a caller
               got a truncated file it had no way to recognise as truncated —
               which is exactly what happened to a program one byte over the
               limit: it loaded, and only a bounds check in the ELF loader
               stopped it from running as garbage. */
            if (size > (uint32)maxlen)
                return -1;
            int cap = (int)size;
            int got = 0;
            while (clus >= 2 && clus < 0xFFF8 && got < cap) {
                uint32 base = fs.data_start + (uint32)(clus - 2) * fs.sec_per_clus;
                for (int sc = 0; sc < fs.sec_per_clus && got < cap; sc++) {
                    uint8 cb[SECSZ];
                    blk_read(base + sc, cb);
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
    }
    return -1;
}
