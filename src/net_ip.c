/* net_ip.c — ARP, IPv4, UDP and a minimal TCP client.

   Scope, stated plainly. UDP here is complete: an 8-byte header and a
   checksum over the pseudo-header, which is all UDP is. TCP is not. What is
   implemented is a client that opens a connection, sends, receives and closes
   in order, and retransmits what goes unacknowledged. What is still missing
   is out-of-order reassembly, window management beyond a fixed advertised
   window, congestion control, and a random initial sequence number. */
#include "netif.h"
#include "ulib.h"
#include "syscall.h"

#define ETH_ARP   0x0806
#define ETH_IPV4  0x0800
#define IP_ICMP   1
#define IP_TCP    6
#define IP_UDP    17

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

static const uint8 me_ip[4] = { 10, 0, 2, 15 };
static const uint8 gw_ip[4] = { 10, 0, 2, 2 };
static uint8 gw_mac[6];
static int   have_gw;

#define UDP_PORT 9999
#define TCP_PORT 9998

/* ---- helpers --------------------------------------------------------- */

static void put16(uint8 *p, unsigned v) { p[0] = (uint8)(v >> 8); p[1] = (uint8)v; }
static void put32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v >> 24); p[1] = (uint8)(v >> 16);
    p[2] = (uint8)(v >> 8);  p[3] = (uint8)v;
}
static unsigned get16(const uint8 *p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint32   get32(const uint8 *p)
{
    return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) |
           ((uint32)p[2] << 8)  | p[3];
}

/* One's complement sum with the carries folded back in. Forgetting the fold
   gives a checksum that is correct for short packets and wrong for long ones. */
static uint32 sum16(const uint8 *p, int len, uint32 sum)
{
    for (int i = 0; i + 1 < len; i += 2)
        sum += ((uint32)p[i] << 8) | p[i + 1];
    if (len & 1)
        sum += (uint32)p[len - 1] << 8;
    return sum;
}
static uint16 fold(uint32 sum)
{
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16)(~sum & 0xffff);
}

/* UDP and TCP checksum a pseudo-header that is not on the wire: the two
   addresses, the protocol and the segment length. It exists so a segment
   delivered to the wrong host fails its checksum. */
static uint16 l4_checksum(int proto, const uint8 *src, const uint8 *dst,
                          const uint8 *seg, int len)
{
    uint8 ph[12];
    umemcpy(ph, src, 4);
    umemcpy(ph + 4, dst, 4);
    ph[8] = 0;
    ph[9] = (uint8)proto;
    put16(ph + 10, (unsigned)len);
    return fold(sum16(seg, len, sum16(ph, 12, 0)));
}

/* ---- frame construction ---------------------------------------------- */

static uint8 out[1600];

static int eth_hdr(uint8 *p, const uint8 *dst_mac, unsigned type)
{
    umemcpy(p, dst_mac, 6);
    umemcpy(p + 6, net_mac, 6);
    put16(p + 12, type);
    return 14;
}

/* Returns the offset of the payload; the caller fills it and calls ip_send. */
static int ip_hdr(uint8 *p, int proto, int payload_len)
{
    int o = eth_hdr(p, gw_mac, ETH_IPV4);
    uint8 *ip = p + o;
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, (unsigned)(20 + payload_len));
    put16(ip + 4, 0x4321);
    put16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = (uint8)proto;
    put16(ip + 10, 0);
    umemcpy(ip + 12, me_ip, 4);
    umemcpy(ip + 16, gw_ip, 4);
    put16(ip + 10, fold(sum16(ip, 20, 0)));    /* header only, by definition */
    return o + 20;
}

