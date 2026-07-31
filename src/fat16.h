#pragma once
#include "riscv.h"

/* A FAT16 driver over a virtio-blk disk. Root directory only, whole files at
   a time: a file is read into a buffer and written back from one, never
   edited in place. That is a real limit and it removes an entire class of
   bug, since there is no partial state to be interrupted in. */

struct dirent {
    char   name[13];    /* 8.3, null-terminated */
    uint32 size;
    int    is_dir;
};

int fat16_init(void);                                   /* 0 ok, -1 bad FS */
int fat16_list_root(struct dirent *out, int max);       /* returns entry count */
int fat16_read(const char *name, void *out, int maxlen);/* bytes read, -1 missing */
int fat16_write(const char *name, const void *buf, int len); /* -> len, or -1 */
int fat16_remove(const char *name);                     /* 0, or -1 if absent */
