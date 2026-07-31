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
   table, a send buffer with as many segments in flight as the window allows,
   out-of-order reassembly, flow control from the space actually left in the
   read queue, a retransmission timeout measured with Jacobson's estimator
   under Karn's rule, slow start and congestion avoidance, fast retransmit on
   three duplicate acknowledgements, an initial sequence number taken from the
   clock, and the maximum-segment-size option in both directions.

   What it does not do: window scaling, selective acknowledgement, Nagle's
   algorithm or delayed acknowledgements. Its reassembly queue is three
   segments deep and a fourth is dropped. And its congestion control, while
   written, has never been observed working: this link loses nothing that was
   not deliberately dropped. */
#include "netif.h"
#include "malloc.h"
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

/* Not const any more: two of these machines on one wire have to differ,
   and the address is a property of the running system rather than of the
   image. /net/ctl sets it. */
static uint8 me_ip[4]   = { 10, 0, 2, 15 };
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

/* A request that has been received but not answered. See "parking a request"
   below: this is the whole of blocking I/O in this system. */
struct parked {
    int used;
    int task;                     /* who is waiting */
    int fd;
    int len;                      /* how much they asked for */
};


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
#define DNS_TIMEOUT  1000    /* milliseconds before asking again */
#define DNS_TRIES       3

/* One question outstanding at a time, and the program that asked it is parked
   until the answer comes back. UDP has no retransmission of its own, so this
   is where "ask again" lives. */
static unsigned dns_id;
static struct parked dns_waiter;
static char     dns_name[64];
static uint64   dns_at;                 /* absolute ms; 0 = nothing pending */
/* A reply the client was too busy to take has to be offered again, and the
   next packet or timer might be a long way off. Refusing one arms a deadline
   of its own — otherwise an answer can sit in a queue with nobody to nudge
   it. */
static uint64   retry_at;
static int      dns_tries;

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

static void dns_send(const char *name)
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
    udp_send(dns_ip, DNS_MY_PORT, DNS_PORT, q, n);
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

static void ctl_answer(struct parked *p, const char *text, const uint8 *ip);
static void timers_rearm(void);

static void dns_input(const uint8 *m, int len)
{
    if (len < 12 || get16(m) != dns_id || !dns_at)
        return;
    const uint8 *end = m + len;
    int qd = (int)get16(m + 4), an = (int)get16(m + 6);
    if ((get16(m + 2) & 0xf) != 0) {
        net_puts("  dns: the resolver returned an error\n");
        dns_at = 0;
        if (dns_waiter.used)
            ctl_answer(&dns_waiter, "error no such name\n", 0);
        timers_rearm();
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
            dns_at = 0;
            if (dns_waiter.used)
                ctl_answer(&dns_waiter, "ok ", p);
            timers_rearm();
            return;
        }
        p += rdlen;                              /* a CNAME, most likely */
    }
    net_puts("  dns: no address in the answer\n");
    dns_at = 0;
    if (dns_waiter.used)
        ctl_answer(&dns_waiter, "error no address\n", 0);
    timers_rearm();
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

/* Four control blocks, and that number stays fixed on purpose even though
   everything inside them is allocated now. A stack that allocates a block for
   every arriving SYN can be pushed out of memory by a stranger — the oldest
   denial of service there is, and the reason SYN cookies exist. A fixed count
   of blocks bounds what a peer can make this machine spend; allocating what
   is *inside* them means an idle stack costs a few hundred bytes instead of
   thirty-one kilobytes, and a busy one costs exactly what it did before. */
#define NTCB   4
#define RXQ    2048       /* the read queue, and therefore our window */
#define SNDBUF 2048       /* bytes written but not yet acknowledged */
#define MSS    1024       /* the largest segment we send or advertise */
#define NOOO   3          /* segments held aside waiting for a gap to close */
#define NREF   3          /* programs that may hold one connection open */
#define OOOSEG 1200

#define RTO_MIN     200
#define RTO_MAX    8000
#define RTO_INITIAL 300
#define RTO_TRIES     5
#define TIME_WAIT_MS 2000 /* 2*MSL, with MSL taken as a second rather than the
                             two minutes a real link needs: this is a virtual
                             wire and the slot is wanted back. */

/* Sequence numbers wrap. Every comparison has to be made on the *difference*,
   read as signed, or a connection whose numbers cross 2^32 starts rejecting
   its own peer's acknowledgements. */
static int seq_lt(uint32 a, uint32 b) { return (int32)(a - b) <  0; }
static int seq_le(uint32 a, uint32 b) { return (int32)(a - b) <= 0; }
static int seq_gt(uint32 a, uint32 b) { return (int32)(a - b) >  0; }

struct tcb {
    int      state;
    uint8    raddr[4];
    unsigned lport, rport;

    /* --- the send side --------------------------------------------------
       Three sequence numbers and one buffer. snd_una is the oldest byte the
       peer has not acknowledged, snd_nxt the first not yet sent; the bytes
       between them are in flight, and the bytes from snd_nxt to the end of
       the buffer are waiting for room in the window. The SYN occupies the
       number just below snd_base and the FIN the one just above the last
       byte, which is how a handshake and a close take part in the same
       arithmetic as the data. */
    uint32   iss;
    uint32   snd_una, snd_nxt;
    uint32   snd_base;            /* the sequence number of snd_buf[0] */
    uint32   snd_wnd;             /* what the peer last advertised */
    char    *snd_buf;             /* SNDBUF bytes, for as long as this is in use */
    int      snd_len;
    int      snd_fin;             /* a FIN is queued behind those bytes */
    int      fin_sent;
    unsigned peer_mss;

    /* --- congestion and time --------------------------------------------
       cwnd is what the *network* will take, snd_wnd what the peer will take;
       the smaller of the two governs. srtt and rttvar are the smoothed
       round-trip time and its variation, from which the retransmission
       timeout is computed rather than guessed. */
    unsigned cwnd, ssthresh;
    int      rto, srtt, rttvar;   /* milliseconds; srtt < 0 = never measured */
    uint32   rtt_seq;             /* the sequence number being timed */
    uint64   rtt_at;
    int      rtt_timing;
    int      rt_tries;
    uint64   rt_at;               /* absolute ms; 0 = the timer is not running */
    unsigned rt_pending;          /* a control segment we could not address */
    int      dupacks;

    /* --- the receive side ----------------------------------------------- */
    uint32   irs, rcv_nxt;
    char    *rxq;                 /* RXQ bytes, and therefore our window */
    int      rxq_len;
    /* Held aside until the gap in front of them closes. The room for one is
       taken when a segment actually arrives out of order, which on a working
       link is never — so the rarest path in the stack is the one that used to
       account for half of its memory. */
    struct {
        uint32 seq;
        int    len;
        int    used;
        char  *data;
    } ooo[NOOO];

    uint64   tw_at;               /* TIME-WAIT expiry, absolute ms */

    /* On a connection: a read waiting for bytes. On a listener: an accept
       waiting for somebody to call. */
    struct parked reader;
    /* A connect waiting for the handshake to finish. Blocking connect is the
       same idea as blocking read, and a program wants it for the same reason:
       there is nothing useful to do with a connection that is not up yet. */
    struct parked opener;
    int      accepted;            /* handed to a program by accept */

    /* Who holds this connection open. A count would be enough if every
       program closed what it opened; one that exits or faults holding a
       descriptor does not, and the connection would stay open for ever. The
       owner is recorded so the reference can be reclaimed on its behalf. */
    struct { int used; int task; } refs[NREF];

