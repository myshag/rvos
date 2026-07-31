/* sh.c — an interactive shell. It reads a line from the console server,
   splits it, and hands it to the loader, which is a function in the shared
   user text: same code, but the scratch buffer is ours because writable
   state is not shared between programs.

   Nothing here is privileged. Reading the keyboard is a message to the
   console server; running a program is three syscalls made on our behalf by
   spawn(). */
#include "syscall.h"
#include "vfs.h"
#include "ulib.h"
#include "servers.h"

#define LINEMAX 128

static char line[LINEMAX];
static char *argv[8];

int spawn(const char *path, int argc, char *const argv[]);   /* loader.c */

/* One character. This used to spin — read, get 0, yield, read again — because
   the console server always answered at once. It does not any more: a read
   with nothing to read is kept until a key arrives, and this call simply does
   not return until then. The loop is left in place for the case where a
   second reader is told 0, and it no longer runs. */

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Words, with quotes — which stopped being optional the moment a file could
   be called "a rather long file name.txt". Only double quotes, and no escape
   inside them: enough to name a file, and not the beginning of a language. */
static int split(char *s, char **out, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ')
            *s++ = 0;
        if (!*s)
            break;
        if (*s == '"') {
            *s++ = 0;
            out[n++] = s;
            while (*s && *s != '"')
                s++;
            if (*s)
                *s++ = 0;
        } else {
            out[n++] = s;
            while (*s && *s != ' ')
                s++;
        }
    }
    return n;
}

/* Where a bare word is looked for. There is no PATH variable and no reason
   for one yet: /BIN is where programs are, and a name with a slash in it is
   taken as written.

   Nothing here uppercases anything. The filesystem does: an 8.3 name is
   stored upper-cased and looked up the same way, so `ls` finds /BIN/LS.ELF
   without this code knowing that FAT16 shouts. */
static int find_program(const char *word, char *out, int cap)
{
    int has_slash = 0;
    for (const char *p = word; *p; p++)
        if (*p == '/')
            has_slash = 1;

    int n = 0;
    if (!has_slash) {
        const char *pre = "/BIN/";
        while (*pre && n < cap - 6)
            out[n++] = *pre++;
    }
    for (const char *p = word; *p && n < cap - 5; p++)
        out[n++] = *p;
    if (!has_slash) {
        const char *ext = ".ELF";
        while (*ext && n < cap - 1)
            out[n++] = *ext++;
    }
    out[n] = 0;
    return 0;
}

/* Start it, and wait unless the line ended in `&`. Waiting is what makes a
   shell of external commands usable at all: without it the prompt comes back
   in the middle of the program's output, which was tolerable when `cat` was a
   builtin and is not now that it is not. */
static void run_command(int con, int argc, char **argv)
{
    int background = 0;
    if (argc > 1 && streq(argv[argc - 1], "&")) {
        background = 1;
        argc--;
    }
    char path[VFS_PATH_MAX];
    find_program(argv[0], path, (int)sizeof(path));

    int tid = spawn(path, argc, argv);
    if (tid < 0) {
        uputs("sh: no such command: ");
        uputs(argv[0]);
        uputs("\n");
        return;
    }
    if (background) {
        uputs("[");
        char n[24];
        int k = uutoa((unsigned long)tid, n);
        n[k] = 0;
        uputs(n);
        uputs("]\n");
    } else {
        /* The terminal is told who is in front of it, so that Ctrl-C has
           something to mean while this shell is not reading anything. */
        /* По дескриптору, а не по имени: линию терминала именует дескриптор,
           и ioctl с fd = -1 не про неё. Пока за именем стоял драйвер с одной
           единственной консолью, разницы не было — с терминалами появилась. */
        vfs_ioctl_arg(con, IOCTL_INTR, tid);
        sys_wait(tid);
        vfs_ioctl_arg(con, IOCTL_INTR, 0);
    }
}

/* Написать команду в управляющий файл и прочитать ответ — та же форма, что у
   /net/ctl, потому что это тот же интерфейс. */
static int ctl_write(const char *path, const char *cmd, char *answer, int cap)
{
    int fd = vfs_open(path);
    if (fd < 0)
        return -1;
    int l = 0;
    while (cmd[l]) l++;
    if (vfs_write(fd, cmd, l) < 0) {
        vfs_close(fd);
        return -1;
    }
    int n = vfs_read(fd, answer, cap - 1);
    vfs_close(fd);
    if (n <= 0)
        return -1;
    answer[n] = 0;
    return n;
}

