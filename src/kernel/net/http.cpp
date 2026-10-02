// silicon - HTTP/1.1 client (http and https), non-blocking: call http_step() every tick.
// Handles content-length, chunked transfer encoding, redirects and timeouts.
#include "net.h"

// ================================================================ urls
bool url_parse(const char* s, Url& u) {
    memset(&u, 0, sizeof(u));
    while (*s == ' ') s++;
    const char* p = strchr(s, ':');
    const char* slash = strchr(s, '/');
    if (p && p[1] == '/' && p[2] == '/' && (!slash || p < slash)) {
        int l = min((int)(p - s), 7);
        for (int i = 0; i < l; i++) u.scheme[i] = to_lower(s[i]);
        u.scheme[l] = 0;
        s = p + 3;
    } else strlcpy(u.scheme, "http", sizeof(u.scheme));
    if (strcmp(u.scheme, "http") && strcmp(u.scheme, "https")) return false;
    u.port = !strcmp(u.scheme, "https") ? 443 : 80;
    int h = 0;
    while (*s && *s != '/' && *s != ':' && *s != '?' && *s != '#' && h < 127) u.host[h++] = to_lower(*s++);
    u.host[h] = 0;
    if (!h) return false;
    if (*s == ':') { s++; u.port = (u16)atoi(s); while (*s >= '0' && *s <= '9') s++; }
    if (*s != '/') { u.path[0] = '/'; strlcpy(u.path + 1, s, sizeof(u.path) - 1); }
    else strlcpy(u.path, s, sizeof(u.path));
    char* frag = strchr(u.path, '#');
    if (frag) *frag = 0;
    // spaces are not allowed on the wire
    char tmp[512];
    int o = 0;
    for (char* c = u.path; *c && o < 505; c++) {
        if (*c == ' ') { tmp[o++] = '%'; tmp[o++] = '2'; tmp[o++] = '0'; }
        else tmp[o++] = *c;
    }
    tmp[o] = 0;
    strlcpy(u.path, tmp, sizeof(u.path));
    return true;
}

static void normalize_path(char* p) {
    // collapse /./ and /x/../
    char out[512];
    int o = 0;
    const char* s = p;
    char* q = strchr(p, '?');
    int plen = q ? (int)(q - p) : (int)strlen(p);
    while (s < p + plen) {
        if (s[0] == '/' && s[1] == '.' && (s[2] == '/' || s + 2 == p + plen)) { s += 2; continue; }
        if (s[0] == '/' && s[1] == '.' && s[2] == '.' && (s[3] == '/' || s + 3 == p + plen)) {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            s += 3;
            continue;
        }
        out[o++] = *s++;
    }
    if (!o) out[o++] = '/';
    out[o] = 0;
    if (q) strlcat(out, q, sizeof(out));
    strlcpy(p, out, 512);
}

void url_resolve(const char* base, const char* rel, char* out, int n) {
    while (*rel == ' ') rel++;
    if (strchr(rel, ':') && (str_ieq_prefix(rel, "http:") || str_ieq_prefix(rel, "https:") ||
                             str_ieq_prefix(rel, "mailto:") || str_ieq_prefix(rel, "javascript:") ||
                             str_ieq_prefix(rel, "silicon:") || str_ieq_prefix(rel, "file:"))) {
        strlcpy(out, rel, n);
        return;
    }
    Url b;
    if (!url_parse(base, b)) { strlcpy(out, rel, n); return; }
    char port[16] = "";
    if ((!strcmp(b.scheme, "http") && b.port != 80) || (!strcmp(b.scheme, "https") && b.port != 443)) snprintf(port, sizeof(port), ":%u", b.port);
    if (rel[0] == '/' && rel[1] == '/') { snprintf(out, n, "%s:%s", b.scheme, rel); return; }
    char path[512];
    if (rel[0] == '/') strlcpy(path, rel, sizeof(path));
    else if (rel[0] == '?') {
        strlcpy(path, b.path, sizeof(path));
        char* q = strchr(path, '?');
        if (q) *q = 0;
        strlcat(path, rel, sizeof(path));
    } else if (rel[0] == '#' || !rel[0]) strlcpy(path, b.path, sizeof(path));
    else {
        strlcpy(path, b.path, sizeof(path));
        char* q = strchr(path, '?');
        if (q) *q = 0;
        char* last = strrchr(path, '/');
        if (last) last[1] = 0; else strlcpy(path, "/", sizeof(path));
        strlcat(path, rel, sizeof(path));
    }
    char* frag = strchr(path, '#');
    if (frag) *frag = 0;
    normalize_path(path);
    snprintf(out, n, "%s://%s%s%s", b.scheme, b.host, port, path);
}