    int      is_client;           /* the one /net/tcp names */
};

static struct tcb tcbs[NTCB];
static unsigned next_port = 40001;

static struct tcb *tcb_alloc(void)
{
    for (int i = 0; i < NTCB; i++)
        if (tcbs[i].state == T_FREE) {
            struct tcb *c = &tcbs[i];
            umemset(c, 0, sizeof(*c));
            c->snd_buf = malloc(SNDBUF);
            c->rxq     = malloc(RXQ);
            if (!c->snd_buf || !c->rxq) {
                /* Out of memory looks like out of connections, which every
                   caller already knows how to be told. */
                free(c->snd_buf);
                free(c->rxq);
                umemset(c, 0, sizeof(*c));
                return 0;
            }
            c->peer_mss = 536;          /* what RFC 1122 says to assume */
            c->rto      = RTO_INITIAL;
            c->srtt     = -1;
            c->cwnd     = 2 * MSS;
            c->ssthresh = 64 * 1024;
            return c;
        }
    return 0;
}

static void tcb_free(struct tcb *c)
{
    free(c->snd_buf);
    free(c->rxq);
    for (int i = 0; i < NOOO; i++)
        free(c->ooo[i].data);
    umemset(c, 0, sizeof(*c));
}

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

/* ---- TCP: building and sending a segment ------------------------------- */

static unsigned rcv_window(const struct tcb *c)
{
    int free_space = RXQ - c->rxq_len;
    return (unsigned)(free_space > 0 ? free_space : 0);
}

/* Build a segment into `out`; returns its total frame length, or -1 if the
   peer's hardware address is not known yet. `mss` non-zero adds the one TCP
   option this stack uses, which is why the header length is a field rather
   than a constant: a SYN carries 24 bytes of header, everything else 20. */
static int tcp_build(const uint8 *dip, unsigned sport, unsigned dport,
                     uint32 seq, uint32 ack, unsigned flags, unsigned wnd,
                     const char *data, int dlen, unsigned mss)
{
    int hlen = mss ? 24 : 20;
    int o = ip_begin(out, dip, IP_TCP, hlen + dlen);
    if (o < 0)
        return -1;
    uint8 *t = out + o;
    put16(t, sport);
    put16(t + 2, dport);
    put32(t + 4, seq);
    put32(t + 8, ack);
    t[12] = (uint8)((hlen / 4) << 4);
    t[13] = (uint8)flags;
    put16(t + 14, wnd);
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (mss) {
        t[20] = 2; t[21] = 4;           /* kind 2, length 4: maximum segment size */
        put16(t + 22, mss);
    }
    if (dlen)
        umemcpy(t + hlen, data, (unsigned long)dlen);
    put16(t + 16, l4_checksum(IP_TCP, me_ip, dip, t, hlen + dlen));
    return o + hlen + dlen;
}

/* A test hook. On a virtual link nothing is ever lost, so the retransmit path
   would never run and its correctness would be a matter of opinion. This drops
   exactly one sequence-consuming segment on the floor after it is built. */
static int drop_next = 1;

static void timers_rearm(void);

/* Put one segment on the wire. Sequence bookkeeping belongs to the caller:
   this function is used both for a first transmission and for a
   retransmission, and the difference between them is entirely in what the
   caller does to snd_nxt afterwards. */
static int seg_xmit(struct tcb *c, unsigned flags, uint32 seq,
                    const char *data, int dlen)
{
    unsigned mss = (flags & TCP_SYN) ? MSS : 0;
    int flen = tcp_build(c->raddr, c->lport, c->rport, seq, c->rcv_nxt,
                         flags, rcv_window(c), data, dlen, mss);
    if (flen < 0) {
        /* The peer's hardware address is not known yet — a request has just
           gone out for it. A control segment is remembered and tried again
           when the answer has had time to arrive; data stays in the buffer,
           where the retransmission timer will find it. */
        if (!dlen) {
            c->rt_pending = flags;
            c->rt_at = unow_ms() + RTO_INITIAL;
            timers_rearm();
        }
        return -1;
    }
    c->rt_pending = 0;

    if (drop_next && (dlen || (flags & (TCP_SYN | TCP_FIN)))) {
        drop_next = 0;
        net_puts("  tcp: [test] dropping this segment before it reaches the card\n");
        return 0;
    }
    net_transmit(out, flen);
    return 0;
}

/* The timer runs while anything is outstanding, and is restarted — not
   merely left running — each time the oldest unacknowledged byte changes. */
static void timer_start(struct tcb *c)
{
    if (!c->rt_at) {
        c->rt_at = unow_ms() + (uint64)c->rto;
        timers_rearm();
    }
}
static void timer_stop(struct tcb *c)
{
    c->rt_at = 0;
    c->rt_tries = 0;
    timers_rearm();
}

/* ---- TCP: output -------------------------------------------------------
   One routine decides what may go: everything between snd_nxt and the end of
   the window, in segments no larger than the peer said it would take. It is
   called after a write, after an acknowledgement opens the window, and after
   a timeout has moved snd_nxt back — the three things that can make sending
   possible. */

static void tcp_output(struct tcb *c)
{
    if (c->state == T_SYN_SENT || c->state == T_SYN_RCVD) {
        if (seq_lt(c->snd_nxt, c->snd_base)) {      /* the SYN is unsent */
            unsigned f = TCP_SYN | (c->state == T_SYN_RCVD ? TCP_ACK : 0);
            if (seg_xmit(c, f, c->iss, 0, 0) == 0) {
                c->snd_nxt = c->snd_base;
                timer_start(c);
            }
        }
        return;                    /* nothing may pass a SYN that is in flight */
    }
    if (c->state == T_FREE || c->state == T_LISTEN || c->state == T_TIME_WAIT)
        return;

    uint32 inflight = c->snd_nxt - c->snd_una;
    uint32 allowed  = c->snd_wnd < c->cwnd ? c->snd_wnd : c->cwnd;
    uint32 usable   = allowed > inflight ? allowed - inflight : 0;

    for (;;) {
        int off   = (int)(c->snd_nxt - c->snd_base);
        int avail = c->snd_len - off;
        if (off < 0 || avail <= 0 || usable == 0)
            break;
        int n = avail;
        if (n > (int)c->peer_mss)
            n = (int)c->peer_mss;
        if ((uint32)n > usable)
            n = (int)usable;
        if (seg_xmit(c, TCP_ACK | TCP_PSH, c->snd_nxt,
                     c->snd_buf + off, n) < 0)
            break;
        /* Karn's algorithm needs one segment being timed at a time, and it
           must not be one that has been sent before. */
        if (!c->rtt_timing) {
            c->rtt_timing = 1;
            c->rtt_seq    = c->snd_nxt + (uint32)n - 1;
            c->rtt_at     = unow_ms();
        }
        c->snd_nxt += (uint32)n;
        usable     -= (uint32)n;
        timer_start(c);
    }

    /* The FIN takes the sequence number after the last byte, and only goes
       once every byte before it has been handed to the card. */
    if (c->snd_fin && !c->fin_sent &&
        c->snd_nxt == c->snd_base + (uint32)c->snd_len) {
        if (seg_xmit(c, TCP_ACK | TCP_FIN, c->snd_nxt, 0, 0) == 0) {
            c->fin_sent = 1;
            c->snd_nxt++;
            timer_start(c);
        }
    }
}

/* An acknowledgement of nothing new, sent to say where we are: after
   accepting data, and after a gap in the stream that we want filled. */
