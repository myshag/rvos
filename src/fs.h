#pragma once
#include "fat16.h"

/* Filesystem service protocol. The one-word IPC message carries a pointer to a
   shared fs_req (rvos tasks share an address space). The client fills op/name/
   buf, the server fills result, then acks. */
enum { FS_LIST = 1, FS_READ = 2 };

struct fs_req {
    int   op;
    char  name[16];     /* for FS_READ */
    void *buf;          /* FS_LIST: struct dirent[]; FS_READ: byte buffer */
    int   buf_len;      /* capacity (entries or bytes) */
    int   result;       /* server-filled: count / bytes, -1 on error */
};
