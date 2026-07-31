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
void net_timeout(void);                          /* net_ip.c: the alarm fired */

struct vfs_req;
/* Serve one client request. Returns 1 if the answer is in *r and should go
   back now, 0 if the request has been parked — a read with nothing to read,
   an accept with nothing to accept — and will be answered later through
   net_reply(). That is all "blocking" is here: a server declining to reply
   yet, while the caller sits in the sys_recv it already had to make. */
int  net_vfs(int from, struct vfs_req *r);       /* net_ip.c */
void net_reply(int to, struct vfs_req *r);       /* srv_net.c */