static void send_ack(struct tcb *c)
{
    seg_xmit(c, TCP_ACK, c->snd_nxt, 0, 0);
}

static int tcp_write(struct tcb *c, const char *data, int len)
{
    int room = SNDBUF - c->snd_len;
    int n = len < room ? len : room;
    if (n <= 0)
        return 0;
    umemcpy(c->snd_buf + c->snd_len, data, (unsigned long)n);
    c->snd_len += n;
    tcp_output(c);
    return n;
}

/* ---- TCP: the round-trip time ------------------------------------------
   Jacobson's estimator. The retransmission timeout is the smoothed
   round-trip time plus four times its variation, because what matters is not
   the average delay but how far past the average a sample can reasonably
   fall. A fixed timeout is either too eager on a slow path or too patient on
   a fast one, and it is wrong in a way that gets worse as the path changes. */

static void rtt_update(struct tcb *c, int m)
{
    if (m < 1)
        m = 1;
    if (c->srtt < 0) {
        c->srtt   = m;
        c->rttvar = m / 2;
    } else {
        int err = m - c->srtt;
        if (err < 0)
            err = -err;
        c->rttvar = (3 * c->rttvar + err) / 4;
        c->srtt   = (7 * c->srtt + m) / 8;
    }
    c->rto = c->srtt + 4 * c->rttvar;
    if (c->rto < RTO_MIN) c->rto = RTO_MIN;
    if (c->rto > RTO_MAX) c->rto = RTO_MAX;
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
    c->lport    = next_port++;
    c->rport    = port;
    c->iss      = gen_iss();
    c->snd_una  = c->iss;
    c->snd_nxt  = c->iss;
    c->snd_base = c->iss + 1;
    c->state    = T_SYN_SENT;
    tcp_output(c);
    net_puts("  tcp: SYN -> ");
    ip_puts(ip);
    net_putn(":", (unsigned long)port, "\n");
    return c;
}

/* Our side has nothing more to send. The FIN is queued behind whatever is
   still in the send buffer rather than sent at once — closing does not
   cancel what was written before it. */
static void tcp_close(struct tcb *c)
{
    if (c->state == T_ESTABLISHED) {
        c->snd_fin = 1;
        c->state   = T_FIN_WAIT_1;
        tcp_output(c);
        net_puts("  tcp: closing (active)\n");
    } else if (c->state == T_CLOSE_WAIT) {
        c->snd_fin = 1;
        c->state   = T_LAST_ACK;
        tcp_output(c);
        net_puts("  tcp: closing (peer went first)\n");
    } else if (c->state == T_LISTEN || c->state == T_SYN_SENT) {
        tcb_free(c);
        timers_rearm();
    }
}

/* ---- TCP: reassembly ---------------------------------------------------
   A segment that arrives before the one in front of it is not an error and
   not a loss — it is the ordinary consequence of a network that reorders.
   Dropping it costs a round trip to fetch again something already in hand.
   So a segment ahead of rcv_nxt is held aside, and every time the gap in
   front closes, the held segments that now fit are folded in. */


/* Hand `len` bytes at rcv_nxt to whoever is reading. Returns how many were
   taken: fewer than offered means the read queue is full, and the peer will
   be told so by the window in the next acknowledgement. */
static int rcv_accept(struct tcb *c, const uint8 *data, int len)
{
    int room = RXQ - c->rxq_len;
    int n = len < room ? len : room;
    if (n > 0) {
        umemcpy(c->rxq + c->rxq_len, data, (unsigned long)n);
        c->rxq_len += n;
    }
    return n;
}

static void ooo_drain(struct tcb *c)
{
    for (int progress = 1; progress; ) {
        progress = 0;
        for (int i = 0; i < NOOO; i++) {
            if (!c->ooo[i].used)
                continue;
            uint32 s = c->ooo[i].seq;
            int    l = c->ooo[i].len;
            if (seq_gt(s, c->rcv_nxt))
                continue;                       /* still ahead of the gap */
            if (seq_le(s + (uint32)l, c->rcv_nxt)) {
                c->ooo[i].used = 0;             /* wholly overtaken by events */
                free(c->ooo[i].data);
                c->ooo[i].data = 0;
                progress = 1;
                continue;
            }
            int skip = (int)(c->rcv_nxt - s);
            int n = rcv_accept(c, (const uint8 *)c->ooo[i].data + skip,
                               l - skip);
            c->rcv_nxt += (uint32)n;
            if (n == l - skip) {
                c->ooo[i].used = 0;
                free(c->ooo[i].data);
                c->ooo[i].data = 0;
            }
            net_putn("  tcp: gap closed, ", (unsigned long)n,
                     " held bytes delivered\n");
            progress = 1;
        }
    }
}

static void ooo_store(struct tcb *c, uint32 seq, const uint8 *data, int len)
{
    if (len > OOOSEG)
        len = OOOSEG;
    for (int i = 0; i < NOOO; i++)
        if (c->ooo[i].used && c->ooo[i].seq == seq)
            return;                             /* already held */
    for (int i = 0; i < NOOO; i++)
        if (!c->ooo[i].used) {
            char *room = malloc((unsigned long)len);
            if (!room)
                break;              /* out of memory: drop it, see below */
            c->ooo[i].used = 1;
            c->ooo[i].seq  = seq;
            c->ooo[i].len  = len;
            c->ooo[i].data = room;
            umemcpy(c->ooo[i].data, data, (unsigned long)len);
            net_putn("  tcp: segment ahead of the stream, held aside (",
                     (unsigned long)len, " bytes)\n");
            return;
        }
    /* No room, whether because the queue is full or because the machine is.
       Dropping it is correct — it will be sent again — and is what a real
       stack does when its reassembly queue is full. An allocation that fails
       in TCP is not an error; it is a lost packet, and the protocol was built
       around those from the beginning. */
}

static void rcv_data(struct tcb *c, uint32 seq, const uint8 *data, int len)
{
    if (seq_le(seq + (uint32)len, c->rcv_nxt))
        return;                                 /* every byte already taken */
    if (seq_lt(seq, c->rcv_nxt)) {              /* partly old: trim the front */
        int skip = (int)(c->rcv_nxt - seq);
        data += skip;
        len  -= skip;
        seq   = c->rcv_nxt;
    }
    if (seq == c->rcv_nxt) {
        int n = rcv_accept(c, data, len);
        c->rcv_nxt += (uint32)n;
        net_putn("  tcp: received ", (unsigned long)n,
                 " bytes, queued for readers\n");
        ooo_drain(c);
    } else {
        ooo_store(c, seq, data, len);
    }
}

/* ---- TCP: input --------------------------------------------------------- */

static void demo_opened(struct tcb *c);
static void net_wakeups(void);       /* answer anyone whose wait is over */
static int  ref_count(const struct tcb *c);
static void reap_dead_clients(void);
static void abort_waiters(struct tcb *c, const char *why);

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
    int flen = tcp_build(dip, dport, sport, rseq, rack, rflags, 0, 0, 0, 0);
    if (flen > 0)
        net_transmit(out, flen);
}

/* The only option this stack reads. A peer that offers none is assumed to
   take 536 bytes, which is what RFC 1122 requires of everybody. */
static void parse_options(struct tcb *c, const uint8 *t, int hlen)
{
    const uint8 *o = t + 20, *end = t + hlen;
    while (o < end) {
        if (*o == 0) break;                     /* end of option list */
        if (*o == 1) { o++; continue; }         /* no-op padding */
        if (o + 1 >= end || o[1] < 2) break;    /* malformed: stop reading */
        if (o[0] == 2 && o[1] == 4 && o + 4 <= end) {
            unsigned m = get16(o + 2);
            if (m >= 128 && m <= 1460)
                c->peer_mss = m;
        }
        o += o[1];
    }
}

