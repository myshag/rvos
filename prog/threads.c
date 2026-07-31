/* threads.c — несколько нитей управления в одном адресном пространстве.

   Задача здесь всегда была адресным пространством и одной нитью. sys_thread
   разделяет эти два понятия: новая задача получает таблицу страниц
   вызывающего, а не свою, и отличается от него ровно одним — собственным
   сохранённым контекстом. Всё остальное в системе не заметило разницы:
   планировщик, ловушки, сообщения и /proc работают с нитью как с задачей,
   потому что она ею и является.

   Стек — забота вызывающего, и это не лень. Стек каждой задачи лежит по
   одному и тому же виртуальному адресу, что работает лишь потому, что
   адресные пространства разные. Двум нитям в одном пространстве он достаться
   не может, поэтому стек выделяет тот, кто создаёт нить, — обычным malloc из
   общей кучи.

   Программа показывает три вещи: что память действительно общая, что от этого
   немедленно появляются гонки, и что с ними делать.

   usage: /BIN/THREADS.ELF                                                  */
#include "lib.h"

#define NTHREAD  4
#define BUMPS    300
#define STACK    8192

/* Общее — оно в .bss этой программы, то есть в той самой странице, которую
   все нити видят по одному адресу. */
static volatile long counter;
static volatile int  guard;
static int           tid[NTHREAD];
static char         *stack[NTHREAD];
static volatile int  phase;

static void lock(void)
{
    while (__sync_lock_test_and_set(&guard, 1))
        sys_yield();
}

static void unlock(void)
{
    __sync_lock_release(&guard);
}

/* Одна нить. Аргумент приходит в a0, как первый аргумент обычной функции —
   ядро просто кладёт его туда перед первым переходом. */
static void worker(long n)
{
    if (phase == 0) {
        /* Приращение — это чтение, сложение и запись, и между чтением и
           записью задачу может вытеснить таймер. Окно есть всегда; здесь оно
           расширено до наблюдаемого явной уступкой процессора, иначе нить
           успевает пройти весь цикл в пределах одного кванта в 50 мс и гонка
           не проявляется ни разу. Расширено, а не создано — разница именно
           в этом. */
        for (int i = 0; i < BUMPS; i++) {
            long v = counter;
            sys_yield();
            counter = v + 1;
        }
    } else if (phase == 1) {
        for (int i = 0; i < BUMPS; i++) {
            lock();
            long v = counter;
            sys_yield();            /* то же окно, но под замком */
            counter = v + 1;
            unlock();
        }
    } else {
        /* Распределитель памяти общий, потому что куча общая. Если бы он не
           брал замок, эти четыре нити разорвали бы его список свободного. */
        for (int i = 0; i < 400; i++) {
            void *p = malloc((unsigned long)(i * 37 % 500 + 1));
            if (!p) {
                say("нить: память кончилась\n");
                break;
            }
            *(char *)p = (char)n;
            free(p);
        }
    }
    sys_exit();
}

/* Возвращает, сколько нитей действительно пошло: слот задачи может и не
   найтись, и тогда ожидаемое число приращений другое. */
static int run_round(const char *what)
{
    int started = 0;
    counter = 0;
    for (int i = 0; i < NTHREAD; i++) {
        tid[i] = sys_thread(worker, stack[i] + STACK, i);
        if (tid[i] >= 0)
            started++;
        else
            say("нет свободного слота под нить\n");
    }
    for (int i = 0; i < NTHREAD; i++)
        if (tid[i] >= 0)
            sys_wait(tid[i]);
    say(what);
    return started;
}

__attribute__((section(".text.start"))) void _start(void)
{
    say("я задача ");
    sayn((unsigned long)sys_self());
    say(", счётчик по адресу ");
    sayn((unsigned long)&counter);
    say("\n\n");

    for (int i = 0; i < NTHREAD; i++) {
        stack[i] = malloc(STACK);
        if (!stack[i]) {
            say("нет памяти под стек\n");
            sys_exit();
        }
    }

    phase = 0;
    int n = run_round("без замка:  ");
    unsigned long want = (unsigned long)n * BUMPS;
    sayn((unsigned long)counter);
    say(" из ");
    sayn(want);
    say(counter == (long)want ? "  (в этот раз повезло)\n"
                              : "  — потеряно на гонке\n");

    phase = 1;
    n = run_round("с замком:   ");
    want = (unsigned long)n * BUMPS;
    sayn((unsigned long)counter);
    say(" из ");
    sayn(want);
    say(counter == (long)want ? "  сходится\n" : "  РАСХОЖДЕНИЕ\n");

    phase = 2;
    run_round("1600 malloc/free из четырёх нитей: прошло\n");

    struct mstat st;
    malloc_stat(&st);
    say("куча: взято ");
    sayn(st.taken / 1024);
    say("K, свободно ");
    sayn(st.idle / 1024);
    say("K в ");
    sayn((unsigned long)st.pieces);
    say(" кусках\n");

    for (int i = 0; i < NTHREAD; i++)
        free(stack[i]);
    sys_exit();
}
