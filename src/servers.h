#pragma once

/* Tasks. IDs are creation order in kmain() — task_create() hands out 0,1,2,…
   — and they are what a namespace binds path prefixes to. */
#define FS_TASK_ID      0
#define CONSOLE_TASK_ID 1
#define PROC_TASK_ID    2
#define NULL_TASK_ID    3
#define SHELL_TASK_ID   4
#define SANDBOX_TASK_ID 5
#define SNOOPER_TASK_ID 6
#define USER_TASK_ID    7
#define IDLE_TASK_ID    8

void fs_server(void);        /* srv_fs.c      — FAT16 behind the interface */
void console_server(void);   /* srv_console.c — UART behind the interface */
void proc_server(void);      /* srv_proc.c    — kernel state as files */
void null_server(void);      /* srv_null.c    — the bit bucket */