/* Everything we have sent has been acknowledged. */
static int all_acked(const struct tcb *c) { return c->snd_una == c->snd_nxt; }

static void enter_time_wait(struct tcb *c)
{
    c->state  = T_TIME_WAIT;
    c->rt_at  = 0;
    c->tw_at  = unow_ms() + TIME_WAIT_MS;
    timers_rearm();
}

static void ack_input(struct tcb *c, uint32 ack)
{
    if (seq_gt(ack, c->snd_nxt))
        return;                                 /* acking what we never sent */

    if (seq_le(ack, c->snd_una)) {
        /* A duplicate acknowledgement means a segment arrived out of order —
           which means one before it may be lost. Three of them is the
           conventional threshold for believing that rather than waiting for
           the timer, and is most of what makes loss recovery quick. */
        if (ack == c->snd_una && c->snd_una != c->snd_nxt && ++c->dupacks == 3) {
            net_puts("  tcp: three duplicate acks — retransmitting at once\n");
            uint32 flight = c->snd_nxt - c->snd_una;
            c->ssthresh = flight / 2 > 2 * MSS ? flight / 2 : 2 * MSS;
            c->cwnd     = c->ssthresh;
            c->snd_nxt  = c->snd_una;
            c->fin_sent = 0;
            c->rtt_timing = 0;
            tcp_output(c);
        }
        return;
    }

    c->dupacks = 0;

    /* Karn: a round-trip time may only be measured from a segment that was
       sent once. rtt_timing is cleared by every retransmission. */
    if (c->rtt_timing && seq_le(c->rtt_seq, ack - 1)) {
        rtt_update(c, (int)(unow_ms() - c->rtt_at));
        c->rtt_timing = 0;
    }

    /* Drop the acknowledged bytes off the front of the send buffer. The SYN
       and the FIN occupy sequence numbers outside it, so the count has to be
       clamped to what the buffer actually holds. */
    if (seq_gt(ack, c->snd_base)) {
        int d = (int)(ack - c->snd_base);
        if (d > c->snd_len)
            d = c->snd_len;
        if (d > 0) {
            for (int i = 0; i + d < c->snd_len; i++)
                c->snd_buf[i] = c->snd_buf[i + d];
            c->snd_len  -= d;
            c->snd_base += (uint32)d;
        }
    }
    c->snd_una = ack;

    /* Slow start until ssthresh, then congestion avoidance: one more segment
       per round trip instead of one more per acknowledgement. On this link
       neither ever has anything to protect against, which is exactly why the
       code has to be right by construction rather than by observation. */
    if (c->cwnd < c->ssthresh)
        c->cwnd += MSS;
    else
        c->cwnd += MSS * MSS / (c->cwnd ? c->cwnd : MSS);
    if (c->cwnd > 65535)
        c->cwnd = 65535;

    if (all_acked(c))
        timer_stop(c);
    else {
        c->rt_at = unow_ms() + (uint64)c->rto;  /* restart for what remains */
        c->rt_tries = 0;
        timers_rearm();
    }
}

static void tcp_segment(const uint8 *sip, const uint8 *t, int seglen)
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
        n->lport    = dport;
        n->rport    = sport;
        n->irs      = seq;
        n->rcv_nxt  = seq + 1;                  /* their SYN counts as one */
        n->iss      = gen_iss();
        n->snd_una  = n->iss;
        n->snd_nxt  = n->iss;
        n->snd_base = n->iss + 1;
        n->snd_wnd  = get16(t + 14);
        n->state    = T_SYN_RCVD;
        parse_options(n, t, hlen);
        tcp_output(n);
        net_puts("  tcp: SYN from ");
        ip_puts(sip);
        net_putn(":", (unsigned long)sport, ", accepted; SYN-ACK sent\n");
        return;
    }

    if (flags & TCP_RST) {
        net_puts("  tcp: reset by peer\n");
        abort_waiters(c, "error refused\n");
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
        parse_options(c, t, hlen);
        if (flags & TCP_ACK) {
            if (seq_le(ack, c->iss) || seq_gt(ack, c->snd_nxt)) {
                tcp_reset(sip, sport, dport, flags, seq, ack, dlen);
                return;
            }
            ack_input(c, ack);
            c->state = T_ESTABLISHED;
            send_ack(c);
            net_puts("  tcp: connection established\n");
            demo_opened(c);
        } else {
            /* Both sides called at once. Rare, and the only reason SYN-RCVD
               is reachable from here. */
            c->state   = T_SYN_RCVD;
            c->snd_nxt = c->iss;                /* resend the SYN with an ack */
            tcp_output(c);
        }
        return;
    }

    /* --- everything from SYN-RCVD onwards -------------------------------- */

    if (flags & TCP_ACK) {
        c->snd_wnd = get16(t + 14);
        ack_input(c, ack);
    }

    switch (c->state) {
    case T_SYN_RCVD:
        if (!(flags & TCP_ACK))
            return;
        c->state = T_ESTABLISHED;
        net_puts("  tcp: connection established (inbound)\n");
        demo_opened(c);
        break;
    case T_FIN_WAIT_1:
        if (c->fin_sent && all_acked(c))
            c->state = T_FIN_WAIT_2;            /* our FIN was taken */
        break;
    case T_CLOSING:
        if (c->fin_sent && all_acked(c)) {
            enter_time_wait(c);
            net_puts("  tcp: closed (simultaneous)\n");
            return;
        }
        break;
    case T_LAST_ACK:
        if (c->fin_sent && all_acked(c)) {
            net_puts("  tcp: closed\n");
            tcb_free(c);
            timers_rearm();
            return;
        }
        break;
    case T_TIME_WAIT:
        /* A retransmitted FIN: acknowledge it again and keep waiting. */
        if (flags & TCP_FIN)
            send_ack(c);
        return;
    default:
        break;
    }

    /* --- data ------------------------------------------------------------ */
    if (dlen > 0) {
        uint32 before = c->rcv_nxt;
        rcv_data(c, seq, t + hlen, dlen);
        send_ack(c);                 /* in order or not, say where we are */
        if (before == c->rcv_nxt && seq != before)
            return;                  /* held aside: no FIN can follow it yet */
    }

    /* --- FIN, but only the one that fits where the stream has got to ----- */
    if ((flags & TCP_FIN) && seq + (uint32)dlen == c->rcv_nxt) {
        c->rcv_nxt++;
        switch (c->state) {
        case T_ESTABLISHED:
            c->state = T_CLOSE_WAIT;
            send_ack(c);
            net_puts("  tcp: peer closed its half\n");
            /* Nobody is holding it open, so there is nothing left to say. */
            if (ref_count(c) == 0)
                tcp_close(c);
            break;
        case T_FIN_WAIT_1:
            send_ack(c);
            if (c->fin_sent && all_acked(c)) {
                enter_time_wait(c);
                net_puts("  tcp: closed\n");
            } else {
                c->state = T_CLOSING;
            }
            break;
        case T_FIN_WAIT_2:
            send_ack(c);
            enter_time_wait(c);
            net_puts("  tcp: closed\n");
            break;
        default:
            break;
        }
    }
}

