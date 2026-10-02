// silicon - TCP (client side): handshake, ordered receive with acks, go-back-N retransmission
// with exponential backoff, flow control by the peer's window, graceful close and resets.
#include "net.h"

struct Sock {
    int  state;
    ip4  rip;
    u16  rport, lport;
    u32  iss, snd_una, snd_wnd, rcv_nxt;
    u16  mss;
    u8*  tx; u32 tx_len, tx_cap, tx_sent;   // tx[0] is the byte at seq snd_una
    u8*  rx; u32 rx_len, rx_cap, rx_read;
    bool fin_rcvd, fin_sent, fin_acked, app_closed, failed;
    u64  rto_at; u32 rto; int retries;
    u64  tw_until;
};

static Sock socks[16];
static u16 next_port = 0;

static bool seq_lt(u32 a, u32 b) { return (i32)(a - b) < 0; }
static bool seq_le(u32 a, u32 b) { return (i32)(a - b) <= 0; }

const char* tcp_state_name(int st) {
    static const char* n[] = {"free", "closed", "syn-sent", "established", "fin-wait-1", "fin-wait-2",
                              "close-wait", "last-ack", "time-wait", "closing"};
    return n[clamp(st, 0, 9)];
}

static u32 rx_free(Sock& s) { return s.rx_cap - (s.rx_len - s.rx_read); }

static void send_seg(Sock& s, u32 seq, u8 flags, const u8* data, u32 len) {
    u8 pkt[1600];
    u32 hl = (flags & 0x02) ? 24 : 20;
    pkt[0] = s.lport >> 8; pkt[1] = s.lport & 0xFF;
    pkt[2] = s.rport >> 8; pkt[3] = s.rport & 0xFF;
    u32 nseq = htonl(seq), nack = htonl(s.rcv_nxt);
    memcpy(pkt + 4, &nseq, 4);
    memcpy(pkt + 8, &nack, 4);
    pkt[12] = (u8)((hl / 4) << 4);
    pkt[13] = flags;
    u16 win = (u16)min<u32>(rx_free(s), 48 * 1024);   // a burst this big still fits the NIC's ring
    pkt[14] = win >> 8; pkt[15] = win & 0xFF;
    pkt[16] = pkt[17] = 0;
    pkt[18] = pkt[19] = 0;
    if (hl == 24) { pkt[20] = 2; pkt[21] = 4; pkt[22] = 1460 >> 8; pkt[23] = 1460 & 0xFF; }
    if (len) memcpy(pkt + hl, data, len);
    // checksum over the pseudo header + segment
    u8 ph[12];
    memcpy(ph, &netcfg.ip, 4);
    memcpy(ph + 4, &s.rip, 4);
    ph[8] = 0; ph[9] = 6;
    ph[10] = (hl + len) >> 8; ph[11] = (hl + len) & 0xFF;
    u32 sum = 0;
    for (int i = 0; i < 12; i += 2) sum += (ph[i] << 8) | ph[i + 1];
    u16 c = ip_checksum(pkt, hl + len, sum);
    memcpy(pkt + 16, &c, 2);
    ip_send(s.rip, 6, pkt, hl + len);
}

static void send_ack(Sock& s) { send_seg(s, s.snd_una + s.tx_sent + (s.fin_sent ? 1 : 0), 0x10, nullptr, 0); }

static void arm(Sock& s) { s.rto_at = ticks() + s.rto; }

// push out whatever the window allows
static void output(Sock& s) {
    if (s.state == TCP_SYN_SENT) return;
    if (s.state != TCP_ESTABLISHED && s.state != TCP_CLOSE_WAIT && s.state != TCP_FIN_WAIT1 && s.state != TCP_LAST_ACK) return;
    u32 wnd = max<u32>(s.snd_wnd, 1);
    while (s.tx_sent < s.tx_len && s.tx_sent < wnd) {
        u32 n = min(min<u32>(s.mss, s.tx_len - s.tx_sent), wnd - s.tx_sent);
        send_seg(s, s.snd_una + s.tx_sent, 0x18, s.tx + s.tx_sent, n);
        s.tx_sent += n;
        if (!s.rto_at) arm(s);
    }
    if (s.app_closed && !s.fin_sent && s.tx_sent == s.tx_len &&
        (s.state == TCP_ESTABLISHED || s.state == TCP_CLOSE_WAIT)) {
        send_seg(s, s.snd_una + s.tx_len, 0x11, nullptr, 0);
        s.fin_sent = true;
        s.state = s.state == TCP_ESTABLISHED ? TCP_FIN_WAIT1 : TCP_LAST_ACK;
        if (!s.rto_at) arm(s);
    }
}

