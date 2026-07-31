/* frag.c — what taking memory and giving it back out of order does to a heap.

   There is no defragmenter in this system and there cannot be a compacting
   one: malloc hands the caller a bare pointer, so a block that is in use
   cannot be moved without invalidating it. What the allocator has instead is
   coalescing, which joins free neighbours, and that is enough for some
   patterns and useless for others. This program is the difference, in
   numbers.

   usage: /BIN/FRAG.ELF                                                    */
#include "lib.h"

/* Columns, not bytes: a label may be in any alphabet and a continuation byte
   takes no room on the screen. The same distinction the panels had to make. */
static void pad_to(const char *s, int cols)
{
    int shown = 0;
    for (int i = 0; s[i]; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            shown++;
    while (shown++ < cols)
        say(" ");
}

static void line(const char *what)
{
    unsigned long taken, idle, big; int pieces;
    malloc_stat(&taken, &idle, &pieces, &big);
    say(what);
    pad_to(what, 26);
    say(" taken "); sayn(taken / 1024);
    say("K  free "); sayn(idle / 1024);
    say("K  in "); sayn((unsigned long)pieces);
    say(" pieces  largest "); sayn(big); say("\n");
}

#define N 2000
static char *v[N];

__attribute__((section(".text.start"))) void _start(void)
{
    line("at rest");

    /* 1. взять много мелких и отдать все — соседние сливаются */
    for (int i = 0; i < N; i++) v[i] = malloc(64);
    line("2000 x 64 bytes");
    for (int i = 0; i < N; i++) free(v[i]);
    line("all freed");

    /* 2. шахматный порядок: отдать каждый второй */
    for (int i = 0; i < N; i++) v[i] = malloc(64);
    for (int i = 0; i < N; i += 2) { free(v[i]); v[i] = 0; }
    line("every second one freed");
    char *big = malloc(4096);
    line("after asking for 4096");
    say(big ? "  got the 4096\n" : "  did not get the 4096\n");
    free(big);
    for (int i = 1; i < N; i += 2) free(v[i]);
    line("the rest freed");

    /* 3. чередование крупных и мелких, потом отдать только крупные */
    for (int i = 0; i < 200; i++) {
        v[i * 2]     = malloc(1024);
        v[i * 2 + 1] = malloc(48);
    }
    line("200 x (1K then 48 bytes)");
    for (int i = 0; i < 200; i++) { free(v[i * 2]); v[i * 2] = 0; }
    line("only the 1K ones freed");
    char *huge = malloc(64 * 1024);
    line("after asking for 64K");
    say(huge ? "  got the 64K\n" : "  did not get the 64K\n");
    free(huge);
    for (int i = 0; i < 200; i++) free(v[i * 2 + 1]);
    line("everything freed");
    sys_exit();
}
