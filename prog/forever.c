/* forever.c — a program that does not stop, so that something else has to.

   It holds an open file while it loops, which is the interesting half: a task
   that is killed has no chance to close anything, and whatever a server was
   keeping on its behalf is the server's problem afterwards. */
#include "lib.h"
__attribute__((section(".text.start"))) void _start(void)
{
    int fd = vfs_open("/README.TXT");
    say(fd < 0 ? "no file; " : "holding a file open; ");
    say("looping, interrupt me\n");
    for (unsigned long i = 0;; i++)
        if ((i & 0xffffff) == 0)
            say(".");
}
