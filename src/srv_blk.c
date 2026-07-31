/* srv_blk.c — a virtio-blk driver, in the filesystem server's own task.

   The same shape as the network: srv_net.c drives the card and net_ip.c
   speaks the protocols, both inside one task; srv_blk.c drives the disk and
   fat16.c reads the filesystem off it, both inside another. Neither pair can
   reach into the other, and neither driver has the kernel on its data path —
   the device registers are mapped into that task and no other, so they are
   ordinary loads and stores.

   What replaces what: until now the "disk" was a FAT16 image copied into
   guest RAM by `-device loader`, and blk_read was a memcpy from a window the
   filesystem server happened to have mapped. It is a real device now, and the
   image is a file on the host that QEMU serves as a drive.

   One request at a time and *polled*, and that is a decision worth naming.
   The obvious alternative is to wait for the interrupt in sys_recv, as the
   network driver does — but the filesystem server is already inside a client
   request when it reads a sector, and a sys_recv there would just as likely
   hand it a second client's request as the interrupt it wanted. That is the
   multiplexing problem import ran into, and the filesystem has no reason to
   take it on: it yields between checks, so nothing else is starved, and the
   request completes in the time QEMU takes to memcpy. */
#include "syscall.h"
#include "ulib.h"
#include "virtio.h"
#include "blk.h"

#define VIRTIO_ID_BLOCK 2

/* A request is three descriptors chained: a header the device reads, a data
   buffer it writes (for a read) or reads (for a write), and one status byte
   it writes. The chain is the point — the device is told "these three belong
   together" by the NEXT flag, not by their being adjacent. */
struct blk_hdr {
    uint32 type;
    uint32 reserved;
    uint64 sector;
};
#define BLK_IN  0                       /* device -> memory */
#define BLK_OUT 1                       /* memory -> device */

static volatile uint32 *dev;
static uint32 rd(int off)           { return dev[off / 4]; }
static void   wr(int off, uint32 v) { dev[off / 4] = v; }
static void   fence(void) { __asm__ volatile("fence rw, rw" ::: "memory"); }

static struct virtq_desc  *desc;
static struct virtq_avail *avail;
static struct virtq_used  *used;
static uint16 last_used;

static struct blk_hdr *hdr;             /* one page, split three ways */
static uint8          *data;
static volatile uint8 *status;
static uint64 hdr_pa, data_pa, status_pa;

static int ready;

static int find_device(void)
{
    for (int i = 0; i < VIRTIO_MMIO_SLOTS; i++) {
        volatile uint32 *p =
            (volatile uint32 *)(VIRTIO_MMIO_BASE + (uint64)i * VIRTIO_MMIO_STRIDE);
        if (p[VIRTIO_MAGIC / 4] != VIRTIO_MAGIC_VALUE)
            continue;
        if (p[VIRTIO_DEVICE_ID / 4] != VIRTIO_ID_BLOCK)
            continue;
        dev = p;
        return i;
    }
    return -1;
}

/* Same handshake as the network card, and the same trap: FEATURES_OK has to
   stick or the device will not run. We want no features at all beyond the
   version, which makes the selection short. */
static int negotiate(void)
{
    wr(VIRTIO_STATUS, 0);
    wr(VIRTIO_STATUS, VIRTIO_S_ACKNOWLEDGE);
    wr(VIRTIO_STATUS, VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER);

    wr(VIRTIO_DEV_FEAT_SEL, 1);
    uint32 hi = rd(VIRTIO_DEV_FEATURES);
    wr(VIRTIO_DRV_FEAT_SEL, 0);
    wr(VIRTIO_DRV_FEATURES, 0);
    wr(VIRTIO_DRV_FEAT_SEL, 1);
    wr(VIRTIO_DRV_FEATURES, hi & (1u << (VIRTIO_F_VERSION_1 - 32)));

    wr(VIRTIO_STATUS, VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER | VIRTIO_S_FEATURES_OK);
    if (!(rd(VIRTIO_STATUS) & VIRTIO_S_FEATURES_OK))
        return -1;
    return 0;
}

#define OFF_DESC   0
#define OFF_AVAIL  256
#define OFF_USED   512

