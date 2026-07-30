#pragma once
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
int  uart_tryc(void);   /* -1 if no RX byte waiting, else the byte */
/* minimal formatted output: %c %s %d %u %x %p %% */
void kprintf(const char *fmt, ...);
