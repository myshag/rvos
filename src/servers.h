#pragma once

/* Server tasks. IDs are assignment order in kmain() — task_create() hands out
   0,1,2,... — and they're what the namespace binds path prefixes to. */
#define FS_TASK_ID      0
#define CONSOLE_TASK_ID 1
#define PROC_TASK_ID    2

void fs_server(void);        /* srv_fs.c      — FAT16 behind the interface */
void console_server(void);   /* srv_console.c — UART behind the interface */
void proc_server(void);      /* srv_proc.c    — task table as files */