void url_encode(const char* in, char* out, int n) {
    int o = 0;
    for (; *in && o < n - 4; in++) {
        u8 c = (u8)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = c;
        else if (c == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = "0123456789ABCDEF"[c >> 4]; out[o++] = "0123456789ABCDEF"[c & 15]; }
    }
    out[o] = 0;
}

// ================================================================ requests
struct HttpRequest {
    char url[600];
    Url u;
    char method[8];
    char* req_body;
    char req_ctype[64];
    int phase;
    char err[128];
    ip4 ip;
    int sock;
    TlsConn* tls;
    u64 phase_t, last_data;
    u8* buf; u32 len, cap;
    bool headers_done;
    u32 hdr_end;
    int status;
    char ctype[96];
    char location[600];
    bool chunked, has_len;
    u32 content_length;
    u8* body; u32 body_len;
    int redirects;
    char accept[96];
    char tlsv[12];
};

static void set_phase(HttpRequest* r, int p) { r->phase = p; r->phase_t = ticks(); }

static void fail(HttpRequest* r, const char* msg) {
    strlcpy(r->err, msg, sizeof(r->err));
    set_phase(r, HTTP_ERROR);
    if (r->tls) { tls_free(r->tls); r->tls = nullptr; }
    if (r->sock >= 0) { tcp_abort(r->sock); r->sock = -1; }
}

static bool start(HttpRequest* r, const char* url) {
    strlcpy(r->url, url, sizeof(r->url));
    if (!url_parse(url, r->u)) { fail(r, "that address doesn't look like a web address"); return false; }
    r->sock = -1;
    r->tls = nullptr;
    r->len = 0;
    r->headers_done = false;
    r->status = 0;
    r->ctype[0] = r->location[0] = 0;
    r->chunked = r->has_len = false;
    r->content_length = 0;
    if (r->body) { kfree(r->body); r->body = nullptr; }
    r->body_len = 0;
    set_phase(r, HTTP_RESOLVING);
    return true;
}

HttpRequest* http_get(const char* url, const char* method, const char* body, const char* ctype) {
    HttpRequest* r = new HttpRequest();
    strlcpy(r->method, method ? method : "GET", sizeof(r->method));
    r->req_body = body ? strdup(body) : nullptr;
    strlcpy(r->req_ctype, ctype ? ctype : "application/x-www-form-urlencoded", sizeof(r->req_ctype));
    r->cap = 64 * 1024;
    r->buf = (u8*)kmalloc(r->cap);
    r->sock = -1;
    strlcpy(r->accept, "text/html,text/plain,*/*", sizeof(r->accept));
    start(r, url);
    return r;
}

static const char* find_header(HttpRequest* r, const char* name) {
    static char val[600];
    const char* h = (const char*)r->buf;
    const char* end = h + r->hdr_end;
    int nl = (int)strlen(name);
    const char* line = strchr(h, '\n');
    while (line && line < end) {
        line++;
        if (str_ieq_prefix(line, name) && line[nl] == ':') {
            const char* v = line + nl + 1;
            while (*v == ' ') v++;
            int k = 0;
            while (v < end && *v != '\r' && *v != '\n' && k < 599) val[k++] = *v++;
            val[k] = 0;
            return val;
        }
        line = strchr(line, '\n');
    }
    return nullptr;
}

static void parse_headers(HttpRequest* r) {
    for (u32 i = 3; i < r->len; i++)
        if (r->buf[i - 3] == '\r' && r->buf[i - 2] == '\n' && r->buf[i - 1] == '\r' && r->buf[i] == '\n') {
            r->hdr_end = i + 1;
            r->headers_done = true;
            break;
        }
    if (!r->headers_done) return;
    char save = r->buf[r->hdr_end - 1];
    r->buf[r->hdr_end - 1] = 0;
    const char* sp = strchr((const char*)r->buf, ' ');
    r->status = sp ? atoi(sp + 1) : 0;
    const char* v;
    if ((v = find_header(r, "content-type"))) strlcpy(r->ctype, v, sizeof(r->ctype));
    if ((v = find_header(r, "location"))) strlcpy(r->location, v, sizeof(r->location));
    if ((v = find_header(r, "transfer-encoding")) && str_icontains(v, "chunked")) r->chunked = true;
    if ((v = find_header(r, "content-length"))) { r->has_len = true; r->content_length = (u32)atoi(v); }
    r->buf[r->hdr_end - 1] = save;
}

