/* net_ip.c — ARP, IPv4, ICMP, UDP, and a TCP that both calls and answers.

   Up to stage 19 "the TCP connection" was a set of globals: one peer, one pair
   of ports, one sequence number. That is enough to *make* a call and nothing
   else. A stack that can be called has to keep several conversations apart, so
   the globals became a table — a control block per connection, found by the
   four numbers that identify it — and the ad-hoc if-ladder became the state
   machine RFC 793 describes.

   Answering also means answering the protocols underneath. A host that wants
   to reach us asks who has our address, and if nothing replies the connection
   never gets as far as TCP; so ARP is a cache with both halves now, and ICMP
   echo is answered rather than merely recognised.

   Scope, stated plainly. UDP is complete. TCP does: active and passive open,
   the full close sequence including TIME-WAIT, demultiplexing to a connection
   table, receive-side flow control from the space actually left in the read
   queue, an initial sequence number taken from the clock, and retransmission
   of a lost segment. What is still missing: out-of-order reassembly (a
   segment arriving early is dropped and re-requested with a duplicate ack),
   more than one segment in flight, and congestion control. */
#include "netif.h"
#include "ulib.h"
#include "syscall.h"
#include "vfs.h"

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

static const uint8 me_ip[4]   = { 10, 0, 2, 15 };
static const uint8 me_mask[4] = { 255, 255, 255, 0 };
static const uint8 gw_ip[4]   = { 10, 0, 2, 2 };

#define UDP_PORT    9999        /* the host peer our demo datagram goes to */
#define TCP_PORT    9998        /* ...and the host peer it connects to */
#define LISTEN_PORT 7           /* ...and the port we answer on: echo */

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

static int ip_eq(const uint8 *a, const uint8 *b) { return umemcmp(a, b, 4) == 0; }

/* Anything inside our own /24 is reached directly; everything else goes to
   the gateway. That test is the whole of routing here, and it is the reason a
   host address and a netmask are two different things. */
static int on_link(const uint8 *ip)
{
    for (int i = 0; i < 4; i++)
        if ((ip[i] ^ me_ip[i]) & me_mask[i])
            return 0;
    return 1;
}

/* ---- the ARP cache ----------------------------------------------------
   Stage 19 remembered exactly one address — the gateway's — because it only
   ever spoke to the gateway. A stack that answers has to remember whoever
   asked, so this is a table, and it is filled from both directions: replies
   to our requests, and requests addressed to us. The second is not an
   optimisation; a host that ARPs for us is about to send us a packet, and we
   will need its address to answer. */

#define NARP 4
struct arpent {
    uint8 ip[4];
    uint8 mac[6];
    int   used;
};
static struct arpent arp_cache[NARP];

static const uint8 *arp_lookup(const uint8 *ip)
{
    for (int i = 0; i < NARP; i++)
        if (arp_cache[i].used && ip_eq(arp_cache[i].ip, ip))
            return arp_cache[i].mac;
    return 0;
}

static void mac_puts(const uint8 *m)
{
    const char *d = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        char b[4] = { i ? ':' : ' ', d[m[i] >> 4], d[m[i] & 15], 0 };
        net_puts(i ? b : b + 1);
    }
}
static void ip_puts(const uint8 *a)
{
    char b[20];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (i) b[n++] = '.';
        n += uutoa(a[i], b + n);
    }
    b[n] = 0;
    net_puts(b);
}

static void arp_learn(const uint8 *ip, const uint8 *mac)
{
    for (int i = 0; i < NARP; i++)
        if (arp_cache[i].used && ip_eq(arp_cache[i].ip, ip)) {
            umemcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    for (int i = 0; i < NARP; i++)
        if (!arp_cache[i].used) {
            umemcpy(arp_cache[i].ip, ip, 4);
            umemcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].used = 1;
            net_puts("  arp: ");
            ip_puts(ip);
            net_puts(" is at ");
            mac_puts(mac);
            net_puts("\n");
            return;
        }
    /* Full. A real cache would evict the oldest; four entries and a link that
       never changes make that a complication with nothing to show for it. */
}

/* ---- frame construction ---------------------------------------------- */

static uint8 out[1600];
static unsigned ip_id = 1;

static int eth_hdr(uint8 *p, const uint8 *dst_mac, unsigned type)
{
    umemcpy(p, dst_mac, 6);
    umemcpy(p + 6, net_mac, 6);
    put16(p + 12, type);
    return 14;
}

static void arp_request(const uint8 *target)
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
    umemcpy(a + 24, target, 4);
    net_transmit(out, o + 28);
    net_puts("  arp: who has ");
    ip_puts(target);
    net_puts("?\n");
}

static void arp_reply(const uint8 *tip, const uint8 *tmac)
{
    int o = eth_hdr(out, tmac, ETH_ARP);
    uint8 *a = out + o;
    put16(a, 1); put16(a + 2, ETH_IPV4);
    a[4] = 6; a[5] = 4;
    put16(a + 6, 2);                            /* reply */
    umemcpy(a + 8, net_mac, 6);
    umemcpy(a + 14, me_ip, 4);
    umemcpy(a + 18, tmac, 6);
    umemcpy(a + 24, tip, 4);
    net_transmit(out, o + 28);
}

/* Start an IPv4 packet to `dst`. Returns the offset of the payload, or -1 if
   the next hop's hardware address is not known yet — in which case a request
   goes out and the caller's packet is dropped. Dropping is the right answer:
   every protocol above this line either retransmits or does not care. */