/* A second test hook, for the other direction. This link never reorders, so
   the reassembly path would never run either, and code that never runs is
   code nobody has checked. The first segment carrying data on an inbound
   connection is held back and the next one is allowed to overtake it; the
   stack must then hold the overtaking segment aside and deliver both, in
   order, once the gap closes.

   It is armed on the listening port rather than on any connection at all,
   so it disturbs exactly the conversation a reader can drive by hand:
   `nc localhost 5555`, then two lines. */
static uint8 hold_buf[2048];
static uint8 hold_sip[4];
static int   hold_len;
static int   reorder_armed = 1;

static void tcp_input(const uint8 *sip, const uint8 *t, int seglen)
{
    int held_before = hold_len;

    if (reorder_armed && seglen > (t[12] >> 4) * 4 &&
        seglen <= (int)sizeof(hold_buf) && get16(t + 2) == LISTEN_PORT) {
        struct tcb *c = tcb_find(sip, get16(t), get16(t + 2));
        if (c && c->state == T_ESTABLISHED) {
            reorder_armed = 0;
            umemcpy(hold_buf, t, (unsigned long)seglen);
            umemcpy(hold_sip, sip, 4);
            hold_len = seglen;
            net_puts("  tcp: [test] holding a segment back so the next overtakes it\n");
            return;
        }
    }

    /* Only something that moves the stream can overtake: a bare
       acknowledgement leaves no gap behind it, so releasing after one would
       prove nothing. A FIN counts, or the held segment would never come back
       at all on a peer that says one thing and hangs up. */
    int overtakes = seglen > (t[12] >> 4) * 4 || (t[13] & TCP_FIN);

    tcp_segment(sip, t, seglen);

    if (hold_len && held_before && overtakes) {
        int n = hold_len;
        hold_len = 0;
        net_puts("  tcp: [test] releasing the held segment\n");
        tcp_segment(hold_sip, hold_buf, n);
    }

    /* Whatever that segment did — queued bytes, closed a half, completed a
       handshake — somebody may have been waiting for it. Doing this once at
       the end rather than at each of the places that could unblock a caller
       keeps replies out of the middle of segment processing. */
    net_wakeups();
}

/* ---- timers ------------------------------------------------------------
   One alarm, several deadlines. The kernel wakes a task at one time; the task
   works out which of its own deadlines that was. This is why user mode needed
   a clock as well as an alarm. */

static void timers_rearm(void)
{
    uint64 now = unow_ms(), best = dns_at;
    if (retry_at && (!best || retry_at < best))
        best = retry_at;
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

/* A question with no answer. UDP will not ask again by itself, so this does —
   which is the whole reason a resolver needs a timer at all. */
static void dns_retry(uint64 now)
{
    if (!dns_at || dns_at > now)
        return;
    if (++dns_tries >= DNS_TRIES) {
        net_puts("  dns: no answer\n");
        dns_at = 0;
        if (dns_waiter.used)
            ctl_answer(&dns_waiter, "error no answer\n", 0);
        return;
    }
    net_puts("  dns: no answer yet, asking again\n");
    dns_send(dns_name);
    dns_at = now + DNS_TIMEOUT;
}

void net_timeout(void)
{
    uint64 now = unow_ms();

    reap_dead_clients();
    net_wakeups();
    dns_retry(now);

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

        if (++c->rt_tries > RTO_TRIES) {
            net_puts("  tcp: giving up after 5 retransmissions\n");
            abort_waiters(c, "error unreachable\n");
            tcb_free(c);
            continue;
        }

        if (c->rt_pending) {
            /* Never addressed at all: the ARP answer had not arrived when it
               was built. Going back through tcp_output rather than resending
               the bytes keeps the sequence bookkeeping in one place. */
            unsigned f = c->rt_pending;
            c->rt_pending = 0;
            c->rt_at = 0;
            if (f & TCP_SYN)
                tcp_output(c);
            else
                send_ack(c);
            if (!c->rt_at)
                c->rt_at = now + (uint64)c->rto;
            continue;
        }

        /* A timeout is the network saying it cannot take this much. Halve the
           threshold, drop the congestion window to one segment, double the
           timeout, and go back to the oldest unacknowledged byte. */
        uint32 flight = c->snd_nxt - c->snd_una;
        c->ssthresh = flight / 2 > 2 * MSS ? flight / 2 : 2 * MSS;
        c->cwnd     = MSS;
        c->rto     *= 2;
        if (c->rto > RTO_MAX)
            c->rto = RTO_MAX;
        c->rtt_timing = 0;                      /* Karn: do not time this one */
        c->snd_nxt    = c->snd_una;
        c->fin_sent   = 0;
        c->rt_at      = 0;

        net_putn("  tcp: timeout, retransmitting (attempt ",
                 (unsigned long)c->rt_tries, ")\n");
        int tries = c->rt_tries;
        tcp_output(c);
        c->rt_tries = tries;                    /* the backoff count survives */
        if (!c->rt_at)
            c->rt_at = now + (uint64)c->rto;
    }
    timers_rearm();
}

/* ---- the network as files ---------------------------------------------
     /net/status   read: the interface, the ARP cache and the whole table
     /net/ctl      write a command, read the answer: connect, listen,
                   accept, close
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
        if (c->state == T_FREE || n > cap - 120)
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
            /* The numbers the stack decided for itself, rather than the ones
               written into it: what it measured the path to be, what it will
               wait before deciding a segment is lost, and how much it is
               willing to have unacknowledged at once. */
            n = app(o, n, "\n        rx ");
            n += uutoa((unsigned long)c->rxq_len, o + n);
            n = app(o, n, "  unacked ");
            n += uutoa((unsigned long)(c->snd_nxt - c->snd_una), o + n);
            n = app(o, n, "  srtt ");
            if (c->srtt < 0) {
                n = app(o, n, "-");
            } else {
                n += uutoa((unsigned long)c->srtt, o + n);
                n = app(o, n, "ms");
            }
            n = app(o, n, "  rto ");
            n += uutoa((unsigned long)c->rto, o + n);
            n = app(o, n, "ms  cwnd ");
            n += uutoa((unsigned long)c->cwnd, o + n);
        }
        o[n++] = '\n';
    }
    return n;
}

/* Descriptors: a slot per open of a rendered file (status, ctl) and
   slot+CONN0 for a connection, so the fd a program holds says which
   conversation it means.

   A rendered file needs a slot of its own because it needs an *offset*. A
   caller that reads until read() returns nothing — which is what `cat` is —
   never stops if every read hands back the whole text again. A report is
   still a file, and a file ends. */
#define NPFD 4
enum { FD_STATUS0 = 1, FD_CTL0 = 8, FD_CONN0 = 16 };

/* Each open gets a buffer of its own, holding either a ctl answer or a
   snapshot of the status report. The snapshot matters: rendering afresh on
   every read means a reader's offset can end up pointing into a *different*
   string than the one it started reading — one number grew a digit between
   two reads and a stray character appeared at the end. A file is a sequence
   of bytes that does not change under the reader; a report that does is not
   a file. */
static struct {
    int  used;
    int  owner;                     /* the task that opened it */
    int  off;
    int  len;                       /* bytes of answer or snapshot waiting */
    char buf[VFS_DATA_MAX];
} pfd[NPFD];

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
    const char *s = p + ustrlen("/net/tcp/");
    if (*s < '0' || *s > '9')
        return -1;
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v < NTCB ? v : -1;
}

