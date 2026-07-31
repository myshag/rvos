/* ulib.c — user-side libc, linked into the shared user text region. */
#include "ulib.h"
#include "syscall.h"

void *umemcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *umemset(void *dst, int c, unsigned long n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

int umemcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

int ustrlen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

void ustrcpy(char *dst, const char *src)
{
    while ((*dst++ = *src++))
        ;
}

int uutoa(unsigned long v, char *out)
{
    char tmp[24];
    int n = 0;
    if (v == 0)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

int uxtoa(unsigned long v, char *out)
{
    char tmp[24];
    const char *d = "0123456789abcdef";
    int n = 0;
    if (v == 0)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = d[v & 0xf];
        v >>= 4;
    }
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

int ustr_has_prefix(const char *s, const char *p)
{
    while (*p) {
        if (*s != *p)
            return 0;
        s++; p++;
    }
    return 1;
}

void uputs(const char *s)
{
    while (*s)
        _ecall1(SYS_PUTC, *s++);
}

/* The clock. This is the one piece of hardware a user program touches without
   the kernel's help — smain sets scounteren.TM, and rdtime becomes legal.
   QEMU's time base is 10 MHz, so a millisecond is 10000 ticks. */
uint64 unow_ticks(void)
{
    uint64 x;
    __asm__ volatile("rdtime %0" : "=r"(x));
    return x;
}

uint64 unow_ms(void) { return unow_ticks() / 10000UL; }