static void arp_request(void)
{
    static const uint8 bcast[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
    int o = eth_hdr(out, bcast, ETH_ARP);
    uint8 *a = out + o;
    put16(a, 1); put16(a + 2, ETH_IPV4);
    a[4] = 6; a[5] = 4;
    put16(a + 6, 1);                            /* request */
    umemcpy(a + 8, net_mac, 6);
    umemcpy(a + 14, me_ip, 4);
    umemset(a + 18, 0, 6);
    umemcpy(a + 24, gw_ip, 4);
    net_transmit(out, o + 28);
}

static void udp_send(unsigned dport, const char *msg)
{
    int len = ustrlen(msg);
    int o = ip_hdr(out, IP_UDP, 8 + len);
    uint8 *u = out + o;
    put16(u, 40000);                            /* our source port */
    put16(u + 2, dport);
    put16(u + 4, (unsigned)(8 + len));
    put16(u + 6, 0);
    umemcpy(u + 8, msg, (unsigned long)len);
    put16(u + 6, l4_checksum(IP_UDP, me_ip, gw_ip, u, 8 + len));
    net_transmit(out, o + 8 + len);
}

/* ---- a very small TCP ------------------------------------------------- */

enum { T_CLOSED, T_SYN_SENT, T_ESTABLISHED, T_FIN_SENT, T_DONE };
static int    tcp_state;
static uint32 snd_nxt, rcv_nxt;
static int    sent_payload;

/* The last segment that consumed sequence space, kept until it is
   acknowledged. Retransmission is the whole of TCP's reliability: a segment
   is not "sent", it is "sent and not yet given up on". */
static uint8  rt_frame[1600];
static int    rt_len;                   /* 0 = nothing outstanding */
static uint32 rt_seq_end;               /* the ack that would retire it */
static int    rt_tries;
static int    rt_rto = 300;             /* milliseconds, doubled on each loss */

/* A test hook. On a virtual link nothing is ever lost, so the retransmit path
   would never run and its correctness would be a matter of opinion. This
   drops exactly one segment on the floor after it is built. */
static int    drop_next = 1;

static void tcp_send(unsigned flags, const char *data, int dlen)
{
    int o = ip_hdr(out, IP_TCP, 20 + dlen);
    uint8 *t = out + o;
    put16(t, 40001);                            /* source port */
    put16(t + 2, TCP_PORT);
    put32(t + 4, snd_nxt);
    put32(t + 8, rcv_nxt);
    t[12] = 5 << 4;                             /* 20-byte header, no options */
    t[13] = (uint8)flags;
    put16(t + 14, 4096);                        /* a fixed window: no flow control */
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (dlen)
        umemcpy(t + 20, data, (unsigned long)dlen);
    put16(t + 16, l4_checksum(IP_TCP, me_ip, gw_ip, t, 20 + dlen));

    int flen = o + 20 + dlen;

    /* SYN and FIN each consume a sequence number, which is why a handshake
       advances the stream without carrying any data. */
    uint32 consumed = (uint32)dlen + ((flags & (TCP_SYN | TCP_FIN)) ? 1 : 0);

    if (consumed) {
        /* Keep the bytes, not the intent: a retransmission must be the same
           segment, down to the sequence number it carried. */
        umemcpy(rt_frame, out, (unsigned long)flen);
        rt_len     = flen;
        rt_seq_end = snd_nxt + consumed;
        rt_tries   = 0;
        rt_rto     = 300;
        sys_alarm(rt_rto);
    }

    if (drop_next && consumed) {
        drop_next = 0;
        net_puts("  tcp: [test] dropping this segment before it reaches the card\n");
    } else {
        net_transmit(out, flen);
    }

    snd_nxt += consumed;
}

/* Called when the alarm fires with a segment still outstanding. */
void net_timeout(void)
{
    if (!rt_len)
        return;
    if (++rt_tries > 5) {
        net_puts("  tcp: giving up after 5 retransmissions\n");
        rt_len = 0;
        tcp_state = T_DONE;
        return;
    }
    net_putn("  tcp: timeout, retransmitting (attempt ",
             (unsigned long)rt_tries, ")\n");
    net_transmit(rt_frame, rt_len);
    rt_rto *= 2;                        /* exponential backoff */
    sys_alarm(rt_rto);
}

/* An acknowledgement retires the outstanding segment. */
static void tcp_acked(uint32 ack)
{
    if (rt_len && (int32)(ack - rt_seq_end) >= 0) {
        rt_len = 0;
        sys_alarm(0);                   /* cancel the timer */
    }
}

static void tcp_open(void)
{
    snd_nxt = 1000;                             /* a fixed ISS: no randomness */
    rcv_nxt = 0;
    tcp_state = T_SYN_SENT;
    tcp_send(TCP_SYN, 0, 0);
    net_puts("  tcp: SYN -> 10.0.2.2:9998\n");
}

static void tcp_input(const uint8 *t, int seglen)
{
    unsigned flags = t[13];
    int      hlen  = (t[12] >> 4) * 4;
    int      dlen  = seglen - hlen;
    uint32   seq   = get32(t + 4);

    if (flags & TCP_RST) {
        net_puts("  tcp: reset by peer\n");
        tcp_state = T_DONE;
        return;
    }

    if (flags & TCP_ACK)
        tcp_acked(get32(t + 8));

    if (tcp_state == T_SYN_SENT && (flags & TCP_SYN) && (flags & TCP_ACK)) {
        rcv_nxt = seq + 1;                      /* their SYN counts as one */
        tcp_state = T_ESTABLISHED;
        tcp_send(TCP_ACK, 0, 0);
        net_puts("  tcp: SYN-ACK received, connection established\n");

        const char *msg = "hello from rvos\n";
        tcp_send(TCP_ACK | TCP_PSH, msg, ustrlen(msg));
        sent_payload = 1;
        net_puts("  tcp: sent 16 bytes\n");
        return;
    }

    if (tcp_state == T_ESTABLISHED) {
        if (dlen > 0 && seq == rcv_nxt) {
            rcv_nxt += (uint32)dlen;
            net_putn("  tcp: received ", (unsigned long)dlen, " bytes: \"");
            char buf[128];
            int n = dlen < 120 ? dlen : 120;
            umemcpy(buf, t + hlen, (unsigned long)n);
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
                n--;
            buf[n] = 0;
            net_puts(buf);
            net_puts("\"\n");
            tcp_send(TCP_ACK, 0, 0);
        }
        if (flags & TCP_FIN) {
            rcv_nxt++;
            tcp_send(TCP_ACK | TCP_FIN, 0, 0);
            tcp_state = T_FIN_SENT;
            net_puts("  tcp: peer closed; sent FIN\n");
        } else if (sent_payload && dlen > 0) {
            tcp_send(TCP_ACK | TCP_FIN, 0, 0);
            tcp_state = T_FIN_SENT;
            net_puts("  tcp: closing\n");
        }
        return;
    }

    if (tcp_state == T_FIN_SENT && (flags & TCP_ACK)) {
        tcp_state = T_DONE;
        net_puts("  tcp: closed\n");
    }
}

/* ---- dispatch --------------------------------------------------------- */

void net_start(void)
{
    arp_request();
    net_puts("  arp: who has 10.0.2.2?\n");
}

void net_input(uint8 *f, int len)
{
    if (len < 14)
        return;
    unsigned type = get16(f + 12);

    if (type == ETH_ARP && get16(f + 20) == 2 && !have_gw) {
        umemcpy(gw_mac, f + 22, 6);
        have_gw = 1;
        net_puts("  arp: 10.0.2.2 is at ");
        for (int i = 0; i < 6; i++) {
            const char *d = "0123456789abcdef";
            char b[4] = { i ? ':' : ' ', d[gw_mac[i] >> 4], d[gw_mac[i] & 15], 0 };
            net_puts(i ? b : b + 1);
        }
        net_puts("\n");

        udp_send(UDP_PORT, "hello from rvos over udp\n");
        net_puts("  udp: sent a datagram to 10.0.2.2:9999\n");
        tcp_open();
        return;
    }

    if (type != ETH_IPV4 || len < 34)
        return;
    uint8 *ip  = f + 14;
    int    ihl = (ip[0] & 0x0f) * 4;
    int    tot = (int)get16(ip + 2);

    if (ip[9] == IP_TCP)
        tcp_input(ip + ihl, tot - ihl);
    else if (ip[9] == IP_ICMP && ip[ihl] == 0)
        net_puts("  icmp: echo reply\n");
}
