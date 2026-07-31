/* fdt.c — reading the machine's own description of itself.

   Every address in this kernel has been a constant: the UART at 0x10000000,
   the PLIC at 0x0c000000, eight virtio slots a page apart from 0x10001000.
   Each one is right, and each one is right by having been looked up in
   QEMU's source rather than asked for. The board has been telling us all
   along — the flattened device tree is in a1 when the first instruction runs,
   and boot.S threw it away.

   The format is not complicated and is worth stating in full, because it is
   the one place in this system where a data structure arrives from outside:

     header    magic d00dfeed, then the offsets of the two blocks below
     strings   property names, concatenated, NUL-terminated
     struct    a stream of big-endian 32-bit tokens:
                 1  BEGIN_NODE  followed by the name, NUL-padded to 4
                 2  END_NODE
                 3  PROP        length, name-offset into strings, then bytes
                 4  NOP
                 9  END

   Everything is big-endian, on a little-endian machine, which is the price of
   a format designed for Open Firmware on PowerPC and kept because it works.
   A `reg` property is a list of address/size pairs whose *cell counts* come
   from #address-cells and #size-cells on the parent — on this board both are
   2, so every number is 64 bits, and this reader assumes that rather than
   tracking the parent's declaration. It is written down here because it is a
   real limitation and not a hidden one. */
#include "fdt.h"
#include "util.h"
#include "uart.h"

unsigned long dtb_pa;

#define FDT_MAGIC     0xd00dfeed
#define FDT_BEGIN     1
#define FDT_END_NODE  2
#define FDT_PROP      3
#define FDT_NOP       4
#define FDT_END       9

struct fdt_header {
    uint32 magic, totalsize, off_struct, off_strings, off_rsvmap;
    uint32 version, last_comp_version, boot_cpuid;
    uint32 size_strings, size_struct;
};

static const struct fdt_header *hdr;
static const uint8 *fstruct, *fstrings;

/* Its own, because util.h has no string compare and one line is cheaper than
   a new entry in a shared header. */
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static uint32 be32(const void *p)
{
    const uint8 *b = p;
    return ((uint32)b[0] << 24) | ((uint32)b[1] << 16) |
           ((uint32)b[2] << 8) | b[3];
}

static uint64 be64(const void *p)
{
    return ((uint64)be32(p) << 32) | be32((const uint8 *)p + 4);
}

int fdt_init(void)
{
    if (!dtb_pa)
        return -1;
    const struct fdt_header *h = (const struct fdt_header *)dtb_pa;
    if (be32(&h->magic) != FDT_MAGIC)
        return -1;
    hdr      = h;
    fstruct  = (const uint8 *)dtb_pa + be32(&h->off_struct);
    fstrings = (const uint8 *)dtb_pa + be32(&h->off_strings);
    return 0;
}

/* The walk. Every question below is this loop with a different bookkeeping,
   which is why it is a callback rather than four copies: `node` is the name
   of the node a property belongs to, and a property with a null name means
   "a node began". */
typedef int (*fdt_fn)(const char *node, const char *prop,
                      const uint8 *val, int len, int depth, void *arg);

static void fdt_walk(fdt_fn fn, void *arg)
{
    if (!fstruct)
        return;
    const uint8 *p = fstruct;
    const uint8 *end = fstruct + be32(&hdr->size_struct);
    const char *node = "";
    char names[8][40];
    int depth = 0;

    while (p < end) {
        uint32 tok = be32(p);
        p += 4;
        if (tok == FDT_BEGIN) {
            const char *nm = (const char *)p;
            int l = 0;
            while (nm[l]) l++;
            if (depth < 8) {
                int k = 0;
                while (k < 39 && nm[k]) { names[depth][k] = nm[k]; k++; }
                names[depth][k] = 0;
                node = names[depth];
            }
            depth++;
            if (fn(node, 0, 0, 0, depth, arg))
                return;
            p += (l + 4) & ~3u;
        } else if (tok == FDT_END_NODE) {
            depth--;
            node = depth > 0 && depth <= 8 ? names[depth - 1] : "";
        } else if (tok == FDT_PROP) {
            uint32 len  = be32(p);
            uint32 noff = be32(p + 4);
            p += 8;
            if (fn(node, (const char *)fstrings + noff, p, (int)len, depth, arg))
                return;
            p += (len + 3) & ~3u;
        } else if (tok == FDT_NOP) {
            continue;
        } else {
            break;                      /* FDT_END, or something unexpected */
        }
    }
}

/* ---- the questions ------------------------------------------------------ */