static int ip_begin(uint8 *p, const uint8 *dst, int proto, int payload_len)
{
    const uint8 *hop = on_link(dst) ? dst : gw_ip;
    const uint8 *mac = arp_lookup(hop);
    if (!mac) {
        arp_request(hop);
        return -1;
    }
    int o = eth_hdr(p, mac, ETH_IPV4);
    uint8 *ip = p + o;
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, (unsigned)(20 + payload_len));
    put16(ip + 4, ip_id++);
    put16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = (uint8)proto;
    put16(ip + 10, 0);
    umemcpy(ip + 12, me_ip, 4);
    umemcpy(ip + 16, dst, 4);
    put16(ip + 10, fold(sum16(ip, 20, 0)));    /* header only, by definition */
    return o + 20;
}

/* ---- ICMP -------------------------------------------------------------
   An echo request is turned around and sent back with the type changed and
   the checksum recomputed. There is no pseudo-header here: ICMP checksums
   cover the ICMP message and nothing else. */

static void icmp_input(const uint8 *ip, int ihl, int tot)
{
    const uint8 *ic = ip + ihl;
    int len = tot - ihl;
    if (len < 8)
        return;
    if (ic[0] == 0) {
        net_puts("  icmp: echo reply\n");
        return;
    }
    if (ic[0] != 8)
        return;

    int o = ip_begin(out, ip + 12, IP_ICMP, len);
    if (o < 0)
        return;
    umemcpy(out + o, ic, (unsigned long)len);
    out[o] = 0;                                /* echo request -> echo reply */
    put16(out + o + 2, 0);
    put16(out + o + 2, fold(sum16(out + o, len, 0)));
    net_transmit(out, o + len);
    net_puts("  icmp: echo request from ");
    ip_puts(ip + 12);
    net_puts(", answered\n");
}

/* ---- UDP -------------------------------------------------------------- */

static int udp_send(const uint8 *dst, unsigned sport, unsigned dport,
                    const void *data, int len)
{
    int o = ip_begin(out, dst, IP_UDP, 8 + len);
    if (o < 0)
        return -1;
    uint8 *u = out + o;
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, (unsigned)(8 + len));
    put16(u + 6, 0);
    umemcpy(u + 8, data, (unsigned long)len);
    put16(u + 6, l4_checksum(IP_UDP, me_ip, dst, u, 8 + len));
    net_transmit(out, o + 8 + len);
    return len;
}

/* ---- DNS ---------------------------------------------------------------
   A name is not an address, and every address this system has used so far was
   written into it by hand. DNS is the one protocol that turns the first into
   the second, and it is a good fit for what already exists: a single UDP
   datagram out, a single one back, no connection and no reliability beyond
   asking again.

   The wire format has one trick worth knowing. A name is a chain of
   length-prefixed labels ending in a zero byte, but a label length whose top
   two bits are set is not a length at all — it is a pointer to a name earlier
   in the same message, which is how a reply repeats the question without
   repeating the bytes. Walking a name means being ready for both. */

static const uint8 dns_ip[4] = { 10, 0, 2, 3 };   /* QEMU's resolver */
#define DNS_PORT       53
#define DNS_MY_PORT 30053    /* clear of the ephemeral range TCP draws from */

static unsigned dns_id;

/* "example.com" -> 7 'example' 3 'com' 0 */
static int dns_encode(uint8 *o, const char *name)
{
    int n = 0;
    while (*name) {
        int len = 0;
        while (name[len] && name[len] != '.')
            len++;
        o[n++] = (uint8)len;
        umemcpy(o + n, name, (unsigned long)len);
        n += len;
        name += len;
        if (*name == '.')
            name++;
    }
    o[n++] = 0;
    return n;
}

static void dns_query(const char *name)
{
    uint8 q[300];
    dns_id = (unsigned)(unow_ticks() & 0xffff);
    put16(q, dns_id);
    put16(q + 2, 0x0100);          /* standard query, recursion desired */
    put16(q + 4, 1);               /* one question */
    put16(q + 6, 0);
    put16(q + 8, 0);
    put16(q + 10, 0);
    int n = 12 + dns_encode(q + 12, name);
    put16(q + n, 1);  n += 2;      /* type A */
    put16(q + n, 1);  n += 2;      /* class IN */
    if (udp_send(dns_ip, DNS_MY_PORT, DNS_PORT, q, n) < 0)
        return;
    net_puts("  dns: who is ");
    net_puts(name);
    net_puts("?\n");
}

/* Step over one name, whether it is spelled out or is a pointer. */
static const uint8 *dns_skip(const uint8 *p, const uint8 *end)
{
    while (p < end) {
        if ((*p & 0xc0) == 0xc0)
            return p + 2 <= end ? p + 2 : end;
        if (*p == 0)
            return p + 1;
        p += 1 + *p;
    }
    return end;
}

static void web_fetch(const uint8 *addr);        /* the demo, below */

static void dns_input(const uint8 *m, int len)
{
    if (len < 12 || get16(m) != dns_id)
        return;
    const uint8 *end = m + len;
    int qd = (int)get16(m + 4), an = (int)get16(m + 6);
    if ((get16(m + 2) & 0xf) != 0) {
        net_puts("  dns: the resolver returned an error\n");
        return;
    }

    const uint8 *p = m + 12;
    for (int i = 0; i < qd && p < end; i++)
        p = dns_skip(p, end) + 4;                /* name, then type and class */

    for (int i = 0; i < an && p < end; i++) {
        p = dns_skip(p, end);
        if (p + 10 > end)
            return;
        unsigned type = get16(p);
        int rdlen = (int)get16(p + 8);
        p += 10;
        if (p + rdlen > end)
            return;
        if (type == 1 && rdlen == 4) {           /* an address at last */
            net_puts("  dns: it is at ");
            ip_puts(p);
            net_puts("\n");
            web_fetch(p);
            return;
        }
        p += rdlen;                              /* a CNAME, most likely */
    }
    net_puts("  dns: no address in the answer\n");
}

