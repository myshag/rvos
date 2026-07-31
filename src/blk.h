#pragma once
#include "riscv.h"

/* The line between the disk driver and the filesystem, drawn where the same
   line is drawn on the network side: srv_blk.c knows about virtqueues and
   sectors, fat16.c knows about directory entries and cluster chains. They
   share a task, because a message per sector would be absurd, but not a file. */

#define BLK_SECSZ 512

int blk_init(void);                      /* srv_blk.c; 0 on success */
int blk_read(uint32 lba, void *buf);     /* one sector */
int blk_write(uint32 lba, const void *buf);