static int name_has_prefix(const char *name, const char *prefix)
{
    int i = 0;
    while (prefix[i]) {
        if (name[i] != prefix[i])
            return 0;
        i++;
    }
    return 1;
}

struct look {
    const char *prefix;
    int    want, seen;
    uint64 base, size;
    int    irq, found;
};

static int look_fn(const char *node, const char *prop, const uint8 *val,
                   int len, int depth, void *arg)
{
    (void)depth;
    struct look *L = arg;
    if (!name_has_prefix(node, L->prefix))
        return 0;
    if (!prop) {                        /* a node began */
        if (L->found)
            return 1;                   /* the one we wanted is complete */
        L->seen++;
        return 0;
    }
    if (L->seen - 1 != L->want)
        return 0;
    if (streq(prop, "reg") && len >= 16) {
        L->base  = be64(val);
        L->size  = be64(val + 8);
        L->found = 1;
    } else if (streq(prop, "interrupts") && len >= 4) {
        L->irq = (int)be32(val);
    }
    return 0;
}

int fdt_reg_n(const char *prefix, int n, uint64 *base, uint64 *size, int *irq)
{
    struct look L = { prefix, n, 0, 0, 0, 0, 0 };
    fdt_walk(look_fn, &L);
    if (!L.found)
        return -1;
    if (base) *base = L.base;
    if (size) *size = L.size;
    if (irq)  *irq  = L.irq;
    return 0;
}

int fdt_reg(const char *prefix, uint64 *base, uint64 *size)
{
    return fdt_reg_n(prefix, 0, base, size, 0);
}

struct count { const char *prefix; int n; };

static int count_fn(const char *node, const char *prop, const uint8 *val,
                    int len, int depth, void *arg)
{
    (void)val; (void)len; (void)depth;
    struct count *C = arg;
    if (!prop && name_has_prefix(node, C->prefix))
        C->n++;
    return 0;
}

int fdt_count(const char *prefix)
{
    struct count C = { prefix, 0 };
    fdt_walk(count_fn, &C);
    return C.n;
}

/* ---- the tree as text --------------------------------------------------- */

struct render { char *out; int o, cap; };

static void put(struct render *R, const char *s)
{
    while (*s && R->o < R->cap - 1)
        R->out[R->o++] = *s++;
}

static void puthex(struct render *R, uint64 v)
{
    char t[17];
    int k = 0;
    if (!v) { put(R, "0"); return; }
    while (v) {
        int d = (int)(v & 15);
        t[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    }
    put(R, "0x");
    while (k && R->o < R->cap - 1)
        R->out[R->o++] = t[--k];
}

static int render_fn(const char *node, const char *prop, const uint8 *val,
                     int len, int depth, void *arg)
{
    struct render *R = arg;
    if (R->o > R->cap - 96)
        return 1;
    if (!prop) {
        for (int i = 1; i < depth && i < 8; i++)
            put(R, "  ");
        put(R, node[0] ? node : "/");
        put(R, "\n");
        return 0;
    }
    /* Only the properties that say where something is and how to hear from
       it. A device tree carries a great deal else — clock phandles, compatible
       strings for drivers that do not exist here — and printing all of it
       would bury the three facts this kernel acts on. */
    int is_reg  = streq(prop, "reg");
    int is_irq  = streq(prop, "interrupts");
    int is_comp = streq(prop, "compatible");
    if (!is_reg && !is_irq && !is_comp)
        return 0;

    for (int i = 0; i < depth && i < 8; i++)
        put(R, "  ");
    put(R, prop);
    put(R, " ");
    if (is_comp) {
        for (int i = 0; i < len && R->o < R->cap - 2; i++)
            R->out[R->o++] = val[i] ? (char)val[i] : ' ';
    } else if (is_reg && len >= 16) {
        puthex(R, be64(val));
        put(R, " + ");
        puthex(R, be64(val + 8));
    } else if (is_irq && len >= 4) {
        char n[16];
        int k = utoa(be32(val), n);
        n[k] = 0;
        put(R, n);
    } else {
        put(R, "(");
        char n[16];
        int k = utoa((unsigned long)len, n);
        n[k] = 0;
        put(R, n);
        put(R, " bytes)");
    }
    put(R, "\n");
    return 0;
}

int fdt_render(char *out, int cap)
{
    struct render R = { out, 0, cap };
    if (!fstruct) {
        put(&R, "no device tree: a1 was zero at boot\n");
        return R.o;
    }
    fdt_walk(render_fn, &R);
    return R.o;
}