static int slot_of(const char *answer)
{
    if (answer[0] != 'o' || answer[1] != 'k')
        return -1;
    const char *p = answer + 2;
    while (*p == ' ')
        p++;
    if (*p < '0' || *p > '9')
        return -1;
    int v = 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    return v;
}

void sh_main(void)
{
    unsigned long go;
    sys_recv(&go, (int)sizeof(go));

    /* Обернуть последовательную линию терминалом — тем же, что стоит за
       сессиями по сети. Дисциплина одна на всю систему, а не по одной у
       каждого читателя, и backspace тут больше не разбирается.

       Своё пространство имён обязательно: привязка /dev/console в корневом
       поменяла бы смысл имени для всех, кто его наследует. Если терминала нет
       — работаем напрямую с устройством, потому что оболочка, которая не
       запустилась, хуже оболочки без правки строки. */
    int con = -1;
    {
        char answer[64];
        if (ctl_write("/tty/ctl", "new /dev/rawcons", answer,
                      (int)sizeof(answer)) > 0 && slot_of(answer) >= 0) {
            char path[24];
            int k = 0;
            for (const char *a = "/tty/"; *a; a++)
                path[k++] = *a;
            k += uutoa((unsigned long)slot_of(answer), path + k);
            path[k] = 0;
            sys_nsclone();
            con = vfs_open(path);
            if (con >= 0)
                sys_bind(path, "/dev/console", MREPL);
        }
    }
    if (con < 0)
        con = vfs_open("/dev/console");
    if (con < 0) {
        uputs("sh: no console\n");
        sys_exit();
    }

    uputs("\n--- sh (U-mode) ----------------------------------------\n");
    uputs("commands live in /BIN; try `ls`, `cat /README.TXT`, `free`\n");

    for (;;) {
        uputs("\nrvos$ ");
        /* Строка приходит уже отредактированной: backspace, ^U, ^W и ^D
           разбирает сервер терминала. Здесь остаётся чтение. */
        int len = vfs_read(con, line, LINEMAX - 1);
        if (len < 0)
            len = 0;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            len--;
        line[len] = 0;
        if (len == 0)
            continue;

        int argc = split(line, argv, 8);
        if (argc == 0)
            continue;

                                        /* Text to a file, which is how a control file is spoken to. Not a
           redirection — there are no pipes here — just the write a program
           would do, available from the prompt. */
        /* Measured on demand rather than at boot: a benchmark that runs
           while the system is busy measures the system, not the thing. It is
           a builtin because it needs two tasks already in the image talking
           to each other, and a program on the disk cannot arrange that. */
        if (streq(argv[0], "bench")) {
            unsigned long go = 0;
            sys_send(BENCH_TASK_ID, &go, (int)sizeof(go));
            continue;
        }

        if (streq(argv[0], "write")) {
            if (argc < 3) {
                uputs("usage: write <path> <text...>\n");
                continue;
            }
            char msg[LINEMAX];
            int k = 0;
            for (int i = 2; i < argc; i++) {
                if (i > 2)
                    msg[k++] = ' ';
                for (const char *q = argv[i]; *q && k < LINEMAX - 1; q++)
                    msg[k++] = *q;
            }
            msg[k] = 0;
            int fd = vfs_open(argv[1]);
            if (fd < 0) {
                uputs("write: cannot open ");
                uputs(argv[1]);
                uputs("\n");
                continue;
            }
            if (vfs_write(fd, msg, k) < 0)
                uputs("write: refused\n");
            char back[VFS_DATA_MAX + 1];
            int n = vfs_read(fd, back, VFS_DATA_MAX);
            if (n > 0) {
                back[n] = 0;
                uputs(back);
            }
            vfs_close(fd);
            continue;
        }

        /* Start a proxy for another machine's namespace and mount it, which
           is two calls and no new mechanism. */
        if (streq(argv[0], "import")) {
            if (argc < 4) {
                uputs("usage: import <a.b.c.d> <port> <prefix>\n");
                continue;
            }
            char *av[4];
            av[0] = (char *)"/BIN/IMPORT.ELF";
            av[1] = argv[1];
            av[2] = argv[2];
            av[3] = argv[3];
            int tid = spawn(av[0], 4, av);
            if (tid < 0)
                uputs("import: cannot run /BIN/IMPORT.ELF\n");
            else if (sys_mount(argv[3], tid, MREPL) < 0)
                uputs("import: no room in the mount table\n");
            else
                uputs("mounted\n");
            continue;
        }

        /* The other builtin, because it answers the question a loader raises:
           does running programs cost memory permanently? */
                run_command(con, argc, argv);
    }
}