static void udp_input(const uint8 *ip, const uint8 *u, int len)
{
    if (len < 8)
        return;
    if (get16(u + 2) == DNS_MY_PORT) {
        dns_input(u + 8, len - 8);
        return;
    }
    net_puts("  udp: ");
    net_putn("", (unsigned long)(len - 8), " bytes from ");
    ip_puts(ip + 12);
    net_putn(":", (unsigned long)get16(u), "\n");
}

/* ---- TCP: the connection table ----------------------------------------
   One control block per conversation. `state` is RFC 793's, and every
   transition below names the one it is taking, because a state machine
   written as a pile of conditions is a state machine nobody can check. */

enum {
    T_FREE, T_LISTEN, T_SYN_SENT, T_SYN_RCVD, T_ESTABLISHED,
    T_FIN_WAIT_1, T_FIN_WAIT_2, T_CLOSING, T_TIME_WAIT,
    T_CLOSE_WAIT, T_LAST_ACK
};

#define NTCB 4
#define RXQ  512
#define MSS  512          /* also VFS_DATA_MAX: a write becomes one segment */

/* Two milliseconds' worth of names for things measured in milliseconds. */
#define RTO_INITIAL 300
#define RTO_TRIES   5
#define TIME_WAIT_MS 2000 /* 2*MSL, with MSL taken as a second rather than the
                             two minutes a real link needs: this is a virtual
                             wire and the slot is wanted back. */

struct tcb {
    int      state;
    uint8    raddr[4];
    unsigned lport, rport;

    uint32   iss, irs;
    uint32   snd_una, snd_nxt, rcv_nxt;
    unsigned snd_wnd;             /* what the peer last advertised */

    /* One outstanding segment, held whole until it is acknowledged. A
       retransmission has to be the same bytes with the same sequence number,
       so the frame is kept rather than the intent that built it. */
    uint8    rt_frame[1600];
    int      rt_len;
    uint32   rt_seq_end;
    int      rt_tries;
    int      rt_rto;
    uint64   rt_at;               /* absolute ms; 0 = no timer */
    unsigned rt_pending;          /* a control segment we could not address */

    uint64   tw_at;               /* TIME-WAIT expiry, absolute ms */

    char     rxq[RXQ];
    int      rxq_len;

    int      fds;                 /* how many programs hold this open */
    int      is_client;           /* the one /net/tcp names */
    int      http;                /* the demo fetch: ask on open, print what
                                     comes back, and keep the queue drained so
                                     the window never shuts */
};

static struct tcb tcbs[NTCB];
static unsigned next_port = 40001;

static struct tcb *tcb_alloc(void)
{
    for (int i = 0; i < NTCB; i++)
        if (tcbs[i].state == T_FREE) {
            umemset(&tcbs[i], 0, sizeof(tcbs[i]));
            return &tcbs[i];
        }
    return 0;
}

static void tcb_free(struct tcb *c) { umemset(c, 0, sizeof(*c)); }

/* Demultiplexing: the exact four-tuple wins; a listener on the local port is
   the fallback. Getting that order wrong sends a listener the segments of an
   established connection, which looks like a peer that has gone mad. */
static struct tcb *tcb_find(const uint8 *raddr, unsigned rport, unsigned lport)
{
    struct tcb *listener = 0;
    for (int i = 0; i < NTCB; i++) {
        struct tcb *c = &tcbs[i];
        if (c->state == T_FREE || c->lport != lport)
            continue;
        if (c->state == T_LISTEN)
            listener = c;
        else if (c->rport == rport && ip_eq(c->raddr, raddr))
            return c;
    }
    return listener;
}

/* RFC 793 wants the initial sequence number to come from a clock ticking
   roughly every 4 microseconds, so that a segment from an old incarnation of
   the same connection cannot be mistaken for a current one. QEMU's time base
   is 10 MHz, so 40 ticks is that interval. */
static uint32 gen_iss(void) { return (uint32)(unow_ticks() / 40); }

/* ---- TCP: sending ------------------------------------------------------ */

static unsigned rcv_window(const struct tcb *c)
{
    int free_space = RXQ - c->rxq_len;
    return (unsigned)(free_space > 0 ? free_space : 0);
}

/* Build a segment into `out`; returns its total frame length, or -1 if the
   peer's hardware address is not known yet. */
static int tcp_build(const uint8 *dip, unsigned sport, unsigned dport,
                     uint32 seq, uint32 ack, unsigned flags, unsigned wnd,
                     const char *data, int dlen)
{
    int o = ip_begin(out, dip, IP_TCP, 20 + dlen);
    if (o < 0)
        return -1;
    uint8 *t = out + o;
    put16(t, sport);
    put16(t + 2, dport);
    put32(t + 4, seq);
    put32(t + 8, ack);
    t[12] = 5 << 4;                             /* 20-byte header, no options */
    t[13] = (uint8)flags;
    put16(t + 14, wnd);
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (dlen)
        umemcpy(t + 20, data, (unsigned long)dlen);
    put16(t + 16, l4_checksum(IP_TCP, me_ip, dip, t, 20 + dlen));
    return o + 20 + dlen;
}

/* A test hook. On a virtual link nothing is ever lost, so the retransmit path
   would never run and its correctness would be a matter of opinion. This drops
   exactly one sequence-consuming segment on the floor after it is built. */
static int drop_next = 1;

static void timers_rearm(void);

