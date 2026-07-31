/* pci.c — asking the bus what is plugged into it.

   The device tree says where the configuration space is; this reads it. On
   this board that space is ECAM — "enhanced configuration access mechanism",
   which is a grand name for the simplest possible arrangement: every
   function's 4 KiB of configuration registers is *memory*, at

       ecam + (bus << 20) + (device << 15) + (function << 12)

   so enumerating the bus is a loop over addresses and a test of one field.
   The older mechanism, still what x86 firmware uses at boot, is a pair of I/O
   ports — write an address to 0xCF8, read the data from 0xCFC — and needs a
   lock, because the pair is global state. ECAM needs nothing.

   A function that is not there answers 0xffff for its vendor. That is not a
   convention agreed between the bus and the reader: it is what a read with
   nobody driving the lines returns, and it became the rule because it was
   already the behaviour.

   Nothing here drives anything. This finds what is present and says so, which
   is the whole of what a `lspci` does and, on a machine whose devices are all
   on the mmio bus, the whole of what is wanted. */
#include "pci.h"
#include "fdt.h"
#include "util.h"

static uint64 ecam;
static int    nfound;

struct pci_fn {
    uint8  bus, dev, fn;
    uint16 vendor, device;
    uint8  class, subclass, progif, header;
    uint8  irq;
    uint64 bar[6];
};
static struct pci_fn found[PCI_MAX];

static uint32 cfg32(uint64 base, int off)
{
    return *(volatile uint32 *)(base + (uint64)off);
}

/* What the class code means, for the handful this machine can produce. The
   full list is a hundred entries and a kernel that drives none of them has no
   use for the other ninety. */
static const char *class_name(uint8 c, uint8 s, uint8 p)
{
    switch (c) {
    case 0x00: return "unclassified";
    case 0x01: return s == 0x08 ? "nvme" : "storage";
    case 0x02: return "network";
    case 0x03: return "display";
    case 0x04: return "multimedia";
    case 0x06: return s == 0x00 ? "host bridge"
                    : s == 0x04 ? "pci bridge" : "bridge";
    case 0x0c:
        if (s != 0x03) return "serial bus";
        switch (p) {
        case 0x00: return "usb uhci";
        case 0x10: return "usb ohci";
        case 0x20: return "usb ehci";
        case 0x30: return "usb xhci";
        default:   return "usb";
        }
    default: return "device";
    }
}

int pci_init(void)
{
    uint64 size;
    if (fdt_reg("pci@", &ecam, &size) < 0)
        return -1;

    nfound = 0;
    for (int bus = 0; bus < 2; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                uint64 a = ecam + ((uint64)bus << 20) +
                           ((uint64)dev << 15) + ((uint64)fn << 12);
                uint32 id = cfg32(a, 0x00);
                if ((id & 0xffff) == 0xffff)
                    continue;           /* nobody driving the lines */
                if (nfound >= PCI_MAX)
                    return nfound;

                struct pci_fn *f = &found[nfound++];
                f->bus = (uint8)bus; f->dev = (uint8)dev; f->fn = (uint8)fn;
                f->vendor = (uint16)(id & 0xffff);
                f->device = (uint16)(id >> 16);

                uint32 cl = cfg32(a, 0x08);
                f->progif   = (uint8)(cl >> 8);
                f->subclass = (uint8)(cl >> 16);
                f->class    = (uint8)(cl >> 24);
                f->header   = (uint8)(cfg32(a, 0x0c) >> 16) & 0x7f;
                f->irq      = (uint8)(cfg32(a, 0x3c) & 0xff);

                /* The base addresses, as firmware left them. A 64-bit BAR is
                   two of these and the low bits say so; the low four bits are
                   flags rather than address in every case. */
                int nbar = f->header == 0 ? 6 : 2;
                for (int b = 0; b < 6; b++)
                    f->bar[b] = 0;
                for (int b = 0; b < nbar; b++) {
                    uint32 lo = cfg32(a, 0x10 + b * 4);
                    if (!lo)
                        continue;
                    if ((lo & 1) == 0 && ((lo >> 1) & 3) == 2 && b + 1 < nbar) {
                        f->bar[b] = ((uint64)cfg32(a, 0x14 + b * 4) << 32) |
                                    (lo & ~0xfu);
                        b++;
                    } else if (lo & 1) {
                        f->bar[b] = lo & ~3u;       /* an I/O port range */
                    } else {
                        f->bar[b] = lo & ~0xfu;
                    }
                }

                /* Only function 0 of a device that says it is multi-function
                   is worth looking past. */
                if (fn == 0 && !(((cfg32(a, 0x0c) >> 16) & 0x80)))
                    break;
            }
    return nfound;
}

