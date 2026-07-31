#pragma once
#include "riscv.h"

/* A FAT16 driver over a virtio-blk disk. Whole files at a time: a file is
   read into a buffer and written back from one, never edited in place. That
   is a real limit and it removes an entire class of bug, since there is no
   partial state to be interrupted in.

   Paths are full paths, with directories: "/DOCS/NOTE.TXT". The root is not a
   directory in FAT16 — it is a fixed run of sectors with no chain and no
   entry describing it — and that difference is confined to one function. */

/* Long enough for a name a person would write, and short enough that a
   listing of one fits in a message. A name longer than this is refused
   rather than silently shortened. */
#define FAT_NAME_MAX 96

struct dirent {
    char   name[FAT_NAME_MAX];  /* UTF-8, null-terminated */
    uint32 size;
    int    is_dir;
};

int fat16_init(void);                                   /* 0 ok, -1 bad FS */
int fat16_list(const char *path, struct dirent *out, int max); /* -1 not a dir */
int fat16_read(const char *path, void *out, int maxlen);/* bytes read, -1 missing */
int fat16_size(const char *path);                       /* bytes, or -1 */
int fat16_write(const char *path, const void *buf, int len); /* -> len, or -1 */
int fat16_remove(const char *path);         /* files, and empty directories */
int fat16_mkdir(const char *path);                      /* 0, or -1 */
