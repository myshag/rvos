#pragma once
#include "riscv.h"

/* virtio-mmio, as the QEMU 'virt' board exposes it: eight transports at fixed
   addresses with fixed interrupt lines. No PCI enumeration is needed, which
   is the whole reason to prefer this over an e1000 — a bus driver would be
   more code than the network driver itself. */
#define VIRTIO_MMIO_BASE   0x10001000UL
#define VIRTIO_MMIO_STRIDE 0x1000UL
#define VIRTIO_MMIO_SLOTS  8

/* Register offsets (virtio 1.x, "modern"). */
#define VIRTIO_MAGIC          0x000   /* 'virt' */
#define VIRTIO_VERSION        0x004   /* 2 = modern */
#define VIRTIO_DEVICE_ID      0x008   /* 1 = net, 2 = block, 0 = absent */
#define VIRTIO_VENDOR_ID      0x00c
#define VIRTIO_DEV_FEATURES   0x010
#define VIRTIO_DEV_FEAT_SEL   0x014
#define VIRTIO_DRV_FEATURES   0x020
#define VIRTIO_DRV_FEAT_SEL   0x024
#define VIRTIO_QUEUE_SEL      0x030
#define VIRTIO_QUEUE_NUM_MAX  0x034
#define VIRTIO_QUEUE_NUM      0x038
#define VIRTIO_QUEUE_READY    0x044
#define VIRTIO_QUEUE_NOTIFY   0x050
#define VIRTIO_INT_STATUS     0x060
#define VIRTIO_INT_ACK        0x064
#define VIRTIO_STATUS         0x070
#define VIRTIO_QUEUE_DESC_LO  0x080
#define VIRTIO_QUEUE_DESC_HI  0x084
#define VIRTIO_QUEUE_AVAIL_LO 0x090
#define VIRTIO_QUEUE_AVAIL_HI 0x094
#define VIRTIO_QUEUE_USED_LO  0x0a0
#define VIRTIO_QUEUE_USED_HI  0x0a4
#define VIRTIO_CONFIG         0x100

#define VIRTIO_MAGIC_VALUE    0x74726976U   /* "virt" */

/* Status bits, set in this order — the handshake is a state machine and the
   device rejects out-of-order steps. */
#define VIRTIO_S_ACKNOWLEDGE  1
#define VIRTIO_S_DRIVER       2
#define VIRTIO_S_DRIVER_OK    4
#define VIRTIO_S_FEATURES_OK  8
#define VIRTIO_S_FAILED       0x80

/* Feature bits. VERSION_1 lives at bit 32, hence the two-word selector. */
#define VIRTIO_NET_F_MAC      5
#define VIRTIO_F_VERSION_1    32

#define VIRTIO_ID_NET         1

/* ---- split virtqueue ------------------------------------------------
   Three arrays the driver and the device share: descriptors say where the
   buffers are, the available ring is the driver's outbox, the used ring is
   the device's. All addresses inside are physical. */
#define VQ_SIZE 8                       /* small, so every area fits a page */

#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2           /* device writes it, i.e. a receive buffer */

struct virtq_desc {
    uint64 addr;                        /* physical */
    uint32 len;
    uint16 flags;
    uint16 next;
};

struct virtq_avail {
    uint16 flags;
    uint16 idx;
    uint16 ring[VQ_SIZE];
};

struct virtq_used_elem {
    uint32 id;
    uint32 len;
};

struct virtq_used {
    uint16 flags;
    uint16 idx;
    struct virtq_used_elem ring[VQ_SIZE];
};

/* Every packet is prefixed by this. In virtio 1.x it is 12 bytes whether or
   not the merge-buffers feature was negotiated. */
struct virtio_net_hdr {
    uint8  flags;
    uint8  gso_type;
    uint16 hdr_len;
    uint16 gso_size;
    uint16 csum_start;
    uint16 csum_offset;
    uint16 num_buffers;
};