// is a chunked body complete? (looks for the terminating 0-size chunk)
static bool chunked_complete(HttpRequest* r) {
    u32 o = r->hdr_end;
    while (o < r->len) {
        u32 size = 0;
        bool any = false;
        while (o < r->len) {
            char c = to_lower((char)r->buf[o]);
            if (c >= '0' && c <= '9') size = size * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') size = size * 16 + (c - 'a' + 10);
            else break;
            o++;
            any = true;
        }
        if (!any) return false;
        while (o < r->len && r->buf[o] != '\n') o++;
        o++;
        if (size == 0) return true;
        o += size + 2;
    }
    return false;
}

static void decode_body(HttpRequest* r) {
    u32 blen = r->len - r->hdr_end;
    r->body = (u8*)kmalloc(blen + 1);
    if (!r->chunked) {
        memcpy(r->body, r->buf + r->hdr_end, blen);
        r->body_len = r->has_len ? min(blen, r->content_length) : blen;
    } else {
        u32 o = r->hdr_end, out = 0;
        while (o < r->len) {
            u32 size = 0;
            while (o < r->len) {
                char c = to_lower((char)r->buf[o]);
                if (c >= '0' && c <= '9') size = size * 16 + (c - '0');
                else if (c >= 'a' && c <= 'f') size = size * 16 + (c - 'a' + 10);
                else break;
                o++;
            }
            while (o < r->len && r->buf[o] != '\n') o++;
            o++;
            if (!size) break;
            u32 n = min(size, r->len - min(o, r->len));
            memcpy(r->body + out, r->buf + o, n);
            out += n;
            o += size + 2;
        }
        r->body_len = out;
    }
    r->body[r->body_len] = 0;
}

