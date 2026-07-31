#pragma once
#include "riscv.h"

/* ulib — the user side's own libc. It exists because user programs may not
   call into the kernel: util.c sits in kernel text, which is mapped without
   the U bit precisely so user mode cannot touch it. The names are prefixed
   rather than being memcpy/strlen so both copies can coexist in one binary.

   Everything here is linked into the shared user text region. */
void  *umemcpy(void *dst, const void *src, unsigned long n);
void  *umemset(void *dst, int c, unsigned long n);
int    umemcmp(const void *a, const void *b, unsigned long n);
int    ustrlen(const char *s);
void   ustrcpy(char *dst, const char *src);
int    uutoa(unsigned long v, char *out);   /* decimal, no NUL; -> length */
int    uxtoa(unsigned long v, char *out);   /* lowercase hex, no NUL */
int    ustr_has_prefix(const char *s, const char *p);

/* Console of last resort: one character per trap, needing no server. Servers
   use it for their startup line, before anyone is listening. */
void   uputs(const char *s);

/* The clock, read directly. sys_alarm says "wake me in N milliseconds", which
   is enough for a task with one deadline; a task juggling several has to know
   the current time to work out which comes first. scounteren.TM makes rdtime
   legal in user mode, so this is one instruction and no trap. */
uint64 unow_ticks(void);
uint64 unow_ms(void);