int blk_init(void)
{
    if (ready)
        return 0;
    if (find_device() < 0)
        return -1;
    if (negotiate() < 0)
        return -1;

    struct dmapage q;
    if (sys_dma_alloc(&q) < 0)
        return -1;
    desc  = (struct virtq_desc  *)(q.va + OFF_DESC);
    avail = (struct virtq_avail *)(q.va + OFF_AVAIL);
    used  = (struct virtq_used  *)(q.va + OFF_USED);

    wr(VIRTIO_QUEUE_SEL, 0);
    if (rd(VIRTIO_QUEUE_NUM_MAX) < VQ_SIZE)
        return -1;
    wr(VIRTIO_QUEUE_NUM, VQ_SIZE);
    wr(VIRTIO_QUEUE_DESC_LO,  (uint32)(q.pa + OFF_DESC));
    wr(VIRTIO_QUEUE_DESC_HI,  (uint32)((q.pa + OFF_DESC) >> 32));
    wr(VIRTIO_QUEUE_AVAIL_LO, (uint32)(q.pa + OFF_AVAIL));
    wr(VIRTIO_QUEUE_AVAIL_HI, (uint32)((q.pa + OFF_AVAIL) >> 32));
    wr(VIRTIO_QUEUE_USED_LO,  (uint32)(q.pa + OFF_USED));
    wr(VIRTIO_QUEUE_USED_HI,  (uint32)((q.pa + OFF_USED) >> 32));
    wr(VIRTIO_QUEUE_READY, 1);

    /* Header, sector and status in one page at offsets the device is told
       about individually — it never sees the page, only three addresses. */
    struct dmapage b;
    if (sys_dma_alloc(&b) < 0)
        return -1;
    hdr       = (struct blk_hdr *)b.va;
    data      = (uint8 *)(b.va + 64);
    status    = (volatile uint8 *)(b.va + 64 + BLK_SECSZ);
    hdr_pa    = b.pa;
    data_pa   = b.pa + 64;
    status_pa = b.pa + 64 + BLK_SECSZ;

    wr(VIRTIO_STATUS, VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER |
                      VIRTIO_S_FEATURES_OK | VIRTIO_S_DRIVER_OK);
    ready = 1;
    return 0;
}

/* Build the chain, ring the bell, and wait for the device to hand it back.
   Returns 0 if the device reported success. */
static int transact(int type, uint32 lba)
{
    hdr->type     = (uint32)type;
    hdr->reserved = 0;
    hdr->sector   = lba;
    *status       = 0xff;               /* so an untouched byte is not success */

    desc[0].addr  = hdr_pa;
    desc[0].len   = sizeof(struct blk_hdr);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next  = 1;

    desc[1].addr  = data_pa;
    desc[1].len   = BLK_SECSZ;
    /* WRITE means "the device writes here", so it is set for a read. Getting
       this backwards yields a request the device accepts and a buffer it
       never touches. */
    desc[1].flags = VIRTQ_DESC_F_NEXT |
                    (type == BLK_IN ? VIRTQ_DESC_F_WRITE : 0);
    desc[1].next  = 2;

    desc[2].addr  = status_pa;
    desc[2].len   = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE;
    desc[2].next  = 0;

    avail->ring[avail->idx % VQ_SIZE] = 0;   /* the head of the chain */
    fence();
    avail->idx++;
    fence();
    wr(VIRTIO_QUEUE_NOTIFY, 0);

    /* Polled, and yielding rather than spinning, so the rest of the system
       runs while the device works. */
    for (int spins = 0; spins < 1000000; spins++) {
        fence();
        if (used->idx != last_used) {
            last_used = used->idx;
            wr(VIRTIO_INT_ACK, rd(VIRTIO_INT_STATUS));
            return *status == 0 ? 0 : -1;
        }
        sys_yield();
    }
    return -1;                          /* the device never answered */
}

int blk_read(uint32 lba, void *buf)
{
    if (!ready && blk_init() < 0)
        return -1;
    if (transact(BLK_IN, lba) < 0)
        return -1;
    umemcpy(buf, data, BLK_SECSZ);
    return 0;
}

int blk_write(uint32 lba, const void *buf)
{
    if (!ready && blk_init() < 0)
        return -1;
    umemcpy(data, buf, BLK_SECSZ);
    return transact(BLK_OUT, lba);
}