int pci_count(void) { return nfound; }

uint64 pci_ecam(void) { return ecam; }

int pci_name(int i, char *out, int cap)
{
    if (i < 0 || i >= nfound || cap < 10)
        return -1;
    struct pci_fn *f = &found[i];
    int o = 0;
    out[o++] = "0123456789abcdef"[(f->bus >> 4) & 15];
    out[o++] = "0123456789abcdef"[f->bus & 15];
    out[o++] = ':';
    out[o++] = "0123456789abcdef"[(f->dev >> 4) & 15];
    out[o++] = "0123456789abcdef"[f->dev & 15];
    out[o++] = '.';
    out[o++] = "0123456789abcdef"[f->fn & 15];
    out[o] = 0;
    return o;
}

static int puts_at(char *out, int o, int cap, const char *s)
{
    while (*s && o < cap - 1)
        out[o++] = *s++;
    return o;
}

static int hex_at(char *out, int o, int cap, uint64 v, int digits)
{
    for (int i = digits - 1; i >= 0 && o < cap - 1; i--)
        out[o++] = "0123456789abcdef"[(v >> (i * 4)) & 15];
    return o;
}

/* One line per function: where it is, what it says it is, and what it is
   for. The shape a person reads, since nothing else reads this. */
int pci_render(int i, char *out, int cap)
{
    if (i < 0 || i >= nfound)
        return -1;
    struct pci_fn *f = &found[i];
    int o = 0;

    o = puts_at(out, o, cap, "vendor   ");
    o = hex_at(out, o, cap, f->vendor, 4);
    o = puts_at(out, o, cap, "\ndevice   ");
    o = hex_at(out, o, cap, f->device, 4);
    o = puts_at(out, o, cap, "\nclass    ");
    o = hex_at(out, o, cap, f->class, 2);
    o = hex_at(out, o, cap, f->subclass, 2);
    o = hex_at(out, o, cap, f->progif, 2);
    o = puts_at(out, o, cap, "  ");
    o = puts_at(out, o, cap, class_name(f->class, f->subclass, f->progif));
    o = puts_at(out, o, cap, "\nirq      ");
    {
        char n[8];
        int k = utoa(f->irq, n);
        n[k] = 0;
        o = puts_at(out, o, cap, n);
    }
    for (int b = 0; b < 6; b++) {
        if (!f->bar[b])
            continue;
        o = puts_at(out, o, cap, "\nbar");
        out[o++] = (char)('0' + b);
        o = puts_at(out, o, cap, "     0x");
        o = hex_at(out, o, cap, f->bar[b], 8);
    }
    o = puts_at(out, o, cap, "\ndriver   none\n");
    out[o] = 0;
    return o;
}

/* The summary line, for the boot log and for a listing. */
int pci_summary(int i, char *out, int cap)
{
    if (i < 0 || i >= nfound)
        return -1;
    struct pci_fn *f = &found[i];
    int o = pci_name(i, out, cap);
    o = puts_at(out, o, cap, "  ");
    o = hex_at(out, o, cap, f->vendor, 4);
    out[o++] = ':';
    o = hex_at(out, o, cap, f->device, 4);
    o = puts_at(out, o, cap, "  ");
    o = puts_at(out, o, cap, class_name(f->class, f->subclass, f->progif));
    out[o] = 0;
    return o;
}
