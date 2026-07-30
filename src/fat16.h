#pragma once
#include "riscv.h"

/* Read-only FAT16 driver over a RAM-backed block device (the disk image is
   loaded into guest memory at boot). Root-directory only — enough to list and
   read files for an educational filesystem. */

struct dirent {
    char   name[13];    /* 8.3, null-terminated */
    uint32 size;
    int    is_dir;
};

int fat16_init(void);                                   /* 0 ok, -1 bad FS */
int fat16_list_root(struct dirent *out, int max);       /* returns entry count */
int fat16_read(const char *name, void *out, int maxlen);/* bytes read, -1 missing */