static void seg_send(struct tcb *c, unsigned flags, const char *data, int dlen)
{
    int flen = tcp_build(c->raddr, c->lport, c->rport, c->snd_nxt, c->rcv_nxt,
                         flags, rcv_window(c), data, dlen);
    if (flen < 0) {
        /* The peer's hardware address is not known yet — a request has just
           gone out for it. A control segment is remembered and tried again
           when the answer has had time to arrive; a segment carrying data is
           simply not sent, and the caller is told nothing moved. */
        if (!dlen) {
            c->rt_pending = flags;
            c->rt_at = unow_ms() + RTO_INITIAL;
            timers_rearm();
        }
        return;
    }
    c->rt_pending = 0;

    /* SYN and FIN each consume a sequence number, which is why a handshake
       advances the stream without carrying any data. Anything that consumes
       sequence space is something the peer must acknowledge, and therefore
       something we must be prepared to send again. */
    uint32 consumed = (uint32)dlen + ((flags & (TCP_SYN | TCP_FIN)) ? 1 : 0);

    if (consumed) {
        umemcpy(c->rt_frame, out, (unsigned long)flen);
        c->rt_len     = flen;
        c->rt_seq_end = c->snd_nxt + consumed;
        c->rt_tries   = 0;
        c->rt_rto     = RTO_INITIAL;
        c->rt_at      = unow_ms() + (uint64)c->rt_rto;
        timers_rearm();
    }

    if (drop_next && consumed) {
        drop_next = 0;
        net_puts("  tcp: [test] dropping this segment before it reaches the card\n");
    } else {
        net_transmit(out, flen);
    }

    c->snd_nxt += consumed;
}

/* A segment for a connection that does not exist. RFC 793 is specific about
   what a reset carries: if the offending segment had an ACK, the reset takes
   its sequence number from that acknowledgement; otherwise it acknowledges
   everything the segment occupied and starts at zero. Without this a host
   that connects to a closed port waits for its SYN to time out instead of
   being told at once. */
static void tcp_reset(const uint8 *dip, unsigned sport, unsigned dport,
                      unsigned flags, uint32 seq, uint32 ack, int dlen)
{
    uint32 rseq, rack;
    unsigned rflags;
    if (flags & TCP_ACK) {
        rseq = ack; rack = 0; rflags = TCP_RST;
    } else {
        rseq = 0;
        rack = seq + (uint32)dlen + ((flags & (TCP_SYN | TCP_FIN)) ? 1 : 0);
        rflags = TCP_RST | TCP_ACK;
    }
    int flen = tcp_build(dip, dport, sport, rseq, rack, rflags, 0, 0, 0);
    if (flen > 0)
        net_transmit(out, flen);
}

static void tcp_acked(struct tcb *c, uint32 ack)
{
    if (c->rt_len && (int32)(ack - c->rt_seq_end) >= 0) {
        c->rt_len = 0;
        c->rt_at  = 0;
        timers_rearm();
    }
}

/* Everything we sent has been acknowledged — i.e. our FIN, if we sent one. */
static int fin_acked(const struct tcb *c) { return c->snd_una == c->snd_nxt; }

static void enter_time_wait(struct tcb *c)
{
    c->state = T_TIME_WAIT;
    c->rt_len = 0;
    c->rt_at  = 0;
    c->tw_at  = unow_ms() + TIME_WAIT_MS;
    timers_rearm();
}

/* ---- TCP: opening and closing ------------------------------------------ */

static struct tcb *tcp_listen(unsigned port)
{
    struct tcb *c = tcb_alloc();
    if (!c)
        return 0;
    c->state = T_LISTEN;
    c->lport = port;
    net_putn("  tcp: listening on port ", (unsigned long)port, "\n");
    return c;
}

static struct tcb *tcp_connect(const uint8 *ip, unsigned port)
{
    struct tcb *c = tcb_alloc();
    if (!c)
        return 0;
    umemcpy(c->raddr, ip, 4);
    c->lport   = next_port++;
    c->rport   = port;
    c->iss     = gen_iss();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss;
    c->rcv_nxt = 0;
    c->state   = T_SYN_SENT;
    seg_send(c, TCP_SYN, 0, 0);
    net_puts("  tcp: SYN -> ");
    ip_puts(ip);
    net_putn(":", (unsigned long)port, "\n");
    return c;
}

/* Our side has nothing more to send. Which close this is depends on whether
   the peer has already said the same: an active close leads to FIN-WAIT, a
   close after theirs leads to LAST-ACK. */
static void tcp_close(struct tcb *c)
{
    if (c->state == T_ESTABLISHED) {
        seg_send(c, TCP_ACK | TCP_FIN, 0, 0);
        c->state = T_FIN_WAIT_1;
        net_puts("  tcp: closing (active)\n");
    } else if (c->state == T_CLOSE_WAIT) {
        seg_send(c, TCP_ACK | TCP_FIN, 0, 0);
        c->state = T_LAST_ACK;
        net_puts("  tcp: closing (peer went first)\n");
    } else if (c->state == T_LISTEN || c->state == T_SYN_SENT) {
        tcb_free(c);
    }
}

/* ---- TCP: receiving ---------------------------------------------------- */

static void demo_opened(struct tcb *c);          /* the demo, at the bottom */
static void demo_received(struct tcb *c, const uint8 *data, int dlen);

static void tcp_queue(struct tcb *c, const uint8 *data, int dlen)
{
    int room = RXQ - c->rxq_len;
    int n = dlen < room ? dlen : room;
    umemcpy(c->rxq + c->rxq_len, data, (unsigned long)n);
    c->rxq_len += n;
}

