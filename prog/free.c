/* free.c — pages, from the one place that knows: the kernel's allocator. */
#include "lib.h"

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    (void)argc; (void)argv;
    int m[2] = { 0, 0 };
    sys_meminfo(m);
    say("free ");
    sayn((unsigned long)m[0]);
    say(" of ");
    sayn((unsigned long)m[1]);
    say(" pages (");
    sayn((unsigned long)m[0] * 4);
    say(" KiB free)\n");
    sys_exit();
}
