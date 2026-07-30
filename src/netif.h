#pragma once
#include "riscv.h"

/* The line between the driver and the protocols. srv_net.c knows about
   virtqueues and frames and nothing about addresses; net_ip.c knows about
   addresses and nothing about the card. They are still one task — splitting
   them into two would mean a message per packet — but they are separate
   files so neither can quietly reach into the other. */

extern uint8 net_mac[6];               /* filled in by the driver */

int  net_transmit(const void *frame, int len);   /* srv_net.c */
void net_puts(const char *s);                    /* srv_net.c: console */
void net_putn(const char *label, unsigned long v, const char *tail);

void net_start(void);                            /* net_ip.c: begin the demo */
void net_input(uint8 *frame, int len);           /* net_ip.c: a frame arrived */