static void tcp_input(const uint8 *sip, const uint8 *t, int seglen)
{
    unsigned flags = t[13];
    int      hlen  = (t[12] >> 4) * 4;
    if (hlen < 20 || hlen > seglen)
        return;
    int      dlen  = seglen - hlen;
    uint32   seq   = get32(t + 4);
    uint32   ack   = get32(t + 8);
    unsigned sport = get16(t);
    unsigned dport = get16(t + 2);

    /* Check the checksum before believing any of the above. A stack that
       trusts a corrupt segment corrupts a stream, and the pseudo-header is
       what makes a segment delivered to the wrong host fail here. */
    if (l4_checksum(IP_TCP, sip, me_ip, t, seglen) != 0) {
        net_puts("  tcp: bad checksum, dropped\n");
        return;
    }

    struct tcb *c = tcb_find(sip, sport, dport);
    if (!c) {
        if (!(flags & TCP_RST))
            tcp_reset(sip, sport, dport, flags, seq, ack, dlen);
        return;
    }

    /* --- passive open: LISTEN + SYN --------------------------------------
       The listener stays listening. The connection gets a control block of
       its own, which is the difference between a program that can be called
       once and a server. */
    if (c->state == T_LISTEN) {
        if (flags & TCP_RST)
            return;
        if (!(flags & TCP_SYN)) {
            tcp_reset(sip, sport, dport, flags, seq, ack, dlen);
            return;
        }
        struct tcb *n = tcb_alloc();
        if (!n)
            return;              /* backlog full: drop, and they will retry */
        umemcpy(n->raddr, sip, 4);
        n->lport   = dport;
        n->rport   = sport;
        n->irs     = seq;
        n->rcv_nxt = seq + 1;                   /* their SYN counts as one */
        n->iss     = gen_iss();
        n->snd_una = n->iss;
        n->snd_nxt = n->iss;
        n->snd_wnd = get16(t + 14);
        n->state   = T_SYN_RCVD;
        seg_send(n, TCP_SYN | TCP_ACK, 0, 0);
        net_puts("  tcp: SYN from ");
        ip_puts(sip);
        net_putn(":", (unsigned long)sport, ", accepted; SYN-ACK sent\n");
        return;
    }

    if (flags & TCP_RST) {
        net_puts("  tcp: reset by peer\n");
        tcb_free(c);
        timers_rearm();
        return;
    }

    /* --- active open: SYN-SENT ------------------------------------------ */
    if (c->state == T_SYN_SENT) {
        if (!(flags & TCP_SYN))
            return;
        c->irs     = seq;
        c->rcv_nxt = seq + 1;
        c->snd_wnd = get16(t + 14);
        if (flags & TCP_ACK) {
            if ((int32)(ack - c->iss) <= 0 || (int32)(ack - c->snd_nxt) > 0) {
                tcp_reset(sip, sport, dport, flags, seq, ack, dlen);
                return;
            }
            c->snd_una = ack;
            tcp_acked(c, ack);
            c->state = T_ESTABLISHED;
            seg_send(c, TCP_ACK, 0, 0);
            net_puts("  tcp: connection established\n");
            demo_opened(c);
        } else {
            /* Both sides called at once. Rare, and the only reason SYN-RCVD
               is reachable from here. */
            c->state = T_SYN_RCVD;
            c->snd_nxt = c->iss;                /* resend the SYN with an ack */
            seg_send(c, TCP_SYN | TCP_ACK, 0, 0);
        }
        return;
    }

    /* --- everything from SYN-RCVD onwards -------------------------------- */

    if (flags & TCP_ACK) {
        if ((int32)(ack - c->snd_una) > 0 && (int32)(ack - c->snd_nxt) <= 0)
            c->snd_una = ack;
        c->snd_wnd = get16(t + 14);
        tcp_acked(c, ack);
    }

    switch (c->state) {
    case T_SYN_RCVD:
        if (!(flags & TCP_ACK))
            return;
        c->state = T_ESTABLISHED;
        net_puts("  tcp: connection established (inbound)\n");
        /* The demo's one piece of policy, and it is policy: a stack has no
           business deciding what to say to a caller. It is here because
           nothing else is listening yet — the program that will read
           /net/tcp/N and answer for itself is the next stage's work — and
           without it a call into rvos would show only one direction. */
        {
            static const char banner[] =
                "rvos: you have reached the guest on port 7\n";
            seg_send(c, TCP_ACK | TCP_PSH, banner, (int)sizeof(banner) - 1);
        }
        break;
    case T_FIN_WAIT_1:
        if (fin_acked(c))
            c->state = T_FIN_WAIT_2;            /* our FIN was taken */
        break;
    case T_CLOSING:
        if (fin_acked(c)) {
            enter_time_wait(c);
            net_puts("  tcp: closed (simultaneous)\n");
            return;
        }
        break;
    case T_LAST_ACK:
        if (fin_acked(c)) {
            net_puts("  tcp: closed\n");
            tcb_free(c);
            timers_rearm();
            return;
        }
        break;
    case T_TIME_WAIT:
        /* A retransmitted FIN: acknowledge it again and keep waiting. */
        if (flags & TCP_FIN) {
            int flen = tcp_build(c->raddr, c->lport, c->rport, c->snd_nxt,
                                 c->rcv_nxt, TCP_ACK, 0, 0, 0);
            if (flen > 0)
                net_transmit(out, flen);
        }
        return;
    default:
        break;
    }

    /* --- data ------------------------------------------------------------
       In order only. A segment that arrives early is dropped and answered
       with a duplicate acknowledgement, which asks for the gap to be filled;
       holding it aside until the gap closes is reassembly, and that is the
       next thing this stack needs. */
    if (dlen > 0) {
        if (seq == c->rcv_nxt) {
            c->rcv_nxt += (uint32)dlen;
            if (c->http) {
                /* The demo reads as fast as the bytes arrive, so the window
                   never shuts and the transfer runs to the end. */
                demo_received(c, t + hlen, dlen);
            } else {
                tcp_queue(c, t + hlen, dlen);
                net_putn("  tcp: received ", (unsigned long)dlen,
                         " bytes, queued for readers\n");
            }
            seg_send(c, TCP_ACK, 0, 0);
        } else {
            net_puts("  tcp: out-of-order segment dropped, re-acking\n");
            seg_send(c, TCP_ACK, 0, 0);
            return;
        }
    }

    /* --- FIN, but only the one that fits where the stream has got to ----- */
    if ((flags & TCP_FIN) && seq + (uint32)dlen == c->rcv_nxt) {
        c->rcv_nxt++;
        switch (c->state) {
        case T_ESTABLISHED:
            c->state = T_CLOSE_WAIT;
            seg_send(c, TCP_ACK, 0, 0);
            net_puts("  tcp: peer closed its half\n");
            /* Nobody is holding it open, so there is nothing left to say. */
            if (c->fds == 0)
                tcp_close(c);
            break;
        case T_FIN_WAIT_1:
            seg_send(c, TCP_ACK, 0, 0);
            if (fin_acked(c)) {
                enter_time_wait(c);
                net_puts("  tcp: closed\n");
            } else {
                c->state = T_CLOSING;
            }
            break;
        case T_FIN_WAIT_2:
            seg_send(c, TCP_ACK, 0, 0);
            enter_time_wait(c);
            net_puts("  tcp: closed\n");
            break;
        default:
            break;
        }
    }
}