/* ---- parking a request -------------------------------------------------
   A read with nothing to read, and an accept with nothing to accept, are
   answered by not answering. The caller is already blocked in the sys_recv
   that follows its sys_send, so nothing has to be invented: the reply simply
   comes later, from whichever event makes an answer possible.

   That is the whole of blocking I/O here, and it needs no help from the
   kernel — which is the point. A microkernel that had to grow a "wait for
   data" primitive for every kind of data would not be minimal for long. */

/* Returns 0 if the answer was taken. A -1 leaves the request parked, and
   nothing has been consumed on its behalf — which is why the bytes of a read
   are copied here but removed from the queue only by the caller, and only
   once this has succeeded. */
static int reply_read(struct parked *p, const char *data, int n)
{
    struct vfs_req r;
    umemset(&r, 0, sizeof(r));
    r.op     = VFS_READ;
    r.fd     = p->fd;
    r.len    = p->len;
    r.result = n;
    if (n > 0)
        umemcpy(r.data, data, (unsigned long)n);
    if (net_reply(p->task, &r) < 0)
        return -1;
    p->used = 0;
    return 0;
}

/* An answer that carries no payload, only a result — the reply to the write
   that asked the question. What the answer says is left in the ctl slot's
   buffer, where the caller's next read will find it, exactly as if the
   command had been carried out on the spot. */
static int reply_done(int task, int fd, int op, int result)
{
    struct vfs_req r;
    umemset(&r, 0, sizeof(r));
    r.op     = op;
    r.fd     = fd;
    r.result = result;
    return net_reply(task, &r);
}

/* Put an answer into the ctl slot the caller will read next, and release the
   write that asked. Every ctl command that cannot be answered on the spot —
   resolve, connect, accept — comes back through here. */
static void ctl_answer(struct parked *p, const char *text, const uint8 *ip)
{
    int pi = p->fd - FD_CTL0;
    if (pi < 0 || pi >= NPFD) {
        p->used = 0;
        return;
    }
    /* Writing the answer into the slot is idempotent, so a refused reply can
       simply be tried again with the text already in place. */
    char *o = pfd[pi].buf;
    int   n = app(o, 0, text);
    if (ip) {
        n = app_ip(o, n, ip);
        o[n++] = '\n';
    }
    pfd[pi].len = n;
    pfd[pi].off = 0;

    if (reply_done(p->task, p->fd, VFS_WRITE, p->len) == 0)
        p->used = 0;
}

/* ---- references, and reclaiming them ----------------------------------
   A program that closes what it opened needs none of this. One that exits
   while holding a descriptor — or faults, which is the same thing seen from
   here — needs somebody to notice, or the connection it was using stays open
   for ever and the slot never comes back.

   Nothing tells this server that a task has died; there is no such message,
   and inventing one would put knowledge of every server into the kernel. It
   asks instead. Ids carry a generation, so the question "is that still the
   task I gave this to?" has an answer even after the slot has been reused. */

static int ref_add(struct tcb *c, int task)
{
    for (int i = 0; i < NREF; i++)
        if (!c->refs[i].used) {
            c->refs[i].used = 1;
            c->refs[i].task = task;
            return 0;
        }
    return -1;
}

static int ref_count(const struct tcb *c)
{
    int n = 0;
    for (int i = 0; i < NREF; i++)
        n += c->refs[i].used;
    return n;
}

static void ref_drop(struct tcb *c, int task)
{
    for (int i = 0; i < NREF; i++)
        if (c->refs[i].used && c->refs[i].task == task) {
            c->refs[i].used = 0;
            return;
        }
}

/* Called before serving any request and on every timer tick: cheap enough at
   this scale that there is no reason to be clever about when. */
static void reap_dead_clients(void)
{
    for (int i = 0; i < NPFD; i++)
        if (pfd[i].used && !sys_alive(pfd[i].owner)) {
            net_putn("  net: reclaiming a control file from dead task ",
                     (unsigned long)pfd[i].owner, "\n");
            pfd[i].used = 0;
        }

    for (int i = 0; i < NTCB; i++) {
        struct tcb *c = &tcbs[i];
        if (c->state == T_FREE)
            continue;
        if (c->reader.used && !sys_alive(c->reader.task))
            c->reader.used = 0;
        if (c->opener.used && !sys_alive(c->opener.task))
            c->opener.used = 0;

        int had = ref_count(c);
        for (int k = 0; k < NREF; k++)
            if (c->refs[k].used && !sys_alive(c->refs[k].task)) {
                net_putn("  net: reclaiming a connection from dead task ",
                         (unsigned long)c->refs[k].task, "\n");
                c->refs[k].used = 0;
            }
        /* The last holder is gone, so the connection has nothing left to say.
           Closing it is what that program would have done had it lived. */
        if (had > 0 && ref_count(c) == 0)
            tcp_close(c);
    }
}

/* A connection that will never come up. Whoever was waiting on it has to be
   told, before the block they are waiting on is wiped. */
static void abort_waiters(struct tcb *c, const char *why)
{
    if (c->opener.used)
        ctl_answer(&c->opener, why, 0);
    if (c->reader.used)
        reply_read(&c->reader, 0, 0);           /* end of file */
}

/* How much a reader may take now, and whether "nothing" means "wait". A
   connection whose peer has closed and whose queue is empty is at end of
   file, and a reader must be told so rather than parked for ever. */
static int conn_eof(const struct tcb *c)
{
    return c->rxq_len == 0 &&
           (c->state == T_CLOSE_WAIT || c->state == T_LAST_ACK ||
            c->state == T_CLOSING    || c->state == T_TIME_WAIT);
}

/* Remove n bytes from the head of the read queue. Separate from copying them
   out, because a reply that is refused must leave the queue as it was. */
static void conn_drop(struct tcb *c, int n)
{
    int was_shut = rcv_window(c) == 0;
    if (n <= 0)
        return;
    c->rxq_len -= n;
    for (int i = 0; i < c->rxq_len; i++)        /* shift the remainder down */
        c->rxq[i] = c->rxq[i + n];
    /* Draining the queue opens the window again, and a peer that has been
       told to stop will not start until it is told so. A stack that skips
       this update deadlocks a connection it throttled. */
    if (was_shut && c->state == T_ESTABLISHED)
        send_ack(c);
}

static int conn_take(struct tcb *c, char *dst, int want)
{
    int n = c->rxq_len < want ? c->rxq_len : want;
    if (n <= 0)
        return 0;
    umemcpy(dst, c->rxq, (unsigned long)n);
    conn_drop(c, n);
    return n;
}

/* Called whenever something might unblock somebody: data queued, a peer
   closing, a connection reaching ESTABLISHED. */
#define RETRY_MS 20

