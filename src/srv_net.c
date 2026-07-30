/* srv_net.c — a virtio-net driver running in user mode.

   It owns the device: the MMIO window is mapped into this address space and
   no other, so register access is ordinary loads and stores with no syscall
   on the path. What it cannot do for itself is get memory the *device* can
   reach — a device is programmed with physical addresses — so the rings and
   buffers come from SYS_DMA_ALLOC, the one call that reports both halves of
   a mapping.

   First milestone: find the card, complete the handshake, and put one frame
   on the wire. */
#include "syscall.h"
#include "ulib.h"
#include "virtio.h"

static volatile uint32 *dev;          /* MMIO base of the device we found */

static uint32 rd(int off)          { return dev[off / 4]; }
static void   wr(int off, uint32 v) { dev[off / 4] = v; }

/* The device and the driver write shared memory concurrently; without a fence
   the descriptor writes can still be in flight when the notify arrives, and
   the device reads a half-built ring. */
static void fence(void) { __asm__ volatile("fence rw, rw" ::: "memory"); }

static void say(const char *s) { uputs(s); }

static void say_num(const char *l, unsigned long v, const char *t)
{
    char n[24];
    int k = uutoa(v, n);
    n[k] = 0;
    say(l); say(n); say(t);
}

static void say_hex2(unsigned v)
{
    const char *d = "0123456789abcdef";
    char b[3] = { d[(v >> 4) & 0xf], d[v & 0xf], 0 };
    say(b);
}

/* ---- the transmit queue --------------------------------------------- */
static struct virtq_desc  *tx_desc;
static struct virtq_avail *tx_avail;
static struct virtq_used  *tx_used;
static uint64 tx_desc_pa, tx_avail_pa, tx_used_pa;

static char  *tx_buf;
static uint64 tx_buf_pa;

static uint8 mac[6];

/* ---- the receive queue ---------------------------------------------- */
#define NRX 4
static struct virtq_desc  *rx_desc;
static struct virtq_avail *rx_avail;
static struct virtq_used  *rx_used;
static uint64 rx_buf_pa[NRX];
static char  *rx_buf[NRX];
static uint16 rx_last;          /* how much of the used ring we have consumed */
static uint16 tx_seen;          /* likewise for transmit completions */
static int    net_irq;          /* mmio slot N is wired to interrupt N+1 */

/* Locate a virtio-mmio slot presenting the network device. */
static int find_device(void)
{
    for (int i = 0; i < VIRTIO_MMIO_SLOTS; i++) {
        volatile uint32 *p =
            (volatile uint32 *)(VIRTIO_MMIO_BASE + (uint64)i * VIRTIO_MMIO_STRIDE);
        if (p[VIRTIO_MAGIC / 4] != VIRTIO_MAGIC_VALUE)
            continue;
        if (p[VIRTIO_DEVICE_ID / 4] != VIRTIO_ID_NET)
            continue;
        dev = p;
        return i;
    }
    return -1;
}

/* The handshake is a state machine: the device rejects steps taken out of
   order, and refuses to run at all if FEATURES_OK does not stick. */
static int negotiate(void)
{
    wr(VIRTIO_STATUS, 0);                       /* reset */
    wr(VIRTIO_STATUS, VIRTIO_S_ACKNOWLEDGE);
    wr(VIRTIO_STATUS, VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER);

    /* Features are read a word at a time. VERSION_1 sits at bit 32, so it is
       bit 0 of the second word — a modern device will not proceed without it. */
    wr(VIRTIO_DEV_FEAT_SEL, 0);
    uint32 lo = rd(VIRTIO_DEV_FEATURES);
    wr(VIRTIO_DEV_FEAT_SEL, 1);
    uint32 hi = rd(VIRTIO_DEV_FEATURES);

    uint32 want_lo = lo & (1u << VIRTIO_NET_F_MAC);      /* just the address */
    uint32 want_hi = hi & (1u << (VIRTIO_F_VERSION_1 - 32));

    wr(VIRTIO_DRV_FEAT_SEL, 0);
    wr(VIRTIO_DRV_FEATURES, want_lo);
    wr(VIRTIO_DRV_FEAT_SEL, 1);
    wr(VIRTIO_DRV_FEATURES, want_hi);

    wr(VIRTIO_STATUS, VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER | VIRTIO_S_FEATURES_OK);
    if (!(rd(VIRTIO_STATUS) & VIRTIO_S_FEATURES_OK))
        return -1;                              /* it refused our selection */

    if (want_lo) {
        volatile uint8 *cfg = (volatile uint8 *)dev;
        for (int i = 0; i < 6; i++)
            mac[i] = cfg[VIRTIO_CONFIG + i];
    }
    return 0;
}