/* ---- timers ------------------------------------------------------------
   One alarm, several deadlines. The kernel wakes a task at one time; the task
   works out which of its own deadlines that was. This is why user mode needed
   a clock as well as an alarm. */

static void timers_rearm(void)
{
    uint64 now = unow_ms(), best = 0;
    for (int i = 0; i < NTCB; i++) {
        struct tcb *c = &tcbs[i];
        if (c->state == T_FREE)
            continue;
        if (c->rt_at && (!best || c->rt_at < best))
            best = c->rt_at;
        if (c->tw_at && (!best || c->tw_at < best))
            best = c->tw_at;
    }
    if (!best) {
        sys_alarm(0);
        return;
    }
    sys_alarm(best > now ? (int)(best - now) : 1);
}

void net_timeout(void)
{
    uint64 now = unow_ms();

    for (int i = 0; i < NTCB; i++) {
        struct tcb *c = &tcbs[i];
        if (c->state == T_FREE)
            continue;

        if (c->tw_at && c->tw_at <= now) {
            net_puts("  tcp: time-wait expired, connection forgotten\n");
            tcb_free(c);
            continue;
        }
        if (!c->rt_at || c->rt_at > now)
            continue;

        if (c->rt_pending) {
            unsigned f = c->rt_pending;
            c->rt_pending = 0;
            c->rt_at = 0;
            if (++c->rt_tries > RTO_TRIES) {
                net_puts("  tcp: no route to the peer, giving up\n");
                tcb_free(c);
                continue;
            }
            seg_send(c, f, 0, 0);
            continue;
        }

        if (++c->rt_tries > RTO_TRIES) {
            net_puts("  tcp: giving up after 5 retransmissions\n");
            tcb_free(c);
            continue;
        }
        net_putn("  tcp: timeout, retransmitting (attempt ",
                 (unsigned long)c->rt_tries, ")\n");
        net_transmit(c->rt_frame, c->rt_len);
        c->rt_rto *= 2;                         /* exponential backoff */
        c->rt_at = now + (uint64)c->rt_rto;
    }
    timers_rearm();
}

/* ---- the network as files ---------------------------------------------
     /net/status   read: the interface, the ARP cache and the whole table
     /net/tcp      the connection this system opened
     /net/tcp/N    connection N, by slot — including the ones it answered

   Closing the last descriptor on a connection closes the connection, which is
   what a file interface means by close and happens to be what TCP means by
   it too. */

static const char *state_name(int s)
{
    switch (s) {
    case T_LISTEN:      return "listen";
    case T_SYN_SENT:    return "syn-sent";
    case T_SYN_RCVD:    return "syn-rcvd";
    case T_ESTABLISHED: return "established";
    case T_FIN_WAIT_1:  return "fin-wait-1";
    case T_FIN_WAIT_2:  return "fin-wait-2";
    case T_CLOSING:     return "closing";
    case T_TIME_WAIT:   return "time-wait";
    case T_CLOSE_WAIT:  return "close-wait";
    case T_LAST_ACK:    return "last-ack";
    default:            return "free";
    }
}

static int app(char *o, int n, const char *s)
{
    int l = ustrlen(s);
    umemcpy(o + n, s, (unsigned long)l);
    return n + l;
}
static int app_ip(char *o, int n, const uint8 *a)
{
    for (int i = 0; i < 4; i++) {
        if (i) o[n++] = '.';
        n += uutoa(a[i], o + n);
    }
    return n;
}
/* Pad to a fixed width from where the field began, so the table lines up
   however long a state name happens to be. */
static int app_field(char *o, int n, const char *s, int width)
{
    int start = n;
    n = app(o, n, s);
    while (n - start < width)
        o[n++] = ' ';
    return n;
}

static int net_status(char *o, int cap)
{
    int n = 0;
    n = app(o, n, "mac      ");
    for (int i = 0; i < 6; i++) {
        const char *d = "0123456789abcdef";
        if (i) o[n++] = ':';
        o[n++] = d[net_mac[i] >> 4];
        o[n++] = d[net_mac[i] & 15];
    }
    n = app(o, n, "\naddress  ");
    n = app_ip(o, n, me_ip);
    n = app(o, n, "/24\ngateway  ");
    n = app_ip(o, n, gw_ip);
    o[n++] = '\n';

    for (int i = 0; i < NARP; i++) {
        if (!arp_cache[i].used)
            continue;
        n = app(o, n, "arp      ");
        n = app_ip(o, n, arp_cache[i].ip);
        n = app(o, n, " -> ");
        for (int k = 0; k < 6; k++) {
            const char *d = "0123456789abcdef";
            if (k) o[n++] = ':';
            o[n++] = d[arp_cache[i].mac[k] >> 4];
            o[n++] = d[arp_cache[i].mac[k] & 15];
        }
        o[n++] = '\n';
    }

    for (int i = 0; i < NTCB; i++) {
        struct tcb *c = &tcbs[i];
        if (c->state == T_FREE || n > cap - 80)
            continue;
        n = app(o, n, "tcp ");
        n += uutoa((unsigned long)i, o + n);
        o[n++] = ' ';
        n = app_field(o, n, state_name(c->state), 13);
        o[n++] = ':';
        n += uutoa((unsigned long)c->lport, o + n);
        if (c->state != T_LISTEN) {
            n = app(o, n, " <-> ");
            n = app_ip(o, n, c->raddr);
            o[n++] = ':';
            n += uutoa((unsigned long)c->rport, o + n);
            n = app(o, n, "  rx ");
            n += uutoa((unsigned long)c->rxq_len, o + n);
        }
        o[n++] = '\n';
    }
    return n;
}