static void net_wakeups(void)
{
    int refused = 0;

    for (int i = 0; i < NTCB; i++) {
        struct tcb *c = &tcbs[i];
        if (c->state == T_FREE)
            continue;
        if (c->opener.used && c->state != T_SYN_SENT && c->state != T_SYN_RCVD) {
            char ok[16];
            int  n = app(ok, 0, "ok ");
            n += uutoa((unsigned long)i, ok + n);
            ok[n++] = '\n';
            ok[n]   = 0;
            ctl_answer(&c->opener, ok, 0);
        }
        if (!c->reader.used)
            continue;
        if (c->rxq_len > 0) {
            /* Copied, offered, and only then consumed: a reply the client is
               too busy to take must not eat the bytes it was carrying. */
            int n = c->rxq_len < c->reader.len ? c->rxq_len : c->reader.len;
            if (reply_read(&c->reader, c->rxq, n) == 0)
                conn_drop(c, n);
            else
                refused = 1;
        } else if (conn_eof(c)) {
            reply_read(&c->reader, 0, 0);       /* end of file, not a wait */
        }
    }

    /* An accept waiting on a listener, and a connection on that port that
       nobody has been given yet. */
    (void)0;
    for (int i = 0; i < NTCB; i++) {
        struct tcb *l = &tcbs[i];
        if (l->state != T_LISTEN || !l->reader.used)
            continue;
        for (int k = 0; k < NTCB; k++) {
            struct tcb *c = &tcbs[k];
            if (c == l || c->state == T_FREE || c->lport != l->lport)
                continue;
            if (c->state != T_ESTABLISHED || c->accepted)
                continue;
            char ok[16];
            int  n = app(ok, 0, "ok ");
            n += uutoa((unsigned long)k, ok + n);
            ok[n++] = '\n';
            ok[n]   = 0;
            if (!l->reader.used)                /* answered by ctl_answer */
                break;
            ctl_answer(&l->reader, ok, 0);
            if (!l->reader.used)
                c->accepted = 1;                /* only once it was taken */
            else
                refused = 1;
            break;
        }
    }

    uint64 now = unow_ms();
    if (refused) {
        retry_at = now + RETRY_MS;
        timers_rearm();
    } else if (retry_at && retry_at <= now) {
        retry_at = 0;
        timers_rearm();
    }
}

/* ---- /net/ctl ----------------------------------------------------------
   Write a line, read the answer. Everything the demo used to decide for
   itself — which port to listen on, which host to call — a program can now
   decide instead, which is the difference between a stack with a demo bolted
   on and a stack with an interface.

     connect <a.b.c.d> <port>     -> ok <slot>
     listen  <port>               -> ok <slot>
     accept  <slot>               -> ok <slot>, when someone calls
     close   <slot>               -> ok
*/

