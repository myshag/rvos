#pragma once
#include "vfs.h"

/* fsproto.h — the file protocol, on a wire.

   Inside one machine a request is a `struct vfs_req` handed to the kernel,
   which copies 576 bytes from one address space into another. Between two
   machines that will not do, and it is worth being exact about why: 576 is
   what the compiler chose, including padding it inserted for its own reasons.
   A protocol whose meaning depends on a compiler's padding is not a protocol.
   So the wire form is written out field by field, little-endian, with an
   explicit length for everything that has one.

   It is also much smaller. A one-byte read is 33 bytes here and 576 there.

     u32  len        the whole message, including this field
     u16  tag        which request a reply answers
     u8   kind       0 = request, 1 = reply
     u8   op         VFS_OPEN … VFS_CLOSE
     u32  fd         the server's own descriptor number
     u32  count      bytes wanted (request) or moved (reply)
     i32  result     the answer
     u64  ioctl
     u16  pathlen
     u16  datalen
     …    path, then data

   The tag is not needed by the implementation that follows — it keeps one
   request outstanding at a time — and is here anyway, because pipelining is
   the obvious next thing and a format is the wrong place to economise.

   What this does not have, and a real one would: authentication of any kind
   (anyone who reaches the port gets the namespace), a version number, and any
   notion of the two sides disagreeing about anything. */

#define FS_HDR    32
#define FS_MAXMSG (FS_HDR + VFS_PATH_MAX + VFS_DATA_MAX)

enum { FS_REQUEST = 0, FS_REPLY = 1 };

struct fs_msg {
    unsigned tag;
    int      kind;
    int      op;
    int      fd;
    int      count;
    int      result;
    unsigned long ioctl;
    int      pathlen;
    int      datalen;
    char     path[VFS_PATH_MAX];
    char     data[VFS_DATA_MAX];
};

static inline void fs_put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static inline void fs_put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}
static inline unsigned long fs_get32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
static inline unsigned fs_get16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

/* -> the number of bytes written, or -1 if it would not fit. */
static inline int fs_encode(const struct fs_msg *m, unsigned char *out, int cap)
{
    int pl = m->pathlen, dl = m->datalen;
    if (pl < 0) pl = 0;
    if (dl < 0) dl = 0;
    if (pl > VFS_PATH_MAX) pl = VFS_PATH_MAX;
    if (dl > VFS_DATA_MAX) dl = VFS_DATA_MAX;

    int len = FS_HDR + pl + dl;
    if (len > cap)
        return -1;

    fs_put32(out,      (unsigned long)len);
    fs_put16(out + 4,  m->tag);
    out[6] = (unsigned char)m->kind;
    out[7] = (unsigned char)m->op;
    fs_put32(out + 8,  (unsigned long)(unsigned)m->fd);
    fs_put32(out + 12, (unsigned long)(unsigned)m->count);
    fs_put32(out + 16, (unsigned long)(unsigned)m->result);
    fs_put32(out + 20, m->ioctl & 0xffffffffUL);
    fs_put32(out + 24, m->ioctl >> 32);
    fs_put16(out + 28, (unsigned)pl);
    fs_put16(out + 30, (unsigned)dl);

    for (int i = 0; i < pl; i++)
        out[FS_HDR + i] = (unsigned char)m->path[i];
    for (int i = 0; i < dl; i++)
        out[FS_HDR + pl + i] = (unsigned char)m->data[i];
    return len;
}

/* -> bytes consumed, 0 if the buffer does not yet hold a whole message, -1 if
   what it holds cannot be one. A stream reader has to be able to say all
   three things; a reader that only says "yes" or "no" cannot tell a short
   read from a broken peer. */
static inline int fs_decode(const unsigned char *in, int have, struct fs_msg *m)
{
    if (have < 4)
        return 0;
    unsigned long len = fs_get32(in);
    if (len < FS_HDR || len > (unsigned long)FS_MAXMSG)
        return -1;
    if ((unsigned long)have < len)
        return 0;

    m->tag     = fs_get16(in + 4);
    m->kind    = in[6];
    m->op      = in[7];
    m->fd      = (int)fs_get32(in + 8);
    m->count   = (int)fs_get32(in + 12);
    m->result  = (int)fs_get32(in + 16);
    m->ioctl   = fs_get32(in + 20) | (fs_get32(in + 24) << 32);
    m->pathlen = (int)fs_get16(in + 28);
    m->datalen = (int)fs_get16(in + 30);

    if (m->pathlen < 0 || m->pathlen > VFS_PATH_MAX ||
        m->datalen < 0 || m->datalen > VFS_DATA_MAX ||
        (unsigned long)(FS_HDR + m->pathlen + m->datalen) != len)
        return -1;                      /* the lengths disagree with the length */

    for (int i = 0; i < m->pathlen; i++)
        m->path[i] = (char)in[FS_HDR + i];
    for (int i = 0; i < m->datalen; i++)
        m->data[i] = (char)in[FS_HDR + m->pathlen + i];
    return (int)len;
}
