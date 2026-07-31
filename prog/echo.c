/* echo.c — the words back, and a newline. */
#include "lib.h"

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            say(" ");
        say(argv[i]);
    }
    say("\n");
    sys_exit();
}