int tcp_connect(ip4 dst, u16 port) {
    if (!net_link_up()) return -1;
    int id = -1;
    for (int i = 0; i < 16; i++) if (socks[i].state == TCP_FREE) { id = i; break; }
    if (id < 0) return -1;
    Sock& s = socks[id];
    if (s.tx) kfree(s.tx);
    if (s.rx) kfree(s.rx);
    memset(&s, 0, sizeof(s));
    if (!next_port) next_port = 49152 + rand() % 8000;
    s.lport = next_port++;
    if (next_port > 65000) next_port = 49152;
    s.rip = dst;
    s.rport = port;
    s.iss = rand();
    s.snd_una = s.iss;
    s.mss = 536;
    s.rx_cap = 64 * 1024;
    s.rx = (u8*)kmalloc(s.rx_cap);
    s.tx_cap = 8 * 1024;
    s.tx = (u8*)kmalloc(s.tx_cap);
    s.rto = 700;
    s.state = TCP_SYN_SENT;
    send_seg(s, s.iss, 0x02, nullptr, 0);
    arm(s);
    return id;
}

int tcp_state(int id) { return id >= 0 && id < 16 ? socks[id].state : TCP_FREE; }

bool tcp_send(int id, const void* data, u32 len) {
    if (id < 0 || id >= 16) return false;
    Sock& s = socks[id];
    if (s.state != TCP_ESTABLISHED && s.state != TCP_CLOSE_WAIT && s.state != TCP_SYN_SENT) return false;
    if (s.app_closed) return false;
    if (s.tx_len + len > s.tx_cap) {
        u32 nc = max(s.tx_cap * 2, s.tx_len + len);
        s.tx = (u8*)krealloc(s.tx, nc);
        s.tx_cap = nc;
    }
    memcpy(s.tx + s.tx_len, data, len);
    s.tx_len += len;
    output(s);
    return true;
}

u32 tcp_available(int id) { return id >= 0 && id < 16 ? socks[id].rx_len - socks[id].rx_read : 0; }

u32 tcp_recv(int id, void* buf, u32 max) {
    if (id < 0 || id >= 16) return 0;
    Sock& s = socks[id];
    u32 n = min(max, s.rx_len - s.rx_read);
    u32 free_before = rx_free(s);
    memcpy(buf, s.rx + s.rx_read, n);
    s.rx_read += n;
    // the window was (nearly) shut and reading opened it up again: the peer must hear about it,
    // or it sits waiting for its persist timer
    bool was_closed = free_before < 24 * 1024 && rx_free(s) >= free_before + 8192;
    if (s.rx_read == s.rx_len) s.rx_read = s.rx_len = 0;
    else if (s.rx_read > s.rx_cap / 2) {
        memmove(s.rx, s.rx + s.rx_read, s.rx_len - s.rx_read);
        s.rx_len -= s.rx_read;
        s.rx_read = 0;
    }
    // window was nearly shut: tell the peer it reopened
    if (was_closed && n && (s.state == TCP_ESTABLISHED || s.state == TCP_FIN_WAIT1 || s.state == TCP_FIN_WAIT2)) send_ack(s);
    return n;
}

bool tcp_peer_closed(int id) { return id >= 0 && id < 16 && socks[id].fin_rcvd && tcp_available(id) == 0; }
bool tcp_failed(int id) { return id < 0 || id >= 16 || socks[id].failed; }

void tcp_close(int id) {
    if (id < 0 || id >= 16) return;
    Sock& s = socks[id];
    s.app_closed = true;
    if (s.state == TCP_SYN_SENT || s.failed || s.state == TCP_CLOSED) { s.state = TCP_FREE; return; }
    output(s);
}

void tcp_abort(int id) {
    if (id < 0 || id >= 16) return;
    Sock& s = socks[id];
    if (s.state != TCP_FREE && s.state != TCP_CLOSED && !s.failed) send_seg(s, s.snd_una + s.tx_sent, 0x14, nullptr, 0);
    s.state = TCP_FREE;
}