static const char *skip_spaces(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static const char *parse_uint(const char *s, unsigned *out)
{
    if (*s < '0' || *s > '9')
        return 0;
    unsigned v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (unsigned)(*s++ - '0');
    *out = v;
    return s;
}

static const char *parse_ip(const char *s, uint8 *ip)
{
    for (int i = 0; i < 4; i++) {
        unsigned v;
        s = parse_uint(s, &v);
        if (!s || v > 255)
            return 0;
        ip[i] = (uint8)v;
        if (i < 3) {
            if (*s != '.')
                return 0;
            s++;
        }
    }
    return s;
}

static int word_is(const char **s, const char *word)
{
    const char *p = *s;
    while (*word) {
        if (*p != *word)
            return 0;
        p++; word++;
    }
    if (*p && *p != ' ' && *p != '\t')
        return 0;
    *s = skip_spaces(p);
    return 1;
}

/* Returns 1 if the answer is ready in `slot`, 0 if the caller has been
   parked (accept, with nobody calling yet). */
static int ctl_command(int pi, int from, int fd, int wlen, const char *cmd)
{
    char *o = pfd[pi].buf;
    int   n = 0;
    const char *s = skip_spaces(cmd);

    if (word_is(&s, "resolve")) {
        if (!*s) {
            n = app(o, n, "error syntax\n");
        } else if (dns_at) {
            n = app(o, n, "error resolver busy\n");
        } else {
            int k = 0;
            while (s[k] && s[k] != ' ' && k < (int)sizeof(dns_name) - 1) {
                dns_name[k] = s[k];
                k++;
            }
            dns_name[k] = 0;
            dns_waiter.used = 1;
            dns_waiter.task = from;
            dns_waiter.fd   = fd;
            dns_waiter.len  = wlen;
            dns_tries = 0;
            dns_send(dns_name);
            dns_at = unow_ms() + DNS_TIMEOUT;
            timers_rearm();
            return 0;                     /* parked until the answer arrives */
        }
    } else if (word_is(&s, "connect")) {
        uint8 ip[4];
        unsigned port;
        const char *p = parse_ip(s, ip);
        if (!p || !parse_uint(skip_spaces(p), &port))
            n = app(o, n, "error syntax\n");
        else {
            struct tcb *c = tcp_connect(ip, port);
            if (!c)
                n = app(o, n, "error no free connection\n");
            else {
                /* Whoever asked for the connection holds it, from the moment
                   it exists. Waiting for them to open /net/tcp/N would leave
                   a window in which the connection belongs to nobody and
                   would survive its creator. */
                ref_add(c, from);
                /* Park until the handshake finishes. A program has nothing to
                   do with a connection that is not up, and telling it the slot
                   number early only invites it to poll. */
                c->opener.used = 1;
                c->opener.task = from;
                c->opener.fd   = fd;
                c->opener.len  = wlen;
                return 0;
            }
        }
    } else if (word_is(&s, "listen")) {
        unsigned port;
        if (!parse_uint(s, &port))
            n = app(o, n, "error syntax\n");
        else {
            struct tcb *c = tcp_listen(port);
            if (!c)
                n = app(o, n, "error no free connection\n");
            else {
                ref_add(c, from);       /* the port belongs to whoever asked */
                n = app(o, n, "ok ");
                n += uutoa((unsigned long)(c - tcbs), o + n);
                o[n++] = '\n';
            }
        }
    } else if (word_is(&s, "accept")) {
        unsigned slot;
        if (!parse_uint(s, &slot) || slot >= NTCB ||
            tcbs[slot].state != T_LISTEN) {
            n = app(o, n, "error not a listener\n");
        } else {
            struct tcb *l = &tcbs[slot];
            if (l->reader.used) {
                n = app(o, n, "error already accepting\n");
            } else {
                /* Park it, then look: net_wakeups answers at once if a
                   connection is already sitting there unclaimed. */
                l->reader.used = 1;
                l->reader.task = from;
                l->reader.fd   = fd;
                l->reader.len  = wlen;   /* what to report to the write */
                net_wakeups();
                return 0;                /* parked, or answered inside there */
            }
        }
    } else if (word_is(&s, "address")) {
        uint8 ip[4];
        if (!parse_ip(s, ip)) {
            n = app(o, n, "error syntax\n");
        } else {
            umemcpy(me_ip, ip, 4);
            net_puts("  net: this machine is now ");
            ip_puts(me_ip);
            net_puts("\n");
            n = app(o, n, "ok\n");
        }
    } else if (word_is(&s, "close")) {
        unsigned slot;
        if (!parse_uint(s, &slot) || slot >= NTCB ||
            tcbs[slot].state == T_FREE)
            n = app(o, n, "error no such connection\n");
        else {
            tcp_close(&tcbs[slot]);
            n = app(o, n, "ok\n");
        }
    } else {
        n = app(o, n, "error unknown command\n");
    }

    pfd[pi].len = n;
    pfd[pi].off = 0;
    return 1;
}

/* Is this path exactly `name`, with or without a trailing slash? A directory
   answers to both spellings, because a person types one and a program that
   joins a name onto a prefix produces the other. */
static int net_is_name(const char *p, const char *name)
{
    int i = 0;
    while (name[i] && p[i] == name[i])
        i++;
    if (name[i])
        return 0;
    return p[i] == 0 || (p[i] == '/' && p[i + 1] == 0);
}

/* What is in /net, and what is in /net/tcp. The same two-fields-then-a-name
   shape the filesystem uses, because a listing is a listing and `ls` should
   not have to know which server produced it. A connection appears here the
   moment it exists, which makes `ls /net/tcp` the shortest way to ask what
   this machine is talking to. */
static int net_dir(const char *path, char *out, int cap)
{
    int o = 0;
    if (net_is_name(path, "/net")) {
        const char *fixed = "- 0 ctl\n- 0 status\nd 0 tcp\n";
        while (fixed[o] && o < cap - 1) { out[o] = fixed[o]; o++; }
        return o;
    }
    for (int i = 0; i < NTCB && o < cap - 24; i++) {
        if (tcbs[i].state == T_FREE)
            continue;
        out[o++] = '-';
        out[o++] = ' ';
        out[o++] = '0';
        out[o++] = ' ';
        o += uutoa((unsigned long)i, out + o);
        out[o++] = '\n';
    }
    return o;
}

int net_vfs(int from, struct vfs_req *r)
{
    reap_dead_clients();
    net_wakeups();          /* any reply refused earlier gets another go */

    switch (r->op) {
    case VFS_OPEN: {
        int base = -1, dir = 0;
        /* The directories first: "/net/tcp/0" begins with "/net/tcp" and the
           order of these tests is the difference between a connection and a
           listing of them. */
        if (net_is_name(r->path, "/net") || net_is_name(r->path, "/net/tcp")) {
            base = FD_STATUS0;
            dir  = 1;
        }
        else if (ustr_has_prefix(r->path, "/net/status"))
            base = FD_STATUS0;
        else if (ustr_has_prefix(r->path, "/net/ctl"))
            base = FD_CTL0;

        if (base >= 0) {
            r->result = -1;
            for (int i = 0; i < NPFD; i++)
                if (!pfd[i].used) {
                    pfd[i].used  = 1;
                    pfd[i].owner = from;
                    pfd[i].off   = 0;
                    /* The status file is rendered once, here: what the caller
                       reads is the system as it was when it opened it. */
                    pfd[i].len   = dir
                                 ? net_dir(r->path, pfd[i].buf,
                                           (int)sizeof(pfd[i].buf))
                                 : base == FD_STATUS0
                                 ? net_status(pfd[i].buf, (int)sizeof(pfd[i].buf))
                                 : 0;
                    r->result = base + i;
                    break;
                }
        } else if (ustr_has_prefix(r->path, "/net/tcp/")) {
            int i = path_slot(r->path);
            if (i < 0 || tcbs[i].state == T_FREE) {
                r->result = -1;
            } else if (ref_add(&tcbs[i], from) < 0) {
                r->result = -1;             /* too many holders */
            } else {
                r->result = FD_CONN0 + i;
            }
        } else if (ustr_has_prefix(r->path, "/net/tcp")) {
            int i = client_slot();
            if (i < 0) {
                r->result = -1;
            } else if (ref_add(&tcbs[i], from) < 0) {
                r->result = -1;
            } else {
                r->result = FD_CONN0 + i;
            }
        } else {
            r->result = -1;
        }
        break;
    }

    case VFS_IOCTL:
        /* Answered before anything else is looked at: a ping asks whether
           this task is still turning its loop, not about a connection. */
        r->result = (r->ioctl_cmd == IOCTL_PING) ? 0 : -1;
        break;

    case VFS_READ:
        if ((r->fd >= FD_STATUS0 && r->fd < FD_STATUS0 + NPFD) ||
            (r->fd >= FD_CTL0    && r->fd < FD_CTL0 + NPFD)) {
            int i = (r->fd >= FD_CTL0 ? r->fd - FD_CTL0 : r->fd - FD_STATUS0);
            if (!pfd[i].used) {
                r->result = -1;
                break;
            }
            int n = pfd[i].len - pfd[i].off;
            if (n > r->len)
                n = r->len;
            if (n <= 0) {
                r->result = 0;                          /* end of file */
            } else {
                umemcpy(r->data, pfd[i].buf + pfd[i].off, (unsigned long)n);
                pfd[i].off += n;
                r->result = n;
            }
        } else {
            struct tcb *c = conn_of(r->fd);
            if (!c) {
                r->result = -1;
                break;
            }
            int n = conn_take(c, r->data, r->len);
            if (n > 0) {
                r->result = n;
            } else if (conn_eof(c)) {
                r->result = 0;                  /* the peer is done talking */
            } else if (c->reader.used) {
                r->result = -1;                 /* one reader per connection */
            } else {
                c->reader.used = 1;             /* park it */
                c->reader.task = from;
                c->reader.fd   = r->fd;
                c->reader.len  = r->len;
                return 0;
            }
        }
        break;

    case VFS_WRITE:
        if (r->fd >= FD_CTL0 && r->fd < FD_CTL0 + NPFD) {
            int i = r->fd - FD_CTL0;
            char cmd[VFS_DATA_MAX + 1];
            int len = r->len < VFS_DATA_MAX ? r->len : VFS_DATA_MAX;
            umemcpy(cmd, r->data, (unsigned long)len);
            cmd[len] = 0;
            for (int k = 0; k < len; k++)
                if (cmd[k] == '\n' || cmd[k] == '\r')
                    cmd[k] = 0;
            if (!ctl_command(i, from, r->fd, r->len, cmd))
                return 0;              /* accept: parked, or answered already */
            r->result = r->len;
        } else {
            struct tcb *c = conn_of(r->fd);
            if (!c || (c->state != T_ESTABLISHED && c->state != T_CLOSE_WAIT)) {
                r->result = -1;                 /* not connected */
            } else {
                /* A write copies into the send buffer and returns; how much
                   of it goes on the wire now is the window's business, not
                   the caller's. A short count means the buffer is full, which
                   is the honest answer and the one a program can act on. */
                r->result = tcp_write(c, r->data, r->len);
            }
        }
        break;

    case VFS_CLOSE:
        if (r->fd >= FD_STATUS0 && r->fd < FD_STATUS0 + NPFD)
            pfd[r->fd - FD_STATUS0].used = 0;
        else if (r->fd >= FD_CTL0 && r->fd < FD_CTL0 + NPFD)
            pfd[r->fd - FD_CTL0].used = 0;
        else {
            /* Closing the last descriptor closes the connection. That is what
               a file interface means by close, and it is what TCP means by it
               too — a program that has stopped reading and writing has said
               everything it is going to say. */
            struct tcb *c = conn_of(r->fd);
            if (c) {
                ref_drop(c, from);
                if (ref_count(c) == 0)
                    tcp_close(c);
            }
        }
        r->result = 0;
        break;

    default:
        r->result = -1;
        break;
    }
    return 1;
}


/* ---- the demo ----------------------------------------------------------
   What is left of it. Everything here is policy — which host to talk to and
   what to say — and stage by stage it has been moving out of this file and
   into programs, which is where it belongs. What remains is the outbound
   call the boot sequence makes so that /net/tcp has something behind it for
   `hello.elf` to find. Naming a host, resolving it and fetching a page is
   /GET.ELF's job now, not the stack's. */

static int demo_done;

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

static void demo_opened(struct tcb *c)
{
    if (c->is_client)
        net_puts("  tcp: /net/tcp is open for business\n");
}

void net_start(void)
{
    /* No listener here any more: a port is opened by whoever wants to answer
       on it, through /net/ctl.

       Both hardware addresses are asked for at once. Nothing can be sent to
       either host until its answer arrives, and a UDP query has no
       retransmission to fall back on, so the query waits for the reply rather
       than the other way round. */
    arp_request(gw_ip);
    arp_request(dns_ip);
}

/* An address became known. Whichever it was, something was waiting for it. */
static void demo_arp_ready(const uint8 *ip)
{
    if (ip_eq(ip, gw_ip))
        demo_start();
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
