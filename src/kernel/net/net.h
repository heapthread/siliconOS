// silicon - networking: ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP, HTTP(S)
// Everything is non-blocking and driven by net_poll() from the main loop.
#pragma once
#include "../core.h"

// ---------------------------------------------------------------- byte order
static inline u16 htons(u16 v) { return (u16)((v >> 8) | (v << 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }
static inline u32 htonl(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }
static inline u32 ntohl(u32 v) { return htonl(v); }

typedef u32 ip4;                      // stored in network byte order
static inline ip4 make_ip(u8 a, u8 b, u8 c, u8 d) { return (u32)a | ((u32)b << 8) | ((u32)c << 16) | ((u32)d << 24); }
void ip_to_str(ip4 ip, char* out);
bool str_to_ip(const char* s, ip4* out);

// ---------------------------------------------------------------- core
struct NetConfig {
    bool  up;                         // a NIC was found
    bool  configured;                 // we have an address
    bool  dhcp;                       // address came from DHCP
    u8    mac[6];
    ip4   ip, mask, gateway, dns;
};
extern NetConfig netcfg;

void net_init();
void net_poll();                       // call often (main loops)
bool net_link_up();
bool net_busy();                       // traffic in flight: poll often instead of sleeping
void net_receive_frame(const u8* frame, u32 len);   // from the driver
void net_stats(u64* txp, u64* rxp, u64* txb, u64* rxb);
void net_dhcp_start();

// ---------------------------------------------------------------- ip layer
bool ip_send(ip4 dst, u8 proto, const void* payload, u32 len);
u16  ip_checksum(const void* data, u32 len, u32 initial = 0);

// ---------------------------------------------------------------- icmp (ping)
typedef void (*PingReply)(void* ctx, u16 seq, u32 rtt_ms, ip4 from, u8 ttl);
bool icmp_ping(ip4 dst, u16 id, u16 seq, PingReply cb, void* ctx);

// ---------------------------------------------------------------- udp
typedef void (*UdpHandler)(ip4 src, u16 sport, const u8* data, u32 len);
bool udp_bind(u16 port, UdpHandler h);
void udp_unbind(u16 port);
bool udp_send(ip4 dst, u16 sport, u16 dport, const void* data, u32 len);

// ---------------------------------------------------------------- dns
// returns 1 resolved (ip filled), 0 still working, -1 failed; call repeatedly
int  dns_resolve(const char* name, ip4* ip);
void dns_flush();

// ---------------------------------------------------------------- tcp
enum TcpState { TCP_FREE, TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT1, TCP_FIN_WAIT2,
                TCP_CLOSE_WAIT, TCP_LAST_ACK, TCP_TIME_WAIT, TCP_CLOSING };
int  tcp_connect(ip4 dst, u16 port);   // socket id or -1
int  tcp_state(int s);
bool tcp_send(int s, const void* data, u32 len);
u32  tcp_available(int s);
u32  tcp_recv(int s, void* buf, u32 max);
bool tcp_peer_closed(int s);           // FIN received and all data read
bool tcp_failed(int s);                // reset / timed out
void tcp_close(int s);                 // graceful close, socket frees itself later
void tcp_abort(int s);
const char* tcp_state_name(int st);
int  tcp_socket_count();
bool tcp_socket_info(int i, int* state, ip4* rip, u16* rport, u16* lport, u32* rx, u32* tx);

// ---------------------------------------------------------------- tls (net/tls.cpp)
struct TlsConn;
TlsConn* tls_new(int tcp_sock, const char* host);
int  tls_handshake(TlsConn* t);       // 1 done, 0 in progress, -1 failed
bool tls_send(TlsConn* t, const void* data, u32 len);
u32  tls_recv(TlsConn* t, void* buf, u32 max);
bool tls_closed(TlsConn* t);
const char* tls_error(TlsConn* t);
const char* tls_version(TlsConn* t);   // "tls 1.2" / "tls 1.3"
void tls_free(TlsConn* t);

// ---------------------------------------------------------------- http(s) client
struct HttpRequest;
enum HttpPhase { HTTP_RESOLVING, HTTP_CONNECTING, HTTP_HANDSHAKE, HTTP_SENDING, HTTP_RECEIVING, HTTP_DONE, HTTP_ERROR };
HttpRequest* http_get(const char* url, const char* method = "GET", const char* body = nullptr, const char* content_type = nullptr);
int   http_step(HttpRequest* r);      // returns the phase; call every tick until DONE/ERROR
int   http_status(HttpRequest* r);
void  http_set_accept(HttpRequest* r, const char* accept);   // before the first http_step
const char* http_error(HttpRequest* r);
const char* http_final_url(HttpRequest* r);
const char* http_content_type(HttpRequest* r);
const char* http_tls_version(HttpRequest* r);   // of the last connection, "" for plain http
const char* http_header(HttpRequest* r, const char* name);
const u8* http_body(HttpRequest* r, u32* len);
u32   http_received(HttpRequest* r);
u32   http_expected(HttpRequest* r);  // content-length or 0
void  http_free(HttpRequest* r);

// url helpers
struct Url { char scheme[8]; char host[128]; u16 port; char path[512]; };
bool url_parse(const char* s, Url& u);
void url_resolve(const char* base, const char* rel, char* out, int n);
void url_encode(const char* in, char* out, int n);