/* Descriptors: a slot per open of the status file, and slot+CONN0 for a
   connection, so the fd a program holds says which conversation it means.

   The status file needs a slot of its own because it needs an *offset*. A
   caller that reads until read() returns nothing — which is what `cat` is —
   never stops if every read hands back the whole text again. A rendered
   report is still a file, and a file ends. */
#define NSTATFD 4
enum { FD_STATUS0 = 1, FD_CONN0 = 16 };

static struct {
    int used;
    int off;
} statfd[NSTATFD];

static struct tcb *conn_of(int fd)
{
    int i = fd - FD_CONN0;
    if (i < 0 || i >= NTCB || tcbs[i].state == T_FREE)
        return 0;
    return &tcbs[i];
}

/* "/net/tcp" with no number means the connection this system placed. */
static int client_slot(void)
{
    for (int i = 0; i < NTCB; i++)
        if (tcbs[i].state != T_FREE && tcbs[i].is_client)
            return i;
    return -1;
}

static int path_slot(const char *p)
{
    /* /net/tcp/N */
    const char *s = p + ustrlen("/net/tcp/");
    if (*s < '0' || *s > '9')
        return -1;
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v < NTCB ? v : -1;
}

void net_vfs(struct vfs_req *r)
{
    switch (r->op) {
    case VFS_OPEN:
        if (ustr_has_prefix(r->path, "/net/status")) {
            r->result = -1;
            for (int i = 0; i < NSTATFD; i++)
                if (!statfd[i].used) {
                    statfd[i].used = 1;
                    statfd[i].off  = 0;
                    r->result = FD_STATUS0 + i;
                    break;
                }
        } else if (ustr_has_prefix(r->path, "/net/tcp/")) {
            int i = path_slot(r->path);
            if (i < 0 || tcbs[i].state == T_FREE) {
                r->result = -1;
            } else {
                tcbs[i].fds++;
                r->result = FD_CONN0 + i;
            }
        } else if (ustr_has_prefix(r->path, "/net/tcp")) {
            int i = client_slot();
            if (i < 0) {
                r->result = -1;
            } else {
                tcbs[i].fds++;
                r->result = FD_CONN0 + i;
            }
        } else {
            r->result = -1;
        }
        break;

    case VFS_READ:
        if (r->fd >= FD_STATUS0 && r->fd < FD_STATUS0 + NSTATFD) {
            int i = r->fd - FD_STATUS0;
            if (!statfd[i].used) {
                r->result = -1;
                break;
            }
            /* Rendered afresh each time, then served from the caller's
               offset: the report is of the system as it is now, and it is
               short enough that one read takes all of it. */
            static char page[VFS_DATA_MAX];
            int len = net_status(page, (int)sizeof(page));
            int n = len - statfd[i].off;
            if (n > r->len)
                n = r->len;
            if (n <= 0) {
                r->result = 0;                          /* end of file */
            } else {
                umemcpy(r->data, page + statfd[i].off, (unsigned long)n);
                statfd[i].off += n;
                r->result = n;
            }
        } else {
            struct tcb *c = conn_of(r->fd);
            if (!c) {
                r->result = -1;
                break;
            }
            int was_shut = rcv_window(c) == 0;
            int n = c->rxq_len < r->len ? c->rxq_len : r->len;
            umemcpy(r->data, c->rxq, (unsigned long)n);
            c->rxq_len -= n;
            for (int i = 0; i < c->rxq_len; i++)     /* shift the remainder */
                c->rxq[i] = c->rxq[i + n];
            /* Draining the queue opens the window again, and a peer that has
               been told to stop will not start until it is told so. A stack
               that skips this update deadlocks a connection it throttled. */
            if (was_shut && n > 0 && c->state == T_ESTABLISHED)
                seg_send(c, TCP_ACK, 0, 0);
            r->result = n;
        }
        break;

    case VFS_WRITE: {
        struct tcb *c = conn_of(r->fd);
        if (!c || (c->state != T_ESTABLISHED && c->state != T_CLOSE_WAIT)) {
            r->result = -1;                         /* not connected */
        } else if (c->rt_len) {
            /* One segment may be in flight at a time, so a write while the
               last one is unacknowledged moves nothing. Saying so is honest;
               a send buffer is what removes the restriction. */
            r->result = 0;
        } else {
            int n = r->len < MSS ? r->len : MSS;
            unsigned wnd = c->snd_wnd;
            if ((unsigned)n > wnd)
                n = (int)wnd;                       /* the peer's window */
            if (n <= 0) {
                r->result = 0;
            } else {
                seg_send(c, TCP_ACK | TCP_PSH, r->data, n);
                net_putn("  tcp: sent ", (unsigned long)n,
                         " bytes on behalf of a program\n");
                r->result = n;
            }
        }
        break;
    }

    case VFS_CLOSE: {
        if (r->fd >= FD_STATUS0 && r->fd < FD_STATUS0 + NSTATFD) {
            statfd[r->fd - FD_STATUS0].used = 0;
        } else {
            /* Closing the last descriptor closes the connection. That is what
               a file interface means by close, and it is what TCP means by it
               too — a program that has stopped reading and writing has said
               everything it is going to say. */
            struct tcb *c = conn_of(r->fd);
            if (c && --c->fds <= 0) {
                c->fds = 0;
                tcp_close(c);
            }
        }
        r->result = 0;
        break;
    }

    default:
        r->result = -1;
        break;
    }
}