int http_step(HttpRequest* r) {
    u64 t = ticks();
    switch (r->phase) {
    case HTTP_RESOLVING: {
        if (!net_link_up()) {
            if (t - r->phase_t > 6000) fail(r, "no network connection (is a network card attached?)");
            break;
        }
        int st = dns_resolve(r->u.host, &r->ip);
        if (st < 0) { char m[200]; snprintf(m, sizeof(m), "couldn't find %s (DNS lookup failed)", r->u.host); fail(r, m); }
        else if (st > 0) {
            r->sock = tcp_connect(r->ip, r->u.port);
            if (r->sock < 0) fail(r, "out of sockets");
            else set_phase(r, HTTP_CONNECTING);
        } else if (t - r->phase_t > 8000) fail(r, "DNS lookup timed out");
        break;
    }
    case HTTP_CONNECTING:
        if (tcp_failed(r->sock)) { fail(r, "connection refused or timed out"); break; }
        if (tcp_state(r->sock) == TCP_ESTABLISHED) {
            if (!strcmp(r->u.scheme, "https")) { r->tls = tls_new(r->sock, r->u.host); set_phase(r, HTTP_HANDSHAKE); }
            else set_phase(r, HTTP_SENDING);
        } else if (t - r->phase_t > 12000) fail(r, "connection timed out");
        break;
    case HTTP_HANDSHAKE: {
        int st = tls_handshake(r->tls);
        if (st < 0) { char m[128]; snprintf(m, sizeof(m), "secure connection failed: %s", tls_error(r->tls)); fail(r, m); }
        else if (st > 0) set_phase(r, HTTP_SENDING);
        else if (t - r->phase_t > 15000) fail(r, "secure connection timed out");
        break;
    }
    case HTTP_SENDING: {
        char req[1400];
        char port[12] = "";
        if ((!strcmp(r->u.scheme, "http") && r->u.port != 80) || (!strcmp(r->u.scheme, "https") && r->u.port != 443))
            snprintf(port, sizeof(port), ":%u", r->u.port);
        int n = snprintf(req, sizeof(req),
                         "%s %s HTTP/1.1\r\nHost: %s%s\r\nUser-Agent: Mozilla/5.0 (silicon; x86_64) Beam/0.2\r\n"
                         "Accept: %s\r\nAccept-Encoding: identity\r\nConnection: close\r\n",
                         r->method, r->u.path, r->u.host, port, r->accept);
        if (r->req_body) n += snprintf(req + n, sizeof(req) - n, "Content-Type: %s\r\nContent-Length: %u\r\n", r->req_ctype, (u32)strlen(r->req_body));
        n += snprintf(req + n, sizeof(req) - n, "\r\n");
        bool ok = r->tls ? tls_send(r->tls, req, n) : tcp_send(r->sock, req, n);
        if (ok && r->req_body) ok = r->tls ? tls_send(r->tls, r->req_body, strlen(r->req_body)) : tcp_send(r->sock, r->req_body, strlen(r->req_body));
        if (!ok) { fail(r, "couldn't send the request"); break; }
        r->last_data = t;
        set_phase(r, HTTP_RECEIVING);
        break;
    }
    case HTTP_RECEIVING: {
        bool got = false;
        for (int k = 0; k < 64; k++) {
            if (r->len + 4096 > r->cap) {
                if (r->cap >= 8 * 1024 * 1024) break;
                r->cap *= 2;
                r->buf = (u8*)krealloc(r->buf, r->cap);
            }
            u32 n = r->tls ? tls_recv(r->tls, r->buf + r->len, r->cap - r->len - 1)
                           : tcp_recv(r->sock, r->buf + r->len, r->cap - r->len - 1);
            if (!n) break;
            r->len += n;
            got = true;
        }
        if (got) r->last_data = t;
        if (!r->headers_done) parse_headers(r);
        bool closed = r->tls ? tls_closed(r->tls) : (tcp_peer_closed(r->sock) || tcp_failed(r->sock));
        bool complete = false;
        if (r->headers_done) {
            if (r->chunked) complete = chunked_complete(r);
            else if (r->has_len) complete = r->len - r->hdr_end >= r->content_length;
            if (r->status == 204 || r->status == 304 || !strcmp(r->method, "HEAD")) complete = true;
        }
        if (complete || closed) {
            if (!r->headers_done) { fail(r, r->len ? "the server sent something that isn't HTTP" : "the server closed the connection without answering"); break; }
            decode_body(r);
            if (r->tls) { strlcpy(r->tlsv, tls_version(r->tls), sizeof(r->tlsv)); tls_free(r->tls); r->tls = nullptr; }
            else r->tlsv[0] = 0;
            tcp_close(r->sock);
            r->sock = -1;
            // follow redirects
            if (r->status >= 300 && r->status < 400 && r->location[0] && r->redirects < 6) {
                char next[600];
                url_resolve(r->url, r->location, next, sizeof(next));
                r->redirects++;
                if (r->status == 303 || r->status == 301 || r->status == 302) {
                    strlcpy(r->method, "GET", sizeof(r->method));
                    if (r->req_body) { kfree(r->req_body); r->req_body = nullptr; }
                }
                start(r, next);
                break;
            }
            set_phase(r, HTTP_DONE);
        } else if (t - r->last_data > 20000) fail(r, "the server stopped answering");
        break;
    }
    }
    return r->phase;
}

int http_status(HttpRequest* r) { return r->status; }
void http_set_accept(HttpRequest* r, const char* accept) { strlcpy(r->accept, accept, sizeof(r->accept)); }
const char* http_error(HttpRequest* r) { return r->err; }
const char* http_final_url(HttpRequest* r) { return r->url; }
const char* http_content_type(HttpRequest* r) { return r->ctype; }
const char* http_tls_version(HttpRequest* r) { return r->tlsv; }
const char* http_header(HttpRequest* r, const char* name) { return r->headers_done ? find_header(r, name) : nullptr; }
const u8* http_body(HttpRequest* r, u32* len) { *len = r->body_len; return r->body; }
u32 http_received(HttpRequest* r) { return r->headers_done ? r->len - r->hdr_end : r->len; }
u32 http_expected(HttpRequest* r) { return r->has_len ? r->content_length : 0; }

void http_free(HttpRequest* r) {
    if (!r) return;
    if (r->tls) tls_free(r->tls);
    if (r->sock >= 0) tcp_abort(r->sock);
    if (r->buf) kfree(r->buf);
    if (r->body) kfree(r->body);
    if (r->req_body) kfree(r->req_body);
    delete r;
}