void tcp_receive(ip4 src, ip4, const u8* d, u32 len) {
    if (len < 20) return;
    u16 sport = (d[0] << 8) | d[1], dport = (d[2] << 8) | d[3];
    u32 seq, ack;
    memcpy(&seq, d + 4, 4);
    memcpy(&ack, d + 8, 4);
    seq = ntohl(seq);
    ack = ntohl(ack);
    u32 hl = (d[12] >> 4) * 4;
    u8 flags = d[13];
    u32 win = (d[14] << 8) | d[15];
    if (hl < 20 || hl > len) return;
    Sock* sp = nullptr;
    for (auto& s : socks)
        if (s.state != TCP_FREE && s.rip == src && s.rport == sport && s.lport == dport) { sp = &s; break; }
    if (!sp) return;
    Sock& s = *sp;
    const u8* payload = d + hl;
    u32 plen = len - hl;

    if (flags & 0x04) {                                   // RST
        s.failed = true;
        s.state = TCP_CLOSED;
        if (s.app_closed) s.state = TCP_FREE;
        return;
    }
    if (s.state == TCP_SYN_SENT) {
        if ((flags & 0x12) == 0x12 && ack == s.iss + 1) {
            s.rcv_nxt = seq + 1;
            s.snd_una = ack;
            s.snd_wnd = win;
            // MSS option
            for (u32 o = 20; o + 1 < hl;) {
                u8 kind = d[o];
                if (kind == 0) break;
                if (kind == 1) { o++; continue; }
                u8 l = d[o + 1];
                if (kind == 2 && l == 4) s.mss = (u16)min(1460, (d[o + 2] << 8) | d[o + 3]);
                if (l < 2) break;
                o += l;
            }
            s.state = TCP_ESTABLISHED;
            s.rto_at = 0;
            s.retries = 0;
            send_ack(s);
            output(s);
        }
        return;
    }

    // ACK processing
    if (flags & 0x10) {
        u32 top = s.snd_una + s.tx_sent + (s.fin_sent ? 1 : 0);
        if (seq_lt(s.snd_una, ack) && seq_le(ack, top)) {
            u32 acked = ack - s.snd_una;
            u32 data_acked = min(acked, s.tx_len);
            if (s.fin_sent && ack == top) s.fin_acked = true;
            memmove(s.tx, s.tx + data_acked, s.tx_len - data_acked);
            s.tx_len -= data_acked;
            s.tx_sent = s.tx_sent > data_acked ? s.tx_sent - data_acked : 0;
            s.snd_una += acked;
            s.retries = 0;
            s.rto = 700;
            s.rto_at = (s.tx_sent || (s.fin_sent && !s.fin_acked)) ? ticks() + s.rto : 0;
        }
        s.snd_wnd = win;
        if (s.fin_acked) {
            if (s.state == TCP_FIN_WAIT1) s.state = TCP_FIN_WAIT2;
            else if (s.state == TCP_CLOSING) { s.state = TCP_TIME_WAIT; s.tw_until = ticks() + 1500; }
            else if (s.state == TCP_LAST_ACK) { s.state = s.app_closed ? TCP_FREE : TCP_CLOSED; return; }
        }
    }

    // data, in order only
    bool need_ack = false;
    if (plen) {
        if (seq == s.rcv_nxt && !s.fin_rcvd) {
            u32 take = min(plen, rx_free(s));
            if (s.rx_len + take > s.rx_cap) {
                memmove(s.rx, s.rx + s.rx_read, s.rx_len - s.rx_read);
                s.rx_len -= s.rx_read;
                s.rx_read = 0;
            }
            memcpy(s.rx + s.rx_len, payload, take);
            s.rx_len += take;
            s.rcv_nxt += take;
        }
        need_ack = true;                                  // ack in-order data, re-ack anything else
    }
    if ((flags & 0x01) && seq + plen == s.rcv_nxt && !s.fin_rcvd) {
        s.rcv_nxt++;
        s.fin_rcvd = true;
        need_ack = true;
        if (s.state == TCP_ESTABLISHED) s.state = TCP_CLOSE_WAIT;
        else if (s.state == TCP_FIN_WAIT1) s.state = s.fin_acked ? TCP_TIME_WAIT : TCP_CLOSING;
        else if (s.state == TCP_FIN_WAIT2) s.state = TCP_TIME_WAIT;
        if (s.state == TCP_TIME_WAIT) s.tw_until = ticks() + 1500;
    }
    if (need_ack) send_ack(s);
    output(s);
}

void tcp_tick() {
    u64 t = ticks();
    for (auto& s : socks) {
        if (s.state == TCP_FREE) continue;
        if (s.state == TCP_TIME_WAIT && t > s.tw_until) { s.state = s.app_closed ? TCP_FREE : TCP_CLOSED; continue; }
        if (s.state == TCP_CLOSED && s.app_closed) { s.state = TCP_FREE; continue; }
        if (!s.rto_at || t < s.rto_at) continue;
        if (++s.retries > 7) {
            s.failed = true;
            s.state = s.app_closed ? TCP_FREE : TCP_CLOSED;
            continue;
        }
        s.rto = min<u32>(s.rto * 2, 6000);
        if (s.state == TCP_SYN_SENT) { send_seg(s, s.iss, 0x02, nullptr, 0); arm(s); continue; }
        // go back N: resend everything unacknowledged
        s.tx_sent = 0;
        if (s.fin_sent && !s.fin_acked) {
            s.fin_sent = false;
            if (s.state == TCP_FIN_WAIT1) s.state = TCP_ESTABLISHED;
            else if (s.state == TCP_LAST_ACK) s.state = TCP_CLOSE_WAIT;
            else if (s.state == TCP_CLOSING) s.state = TCP_CLOSE_WAIT;
        }
        s.rto_at = 0;
        output(s);
        if (!s.rto_at) arm(s);
    }
}

int tcp_socket_count() { return 16; }

// is any connection moving data (or about to)? then the main loops shouldn't hlt
bool tcp_busy() {
    for (auto& s : socks)
        if (s.state != TCP_FREE && s.state != TCP_CLOSED && s.state != TCP_TIME_WAIT) return true;
    return false;
}

bool tcp_socket_info(int i, int* state, ip4* rip, u16* rport, u16* lport, u32* rx, u32* tx) {
    Sock& s = socks[i];
    if (s.state == TCP_FREE) return false;
    *state = s.state; *rip = s.rip; *rport = s.rport; *lport = s.lport;
    *rx = s.rx_len - s.rx_read; *tx = s.tx_len;
    return true;
}
