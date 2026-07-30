#pragma once
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
/* minimal formatted output: %c %s %d %u %x %p %% */
void kprintf(const char *fmt, ...);