/* All three areas of one queue fit in a single page, at aligned offsets — the
   registers take arbitrary physical addresses, so they need not be adjacent
   to anything. */
#define OFF_DESC   0
#define OFF_AVAIL  256
#define OFF_USED   512

static int setup_queue(int q, struct virtq_desc **d, struct virtq_avail **a,
                       struct virtq_used **u)
{
    struct dmapage page;
    if (sys_dma_alloc(&page) < 0)
        return -1;

    *d = (struct virtq_desc *)(page.va + OFF_DESC);
    *a = (struct virtq_avail *)(page.va + OFF_AVAIL);
    *u = (struct virtq_used *)(page.va + OFF_USED);

    wr(VIRTIO_QUEUE_SEL, (uint32)q);
    if (rd(VIRTIO_QUEUE_NUM_MAX) < VQ_SIZE)
        return -1;
    wr(VIRTIO_QUEUE_NUM, VQ_SIZE);

    wr(VIRTIO_QUEUE_DESC_LO,  (uint32)(page.pa + OFF_DESC));
    wr(VIRTIO_QUEUE_DESC_HI,  (uint32)((page.pa + OFF_DESC) >> 32));
    wr(VIRTIO_QUEUE_AVAIL_LO, (uint32)(page.pa + OFF_AVAIL));
    wr(VIRTIO_QUEUE_AVAIL_HI, (uint32)((page.pa + OFF_AVAIL) >> 32));
    wr(VIRTIO_QUEUE_USED_LO,  (uint32)(page.pa + OFF_USED));
    wr(VIRTIO_QUEUE_USED_HI,  (uint32)((page.pa + OFF_USED) >> 32));
    wr(VIRTIO_QUEUE_READY, 1);

    if (q == 1) {
        tx_desc_pa  = page.pa + OFF_DESC;
        tx_avail_pa = page.pa + OFF_AVAIL;
        tx_used_pa  = page.pa + OFF_USED;
    }
    return 0;
}

/* Receive buffers must be published *before* the device is told to run,
   otherwise the first frames arrive with nowhere to go and are dropped. Each
   descriptor is marked WRITE: that flag is what tells the device this buffer
   is for it to fill rather than to read. */
static int setup_rx(void)
{
    for (int i = 0; i < NRX; i++) {
        struct dmapage b;
        if (sys_dma_alloc(&b) < 0)
            return -1;
        rx_buf[i]    = (char *)b.va;
        rx_buf_pa[i] = b.pa;

        rx_desc[i].addr  = b.pa;
        rx_desc[i].len   = 2048;
        rx_desc[i].flags = VIRTQ_DESC_F_WRITE;
        rx_desc[i].next  = 0;
        rx_avail->ring[i] = (uint16)i;
    }
    fence();
    rx_avail->idx = NRX;
    fence();
    wr(VIRTIO_QUEUE_NOTIFY, 0);
    return 0;
}

/* Hand a consumed buffer back so the device can fill it again. */
static void rx_recycle(uint16 id)
{
    rx_avail->ring[rx_avail->idx % VQ_SIZE] = id;
    fence();
    rx_avail->idx++;
    fence();
    wr(VIRTIO_QUEUE_NOTIFY, 0);
}

/* An ARP request for the gateway. A real frame rather than random bytes, so
   the other end has something to answer and a capture is readable. */