/* ---- the demo ----------------------------------------------------------
   Everything below this line is policy, not protocol: which host to talk to,
   what to say, and what to do with the answer. It is kept together at the
   bottom so the stack above it is a stack and nothing else.

   The interesting part is the last item. QEMU's user-mode network is a NAT
   onto the machine's real one, so a segment sent to an address outside
   10.0.2.0/24 leaves for the actual internet and the answer comes back from
   an actual server. That is a far harder examiner than a local `nc`: the
   sequence numbers are somebody else's, the segments come in the sizes a real
   stack chooses, and nothing is forgiven. */

#define WEB_HOST "example.com"

static int demo_done, dns_done;
static int http_shown;              /* bytes of the reply printed so far */
#define HTTP_SHOW 640

static void demo_start(void)
{
    if (demo_done)
        return;
    demo_done = 1;
    udp_send(gw_ip, 40000, UDP_PORT, "hello from rvos over udp\n", 25);
    net_puts("  udp: sent a datagram to 10.0.2.2:9999\n");
    struct tcb *c = tcp_connect(gw_ip, TCP_PORT);
    if (c)
        c->is_client = 1;
}

/* An address, at last, that nobody wrote into this source file. */
static void web_fetch(const uint8 *addr)
{
    struct tcb *c = tcp_connect(addr, 80);
    if (c)
        c->http = 1;
}

static void demo_opened(struct tcb *c)
{
    if (!c->http) {
        if (c->is_client)
            net_puts("  tcp: /net/tcp is open for business\n");
        return;
    }
    static const char req[] =
        "GET / HTTP/1.0\r\nHost: " WEB_HOST "\r\nConnection: close\r\n\r\n";
    seg_send(c, TCP_ACK | TCP_PSH, req, (int)sizeof(req) - 1);
    net_puts("  http: GET / -> " WEB_HOST "\n");
}

/* Print what the server said, up to a point — the whole page would bury the
   console, and the object of the exercise is to show that the bytes are real
   and in order, not to render a web site. */
static void demo_received(struct tcb *c, const uint8 *data, int dlen)
{
    (void)c;
    int n = dlen;
    if (http_shown >= HTTP_SHOW) {
        net_putn("  http: +", (unsigned long)dlen, " bytes (not shown)\n");
        return;
    }
    if (http_shown + n > HTTP_SHOW)
        n = HTTP_SHOW - http_shown;

    for (int i = 0; i < n; ) {
        char line[65];
        int k = 0;
        while (i < n && k < 64)
            line[k++] = (char)data[i++];
        line[k] = 0;
        net_puts(line);
    }
    http_shown += dlen;
    if (http_shown >= HTTP_SHOW)
        net_puts("\n  http: ...\n");
}

void net_start(void)
{
    tcp_listen(LISTEN_PORT);
    /* Both hardware addresses are asked for at once. Nothing can be sent to
       either host until its answer arrives, and a UDP query has no
       retransmission to fall back on, so the query waits for the reply
       rather than the other way round. */
    arp_request(gw_ip);
    arp_request(dns_ip);
}

/* An address became known. Whichever it was, something was waiting for it. */
static void demo_arp_ready(const uint8 *ip)
{
    if (ip_eq(ip, gw_ip))
        demo_start();
    if (ip_eq(ip, dns_ip) && !dns_done) {
        dns_done = 1;
        dns_query(WEB_HOST);
    }
}

static void arp_input(const uint8 *a, int len)
{
    if (len < 28)
        return;
    if (get16(a) != 1 || get16(a + 2) != ETH_IPV4)
        return;                                     /* not ethernet/IPv4 */
    unsigned op = get16(a + 6);
    const uint8 *sha = a + 8, *spa = a + 14, *tpa = a + 24;

    if (op == 1 && ip_eq(tpa, me_ip)) {
        /* Somebody is about to talk to us. Remember them before replying:
           the answer they want is the address we are learning right now. */
        arp_learn(spa, sha);
        arp_reply(spa, sha);
        net_puts("  arp: told ");
        ip_puts(spa);
        net_puts(" where we are\n");
        return;
    }
    if (op == 2) {
        arp_learn(spa, sha);
        demo_arp_ready(spa);
    }
}

void net_input(uint8 *f, int len)
{
    if (len < 14)
        return;
    unsigned type = get16(f + 12);

    if (type == ETH_ARP) {
        arp_input(f + 14, len - 14);
        return;
    }
    if (type != ETH_IPV4 || len < 34)
        return;

    uint8 *ip  = f + 14;
    int    ihl = (ip[0] & 0x0f) * 4;
    int    tot = (int)get16(ip + 2);
    if (ihl < 20 || tot < ihl || tot > len - 14)
        return;
    if (fold(sum16(ip, ihl, 0)) != 0)
        return;                                     /* corrupt header */
    if (!ip_eq(ip + 16, me_ip))
        return;                                     /* not addressed to us */

    switch (ip[9]) {
    case IP_TCP:  tcp_input(ip + 12, ip + ihl, tot - ihl); break;
    case IP_UDP:  udp_input(ip, ip + ihl, tot - ihl);      break;
    case IP_ICMP: icmp_input(ip, ihl, tot);                break;
    default: break;
    }
}
