/* uart.c — NS16550A driver for the QEMU 'virt' UART at 0x10000000, plus a
   tiny kprintf. Polled TX; that's all an educational console needs. */
#include <stdarg.h>
#include "uart.h"

#define UART_BASE 0x10000000UL
#define REG(off)  (*(volatile unsigned char *)(UART_BASE + (off)))

enum {
    RBR = 0, THR = 0, IER = 1, FCR = 2, LCR = 3, LSR = 5,
};
#define LSR_THRE (1u << 5)   /* TX holding register empty */
#define LSR_DR   (1u << 0)   /* RX data ready */

void uart_init(void)
{
    REG(IER) = 0x00;         /* no interrupts (polled) */
    REG(LCR) = 0x03;         /* 8N1 */
    REG(FCR) = 0x01;         /* enable FIFO */
}

void uart_putc(char c)
{
    if (c == '\n')
        uart_putc('\r');
    while (!(REG(LSR) & LSR_THRE))
        ;
    REG(THR) = (unsigned char)c;
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static void print_uint(unsigned long v, unsigned base, int width_neg)
{
    char buf[32];
    const char *digits = "0123456789abcdef";
    int i = 0;
    if (v == 0)
        buf[i++] = '0';
    while (v) {
        buf[i++] = digits[v % base];
        v /= base;
    }
    (void)width_neg;
    while (i > 0)
        uart_putc(buf[--i]);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            uart_putc(*fmt);
            continue;
        }
        switch (*++fmt) {
        case 'c': uart_putc((char)va_arg(ap, int)); break;
        case 's': uart_puts(va_arg(ap, const char *)); break;
        case 'd': {
            long v = va_arg(ap, int);
            if (v < 0) { uart_putc('-'); v = -v; }
            print_uint((unsigned long)v, 10, 0);
            break;
        }
        case 'u': print_uint(va_arg(ap, unsigned int), 10, 0); break;
        case 'x': print_uint(va_arg(ap, unsigned int), 16, 0); break;
        case 'l': {                       /* %ld %lu %lx */
            int base = 10; long sv; unsigned long uv;
            char k = *++fmt;
            if (k == 'd') { sv = va_arg(ap, long);
                            if (sv < 0) { uart_putc('-'); sv = -sv; }
                            print_uint((unsigned long)sv, 10, 0); break; }
            if (k == 'x') base = 16;
            uv = va_arg(ap, unsigned long);
            print_uint(uv, base, 0);
            break;
        }
        case 'p': uart_puts("0x"); print_uint((unsigned long)va_arg(ap, void *), 16, 0); break;
        case '%': uart_putc('%'); break;
        default:  uart_putc('%'); uart_putc(*fmt); break;
        }
    }
    va_end(ap);
}