static int build_arp(char *p)
{
    static const uint8 bcast[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
    static const uint8 me_ip[4] = { 10, 0, 2, 15 };
    static const uint8 gw_ip[4] = { 10, 0, 2, 2 };
    int o = 0;

    umemcpy(p + o, bcast, 6); o += 6;          /* ethernet destination */
    umemcpy(p + o, mac, 6);   o += 6;          /* ethernet source */
    p[o++] = 0x08; p[o++] = 0x06;              /* ethertype: ARP */

    p[o++] = 0x00; p[o++] = 0x01;              /* hardware type: ethernet */
    p[o++] = 0x08; p[o++] = 0x00;              /* protocol type: IPv4 */
    p[o++] = 6;    p[o++] = 4;                 /* address lengths */
    p[o++] = 0x00; p[o++] = 0x01;              /* opcode: request */
    umemcpy(p + o, mac, 6);   o += 6;          /* sender hardware address */
    umemcpy(p + o, me_ip, 4); o += 4;          /* sender protocol address */
    umemset(p + o, 0, 6);     o += 6;          /* target hardware: unknown */
    umemcpy(p + o, gw_ip, 4); o += 4;          /* target protocol address */
    return o;                                  /* 42 bytes */
}

/* Block until the card says something happened. The device has its own
   interrupt-status register on top of the PLIC's masking, and both have to be
   cleared: VIRTIO_INT_ACK tells the card, SYS_IRQ_ACK tells the controller.
   Miss either and this is the last interrupt we ever see. */
static void wait_irq(void)
{
    unsigned long m;
    int from = sys_recv(&m, (int)sizeof(m));
    if (from == IRQ_SENDER) {
        wr(VIRTIO_INT_ACK, rd(VIRTIO_INT_STATUS));
        sys_irq_ack(net_irq);
    }
}

/* Wait for the next received frame. Returns its length and, through `id`, the
   descriptor to hand back once the caller is finished with the bytes. */
static char *receive(int *len, uint16 *id)
{
    for (;;) {
        fence();
        if (rx_used->idx != rx_last) {
            struct virtq_used_elem *e = &rx_used->ring[rx_last % VQ_SIZE];
            *id  = (uint16)e->id;
            *len = (int)e->len - (int)sizeof(struct virtio_net_hdr);
            rx_last++;
            return rx_buf[*id] + sizeof(struct virtio_net_hdr);
        }
        wait_irq();
    }
}

static int transmit(const char *frame, int len)
{
    struct virtio_net_hdr *h = (struct virtio_net_hdr *)tx_buf;
    umemset(h, 0, sizeof(*h));                 /* no offloads, no segmentation */
    umemcpy(tx_buf + sizeof(*h), frame, (unsigned long)len);

    tx_desc[0].addr  = tx_buf_pa;
    tx_desc[0].len   = (uint32)(sizeof(*h) + len);
    tx_desc[0].flags = 0;                      /* device reads it */
    tx_desc[0].next  = 0;

    tx_avail->ring[tx_avail->idx % VQ_SIZE] = 0;
    fence();                                   /* ring entry before the index */
    tx_avail->idx++;
    fence();                                   /* index before the doorbell */

    wr(VIRTIO_QUEUE_NOTIFY, 1);

    /* The single descriptor is reused, so the device must be finished with it
       before the next frame overwrites it. */
    tx_seen++;
    while (tx_used->idx != tx_seen) {
        fence();
        if (tx_used->idx == tx_seen)
            break;
        wait_irq();
    }
    return 0;
}

/* The one's-complement sum both IPv4 and ICMP use. Folding the carries back
   in is what makes it end-around; leaving that out gives a checksum that is
   right for small packets and wrong for large ones. */
static uint16 checksum(const uint8 *p, int len)
{
    uint32 sum = 0;
    for (int i = 0; i + 1 < len; i += 2)
        sum += ((uint32)p[i] << 8) | p[i + 1];
    if (len & 1)
        sum += (uint32)p[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16)(~sum & 0xffff);
}

static void put16(uint8 *p, unsigned v) { p[0] = (uint8)(v >> 8); p[1] = (uint8)v; }

static const uint8 me_ip[4] = { 10, 0, 2, 15 };
static const uint8 gw_ip[4] = { 10, 0, 2, 2 };
static uint8 gw_mac[6];

/* An ICMP echo request to the gateway, wrapped in IPv4 and ethernet. */
static int build_ping(uint8 *p, uint16 seq)
{
    int o = 0;
    umemcpy(p + o, gw_mac, 6); o += 6;
    umemcpy(p + o, mac, 6);    o += 6;
    put16(p + o, 0x0800);      o += 2;          /* ethertype: IPv4 */

    uint8 *ip = p + o;
    int icmp_len = 8 + 8;                       /* header + payload */
    ip[0] = 0x45;                               /* IPv4, 5-word header */
    ip[1] = 0;
    put16(ip + 2, (unsigned)(20 + icmp_len));   /* total length */
    put16(ip + 4, 0x1234);                      /* identification */
    put16(ip + 6, 0);                           /* no fragmentation */
    ip[8]  = 64;                                /* ttl */
    ip[9]  = 1;                                 /* protocol: ICMP */
    put16(ip + 10, 0);                          /* checksum, filled below */
    umemcpy(ip + 12, me_ip, 4);
    umemcpy(ip + 16, gw_ip, 4);
    put16(ip + 10, checksum(ip, 20));           /* header only, by definition */

    uint8 *ic = ip + 20;
    ic[0] = 8;                                  /* echo request */
    ic[1] = 0;
    put16(ic + 2, 0);                           /* checksum, filled below */
    put16(ic + 4, 0xabcd);                      /* identifier */
    put16(ic + 6, seq);
    for (int i = 0; i < 8; i++)
        ic[8 + i] = (uint8)('a' + i);
    put16(ic + 2, checksum(ic, icmp_len));      /* covers header and payload */

    return o + 20 + icmp_len;
}

void net_server(void)
{
    unsigned long go;
    sys_recv(&go, (int)sizeof(go));

    say("\n--- net (U-mode virtio-net driver) ---------------------\n");

    int slot = find_device();
    if (slot < 0) {
        say("  no virtio-net device found\n");
        sys_exit();
    }
    net_irq = slot + 1;               /* the 'virt' board wires them in order */
    sys_irq_register(net_irq);
    say_num("  found virtio-net in mmio slot ", (unsigned long)slot, "");
    say_num(", version ", (unsigned long)rd(VIRTIO_VERSION), "\n");

    if (negotiate() < 0) {
        say("  feature negotiation failed\n");
        sys_exit();
    }
    say("  mac ");
    for (int i = 0; i < 6; i++) {
        if (i) say(":");
        say_hex2(mac[i]);
    }
    say("\n");

    struct virtq_desc *d; struct virtq_avail *a; struct virtq_used *u;
    if (setup_queue(1, &d, &a, &u) < 0) {      /* queue 1 is transmit */
        say("  cannot set up the transmit queue\n");
        sys_exit();
    }
    tx_desc = d; tx_avail = a; tx_used = u;

    if (setup_queue(0, &d, &a, &u) < 0) {      /* queue 0 is receive */
        say("  cannot set up the receive queue\n");
        sys_exit();
    }
    rx_desc = d; rx_avail = a; rx_used = u;
    if (setup_rx() < 0) {
        say("  no dma pages for receive buffers\n");
        sys_exit();
    }

    struct dmapage buf;
    if (sys_dma_alloc(&buf) < 0) {
        say("  no dma page for the buffer\n");
        sys_exit();
    }
    tx_buf = (char *)buf.va;
    tx_buf_pa = buf.pa;

    wr(VIRTIO_STATUS, VIRTIO_S_ACKNOWLEDGE | VIRTIO_S_DRIVER |
                      VIRTIO_S_FEATURES_OK | VIRTIO_S_DRIVER_OK);
    say("  driver ok; queues live\n");

    char frame[64];
    int len = build_arp(frame);
    if (transmit(frame, len) < 0) {
        say("  transmit timed out\n");
        for (;;) _ecall1(SYS_YIELD, 0);
    }
    say_num("  sent an ARP request for 10.0.2.2, ", (unsigned long)len,
            " bytes\n");

    /* From here on the driver is interrupt-driven: it blocks in receive(),
       which blocks in sys_recv(), which the kernel wakes when the card fires.
       No polling anywhere. */
    int have_gw = 0, pinged = 0, replies = 0;
    while (replies == 0) {
        int flen; uint16 id;
        uint8 *f = (uint8 *)receive(&flen, &id);
        unsigned etype = ((unsigned)f[12] << 8) | f[13];

        if (etype == 0x0806 && f[21] == 2 && !have_gw) {     /* ARP reply */
            umemcpy(gw_mac, f + 22, 6);
            have_gw = 1;
            say("  arp: 10.0.2.2 is at ");
            for (int i = 0; i < 6; i++) { if (i) say(":"); say_hex2(gw_mac[i]); }
            say("\n");
        } else if (etype == 0x0800 && f[23] == 1) {          /* IPv4 + ICMP */
            uint8 *ic = f + 14 + 20;
            if (ic[0] == 0) {                                /* echo reply */
                unsigned seq = ((unsigned)ic[6] << 8) | ic[7];
                say_num("  ping reply from 10.0.2.2, seq ",
                        (unsigned long)seq, ", ");
                say_num("", (unsigned long)flen, " bytes\n");
                replies++;
            }
        }

        rx_recycle(id);

        if (have_gw && !pinged) {
            uint8 ping[64];
            int plen = build_ping(ping, 1);
            if (transmit((const char *)ping, plen) == 0) {
                say_num("  sent an ICMP echo request, ", (unsigned long)plen,
                        " bytes\n");
                pinged = 1;
            }
        }
    }

    for (;;)
        _ecall1(SYS_YIELD, 0);
}
