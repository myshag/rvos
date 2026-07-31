/* cp.c — read one name, write another. */
#include "lib.h"

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    if (argc < 3) {
        say("usage: cp <src> <dst>\n");
        sys_exit();
    }
    if (pcopy(argv[1], argv[2]) < 0)
        err("cp", "failed:", argv[1]);
    sys_exit();
}
