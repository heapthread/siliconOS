// silicon - Mail: reads mail over POP3 and sends it over SMTP (plain or TLS). One account, set up in the app.
// The account (including the password, unencrypted) is kept in /home/.config/mail.
#include "apps.h"
#include "../../net/net.h"

static const char* strstr_(const char* h, const char* n) {
    int nl = (int)strlen(n);
    for (; *h; h++) if (!memcmp(h, n, nl)) return h;
    return nullptr;
}

// ---------------------------------------------------------------- base64, quoted-printable, encoded words
static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_encode(const char* in, int n, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < n && o + 5 < cap; i += 3) {
        u32 v = (u8)in[i] << 16 | (i + 1 < n ? (u8)in[i + 1] << 8 : 0) | (i + 2 < n ? (u8)in[i + 2] : 0);
        out[o++] = B64[(v >> 18) & 63]; out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
}
static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}
// decodes into out (NUL-terminated), returns the length
static int b64_decode(const char* in, int n, char* out, int cap) {
    int o = 0;
    u32 acc = 0;
    int bits = 0;
    for (int i = 0; i < n && o + 2 < cap; i++) {
        int v = b64_val(in[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (u32)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (char)((acc >> bits) & 255); }
    }
    out[o] = 0;
    return o;
}
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int qp_decode(const char* in, int n, char* out, int cap, bool header) {
    int o = 0;
    for (int i = 0; i < n && o + 2 < cap; i++) {
        if (in[i] == '=' && i + 2 < n + 0 && hexv(in[i + 1]) >= 0 && hexv(in[i + 2]) >= 0) { out[o++] = (char)(hexv(in[i + 1]) * 16 + hexv(in[i + 2])); i += 2; }
        else if (in[i] == '=' && i + 1 < n && (in[i + 1] == '\r' || in[i + 1] == '\n')) { i += in[i + 1] == '\r' && i + 2 < n && in[i + 2] == '\n' ? 2 : 1; }
        else if (header && in[i] == '_') out[o++] = ' ';
        else out[o++] = in[i];
    }
    out[o] = 0;
    return o;
}
// "=?utf-8?B?...?=" words in a header value, and the result converted to silicon's codepage
static void decode_header(const char* in, char* out, int cap) {
    char* tmp = (char*)kmalloc(strlen(in) * 2 + 8);
    int o = 0;
    for (const char* p = in; *p && o < (int)strlen(in) * 2;) {
        if (p[0] == '=' && p[1] == '?') {
            const char* q1 = strchr(p + 2, '?');
            const char* q2 = q1 ? strchr(q1 + 1, '?') : nullptr;
            const char* end = q2 ? strstr_(q2 + 1, "?=") : nullptr;
            if (q2 && end) {
                char enc = q1[1];
                char* dec = (char*)kmalloc((end - q2) * 2 + 8);
                int dl = (enc == 'B' || enc == 'b') ? b64_decode(q2 + 1, (int)(end - q2 - 1), dec, (int)(end - q2) * 2) : qp_decode(q2 + 1, (int)(end - q2 - 1), dec, (int)(end - q2) * 2, true);
                for (int i = 0; i < dl; i++) tmp[o++] = dec[i];
                kfree(dec);
                p = end + 2;
                while (*p == ' ' && p[1] == '=' && p[2] == '?') p++;      // whitespace between encoded words is dropped
                continue;
            }
        }
        tmp[o++] = *p++;
    }
    tmp[o] = 0;
    utf8_to_silicon(tmp, o, out, cap);
    kfree(tmp);
}

// ---------------------------------------------------------------- a scripted line protocol session (POP3 or SMTP)
struct Session {
    enum { RESOLVE, CONNECT, HANDSHAKE, RUN, DONE, FAILED };
    int st = RESOLVE;
    char host[128] = "";
    u16 port = 0;
    bool use_tls = false, smtp = false;
    ip4 ip = 0;
    int sock = -1;
    TlsConn* tls = nullptr;
    u64 t0 = 0, last = 0;
    char* rx = nullptr;
    int rxn = 0, rxcap = 0;
    struct Step { char cmd[400]; int mode; char* resp; };   // mode: 0 single reply, 1 POP multi-line reply; cmd "" = just wait
    Vec<Step> steps;
    int cur = 0;
    bool sent = false;
    char err[140] = "";

    ~Session() {
        for (int i = 0; i < steps.len; i++) if (steps[i].resp) kfree(steps[i].resp);
        steps.free();
        if (tls) tls_free(tls);
        if (sock >= 0) tcp_abort(sock);
        if (rx) kfree(rx);
    }
    void add(const char* cmd, int mode = 0) { Step s; strlcpy(s.cmd, cmd, sizeof(s.cmd)); s.mode = mode; s.resp = nullptr; steps.push(s); }
    void start(const char* h, int p, bool t, bool is_smtp) {
        strlcpy(host, h, sizeof(host)); port = (u16)p; use_tls = t; smtp = is_smtp;
        t0 = last = ticks();
        rxcap = 64 * 1024;
        rx = (char*)kmalloc(rxcap);
        rx[0] = 0;
        dns_flush();
    }
    void fail(const char* m) { strlcpy(err, m, sizeof(err)); st = FAILED; }
    u32 recv(char* buf, u32 n) { return tls ? tls_recv(tls, buf, n) : tcp_recv(sock, buf, n); }
    bool send_raw(const char* s, u32 n) { return tls ? tls_send(tls, s, n) : tcp_send(sock, s, n); }

    // is the reply to the current step complete?
    bool reply_done(const Step& s) {
        if (!rxn) return false;
        if (smtp) {
            // multi-line: every line starts with "NNN-" except the last, "NNN "
            const char* p = rx;
            while (*p) {
                const char* e = strchr(p, '\n');
                if (!e) return false;
                if (e - p >= 4 && p[3] == ' ') return true;
                if (e - p < 4) return true;
                p = e + 1;
            }
            return false;
        }
        if (s.mode == 1 && rxn >= 3 && rx[0] == '+') return rxn >= 5 && !memcmp(rx + rxn - 5, "\r\n.\r\n", 5);
        return strchr(rx, '\n') != nullptr;
    }
    bool reply_ok(const Step& s) { return smtp ? (rx[0] == '2' || rx[0] == '3') : rx[0] == '+'; (void)s; }

    // returns true while still working
    bool tick() {
        u64 now = ticks();
        if (st == DONE || st == FAILED) return false;
        if (now - last > 25000) {
            fail(st == RESOLVE ? "couldn't look up the server's address" : st == CONNECT ? "couldn't connect to the server" : st == HANDSHAKE ? "the secure handshake never finished" : "the server stopped answering");
            return false;
        }
        switch (st) {
        case RESOLVE: {
            if (!net_link_up()) { fail("no network connection"); return false; }
            int r = dns_resolve(host, &ip);
            if (r < 0) { char m[200]; snprintf(m, sizeof(m), "can't find %s", host); fail(m); }
            else if (r > 0) {
                sock = tcp_connect(ip, port);
                if (sock < 0) fail("out of sockets");
                else { st = CONNECT; last = now; }
            }
            break;
        }
        case CONNECT:
            if (tcp_failed(sock)) { fail("connection refused or timed out"); break; }
            if (tcp_state(sock) == TCP_ESTABLISHED) {
                last = now;
                if (use_tls) { tls = tls_new(sock, host); st = HANDSHAKE; }
                else st = RUN;
            }
            break;
        case HANDSHAKE: {
            int r = tls_handshake(tls);
            if (r < 0) { char m[140]; snprintf(m, sizeof(m), "secure connection failed: %s", tls_error(tls)); fail(m); }
            else if (r > 0) { st = RUN; last = now; }
            break;
        }
        case RUN: {
            if (cur >= steps.len) { st = DONE; break; }
            Step& s = steps[cur];
            if (!sent) {
                if (s.cmd[0]) {
                    char line[420];
                    int n = snprintf(line, sizeof(line), "%s\r\n", s.cmd);
                    if (!send_raw(line, (u32)n)) { fail("couldn't send to the server"); break; }
                }
                sent = true;
                rxn = 0; rx[0] = 0;
                last = now;
            }
            for (int k = 0; k < 32; k++) {
                if (rxn + 4096 > rxcap) { if (rxcap >= 4 * 1024 * 1024) break; rxcap *= 2; rx = (char*)krealloc(rx, rxcap); }
                u32 n = recv(rx + rxn, (u32)(rxcap - rxn - 1));
                if (!n) break;
                rxn += (int)n; rx[rxn] = 0;
                last = now;
            }
            if (reply_done(s)) {
                if (!reply_ok(s)) {
                    char m[160];
                    int l = 0;
                    while (rx[l] && rx[l] != '\r' && rx[l] != '\n' && l < 150) l++;
                    memcpy(m, rx, l); m[l] = 0;
                    char b[170];
                    snprintf(b, sizeof(b), "server: %s", m);
                    fail(b);
                    break;
                }
                s.resp = (char*)kmalloc(rxn + 1);
                memcpy(s.resp, rx, rxn + 1);
                cur++;
                sent = false;
                rxn = 0;
            } else if (tls ? tls_closed(tls) : (tcp_peer_closed(sock) || tcp_failed(sock))) fail("the server closed the connection");
            break;
        }
        }
        return st != DONE && st != FAILED;
    }
};

// ---------------------------------------------------------------- the app
struct MailApp : App {
    struct Account {
        char name[48], addr[96], user[96], pass[96];
        char pop_host[96], smtp_host[96];
        int pop_port, smtp_port;
        bool pop_tls, smtp_tls;
    } acct;
    struct Msg { char from[80], subject[100], date[40]; char* body; int number; bool full; };
    Vec<Msg> msgs;
    int sel = -1;
    Smooth sc, bsc;
    ScrollBar sb;
    Session* sess = nullptr;
    int purpose = 0;                      // 1 refresh, 2 read a message, 3 send
    int want_msg = -1;
    char status[140] = "";
    int mode = 0;                         // 0 inbox, 1 account setup, 2 compose
    TextField f_name, f_addr, f_user, f_pass, f_pop, f_popport, f_smtp, f_smtpport;
    TextField c_to, c_subject;
    Vec<char> c_body;
    int focus = 0;
    int cw = 0, ch = 0;
    static const int BAR = 36, LISTW = 270, ROWH = 40;

    MailApp() {
        memset(&acct, 0, sizeof(acct));
        acct.pop_port = 995; acct.smtp_port = 465; acct.pop_tls = acct.smtp_tls = true;
        load();
        if (!acct.pop_host[0]) { mode = 1; snprintf(status, sizeof(status), "set up your account to get started"); }
        else refresh();
        c_pass_init();
    }
    ~MailApp() {
        if (sess) delete sess;
        for (int i = 0; i < msgs.len; i++) if (msgs[i].body) kfree(msgs[i].body);
        msgs.free(); c_body.free();
    }
    void c_pass_init() { f_pass.password = true; }
    const char* app_id() override { return "mail"; }
    const char* title() override { return "Mail"; }
    int pref_w() override { return 760; }
    int pref_h() override { return 500; }

    // ---- account file
    void load() {
        FsNode* f = fs_lookup("/home/.config/mail");
        if (!f) return;
        const char* t = fs_read_text(f);
        while (*t) {
            const char* e = strchr(t, '\n');
            int l = e ? (int)(e - t) : (int)strlen(t);
            char line[200];
            memcpy(line, t, min(l, 199));
            line[min(l, 199)] = 0;
            char* eq = strchr(line, '=');
            if (eq) {
                *eq = 0;
                const char* v = eq + 1;
                if (!strcmp(line, "name")) strlcpy(acct.name, v, sizeof(acct.name));
                else if (!strcmp(line, "addr")) strlcpy(acct.addr, v, sizeof(acct.addr));
                else if (!strcmp(line, "user")) strlcpy(acct.user, v, sizeof(acct.user));
                else if (!strcmp(line, "pass")) strlcpy(acct.pass, v, sizeof(acct.pass));
                else if (!strcmp(line, "pop")) strlcpy(acct.pop_host, v, sizeof(acct.pop_host));
                else if (!strcmp(line, "smtp")) strlcpy(acct.smtp_host, v, sizeof(acct.smtp_host));
                else if (!strcmp(line, "pop_port")) acct.pop_port = atoi(v);
                else if (!strcmp(line, "smtp_port")) acct.smtp_port = atoi(v);
                else if (!strcmp(line, "pop_tls")) acct.pop_tls = atoi(v) != 0;
                else if (!strcmp(line, "smtp_tls")) acct.smtp_tls = atoi(v) != 0;
            }
            if (!e) break;
            t = e + 1;
        }
    }
    void save_account() {
        char b[900];
        snprintf(b, sizeof(b), "name=%s\naddr=%s\nuser=%s\npass=%s\npop=%s\npop_port=%d\npop_tls=%d\nsmtp=%s\nsmtp_port=%d\nsmtp_tls=%d\n",
                 acct.name, acct.addr, acct.user, acct.pass, acct.pop_host, acct.pop_port, acct.pop_tls, acct.smtp_host, acct.smtp_port, acct.smtp_tls);
        fs_mkdirs("/home/.config");
        fs_write_text("/home/.config/mail", b);
    }
    void setup_open() {
        mode = 1; focus = 0;
        f_name.set(acct.name); f_addr.set(acct.addr); f_user.set(acct.user); f_pass.set(acct.pass);
        f_pop.set(acct.pop_host); f_smtp.set(acct.smtp_host);
        char b[12];
        snprintf(b, sizeof(b), "%d", acct.pop_port); f_popport.set(b);
        snprintf(b, sizeof(b), "%d", acct.smtp_port); f_smtpport.set(b);
    }
    void setup_save() {
        strlcpy(acct.name, f_name.text, sizeof(acct.name)); strlcpy(acct.addr, f_addr.text, sizeof(acct.addr));
        strlcpy(acct.user, f_user.len ? f_user.text : f_addr.text, sizeof(acct.user)); strlcpy(acct.pass, f_pass.text, sizeof(acct.pass));
        strlcpy(acct.pop_host, f_pop.text, sizeof(acct.pop_host)); strlcpy(acct.smtp_host, f_smtp.text, sizeof(acct.smtp_host));
        acct.pop_port = atoi(f_popport.text); acct.smtp_port = atoi(f_smtpport.text);
        save_account();
        mode = 0;
        refresh();
    }

    // ---- mail transfers
    void login_steps(Session* s) {
        s->add("");
        char c[200];
        snprintf(c, sizeof(c), "USER %s", acct.user); s->add(c);
        snprintf(c, sizeof(c), "PASS %s", acct.pass); s->add(c);
    }
    void refresh() {
        if (sess || !acct.pop_host[0]) return;
        sess = new Session();
        login_steps(sess);
        sess->add("STAT");
        sess->start(acct.pop_host, acct.pop_port, acct.pop_tls, false);
        purpose = 1;
        snprintf(status, sizeof(status), "checking mail" G_ELLIP);
    }
    void fetch_headers(int total) {
        // second connection: headers and a bit of the body of the newest messages
        delete sess;
        sess = new Session();
        login_steps(sess);
        int first = max(1, total - 19);
        for (int i = total; i >= first; i--) { char c[40]; snprintf(c, sizeof(c), "TOP %d 25", i); sess->add(c, 1); }
        sess->add("QUIT");
        sess->start(acct.pop_host, acct.pop_port, acct.pop_tls, false);
        purpose = 4;
        fetch_total = total;
    }
    int fetch_total = 0;
    void open_message(int i) {
        sel = i;
        bsc.jump(0);
        if (i < 0 || i >= msgs.len || msgs[i].full || sess) { ui_invalidate(); return; }
        sess = new Session();
        login_steps(sess);
        char c[40];
        snprintf(c, sizeof(c), "RETR %d", msgs[i].number);
        sess->add(c, 1);
        sess->add("QUIT");
        sess->start(acct.pop_host, acct.pop_port, acct.pop_tls, false);
        purpose = 2; want_msg = i;
        snprintf(status, sizeof(status), "downloading the message" G_ELLIP);
    }
    void send_mail() {
        if (sess) return;
        if (!c_to.len || !acct.smtp_host[0]) { snprintf(status, sizeof(status), "enter a recipient (and set up the account)"); return; }
        sess = new Session();
        sess->add("");
        sess->add("EHLO silicon");
        char c[300], b[300];
        b64_encode(acct.user, (int)strlen(acct.user), b, sizeof(b));
        sess->add("AUTH LOGIN"); sess->add(b);
        b64_encode(acct.pass, (int)strlen(acct.pass), b, sizeof(b));
        sess->add(b);
        snprintf(c, sizeof(c), "MAIL FROM:<%s>", acct.addr); sess->add(c);
        snprintf(c, sizeof(c), "RCPT TO:<%s>", c_to.text); sess->add(c);
        sess->add("DATA");
        // the message: headers, blank line, body with dot-stuffing; silicon text goes out as UTF-8
        Vec<char> m;
        char hdr[500];
        char subj_u[300];
        silicon_to_utf8(c_subject.text, c_subject.len, subj_u, sizeof(subj_u));
        char subj_b[400];
        b64_encode(subj_u, (int)strlen(subj_u), subj_b, sizeof(subj_b));
        int n = snprintf(hdr, sizeof(hdr), "From: %s <%s>\r\nTo: %s\r\nSubject: =?utf-8?B?%s?=\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: 8bit\r\nX-Mailer: silicon Mail\r\n\r\n",
                         acct.name[0] ? acct.name : acct.addr, acct.addr, c_to.text, subj_b);
        for (int i = 0; i < n; i++) m.push(hdr[i]);
        char* body = (char*)kmalloc(c_body.len + 1);
        for (int i = 0; i < c_body.len; i++) body[i] = c_body[i];
        body[c_body.len] = 0;
        char* ub = (char*)kmalloc(c_body.len * 4 + 8);
        silicon_to_utf8(body, c_body.len, ub, c_body.len * 4 + 8);
        bool line_start = true;
        for (const char* p = ub; *p; p++) {
            if (line_start && *p == '.') m.push('.');
            if (*p == '\n') { m.push('\r'); m.push('\n'); line_start = true; }
            else { m.push(*p); line_start = false; }
        }
        if (!line_start) { m.push('\r'); m.push('\n'); }
        m.push('.');
        kfree(body); kfree(ub);
        m.push(0);
        // the step command is capped at 400 bytes, so the data goes through a dedicated raw send
        outgoing = (char*)kmalloc(m.len);
        memcpy(outgoing, m.data, m.len);
        m.free();
        sess->add("<<DATA>>");
        sess->add("QUIT");
        sess->start(acct.smtp_host, acct.smtp_port, acct.smtp_tls, true);
        purpose = 3;
        snprintf(status, sizeof(status), "sending" G_ELLIP);
    }
    char* outgoing = nullptr;

    // ---- parsing
    static void header_value(const char* hdrs, const char* name, char* out, int cap) {
        out[0] = 0;
        int nl = (int)strlen(name);
        for (const char* p = hdrs; *p;) {
            const char* e = strchr(p, '\n');
            int l = e ? (int)(e - p) : (int)strlen(p);
            if (l > nl && str_ieq_prefix(p, name) && p[nl] == ':') {
                int o = 0;
                const char* v = p + nl + 1;
                while (*v == ' ') v++;
                // unfold continuation lines
                const char* q = v;
                for (;;) {
                    while (q < p + l && *q != '\r' && o < cap - 1) out[o++] = *q++;
                    if (!e) break;
                    const char* nx = e + 1;
                    if (*nx == ' ' || *nx == '\t') { out[o++] = ' '; while (*nx == ' ' || *nx == '\t') nx++; q = nx; e = strchr(nx, '\n'); p = nx; l = e ? (int)(e - nx) : (int)strlen(nx); continue; }
                    break;
                }
                out[o] = 0;
                return;
            }
            if (!e) break;
            p = e + 1;
        }
    }
    // makes a Msg out of a POP3 reply ("+OK ...\r\n<message>\r\n.\r\n")
    void parse_message(const char* reply, int number, bool full, Msg* m) {
        const char* p = strchr(reply, '\n');
        p = p ? p + 1 : reply;
        const char* split = nullptr;
        for (const char* q = p; *q; q++) if (q[0] == '\n' && (q[1] == '\n' || (q[1] == '\r' && q[2] == '\n'))) { split = q; break; }
        int hl = split ? (int)(split - p) : (int)strlen(p);
        char* hdrs = (char*)kmalloc(hl + 2);
        memcpy(hdrs, p, hl); hdrs[hl] = 0;
        char v[300];
        header_value(hdrs, "From", v, sizeof(v)); decode_header(v, m->from, sizeof(m->from));
        header_value(hdrs, "Subject", v, sizeof(v)); decode_header(v, m->subject, sizeof(m->subject));
        header_value(hdrs, "Date", m->date, sizeof(m->date));
        char cte[40], ctype[200];
        header_value(hdrs, "Content-Transfer-Encoding", cte, sizeof(cte));
        header_value(hdrs, "Content-Type", ctype, sizeof(ctype));
        kfree(hdrs);
        m->number = number;
        m->full = full;
        const char* body = split ? split + 1 : "";
        while (*body == '\r' || *body == '\n') { if (*body == '\n') { body++; break; } body++; }
        // multipart: use the first text/plain part
        const char* b = body;
        if (str_icontains(ctype, "multipart")) {
            const char* bd = strstr_(ctype, "boundary=");
            char boundary[100] = "";
            if (bd) {
                bd += 9;
                int o = 0;
                if (*bd == '"') bd++;
                while (*bd && *bd != '"' && *bd != ';' && o < 98) boundary[o++] = *bd++;
                boundary[o] = 0;
            }
            if (boundary[0]) {
                const char* part = body;
                while ((part = strstr_(part, boundary)) != nullptr) {
                    part += strlen(boundary);
                    const char* ph_end = nullptr;
                    for (const char* q = part; *q; q++) if (q[0] == '\n' && (q[1] == '\n' || (q[1] == '\r' && q[2] == '\n'))) { ph_end = q; break; }
                    if (!ph_end) break;
                    char save = *ph_end;
                    (void)save;
                    int phl = (int)(ph_end - part);
                    char* ph = (char*)kmalloc(phl + 2);
                    memcpy(ph, part, phl); ph[phl] = 0;
                    char pct[100];
                    header_value(ph, "Content-Type", pct, sizeof(pct));
                    bool text = str_icontains(pct, "text/plain") || !pct[0];
                    if (text) header_value(ph, "Content-Transfer-Encoding", cte, sizeof(cte));
                    kfree(ph);
                    if (text) { b = ph_end + 1; while (*b == '\r' || *b == '\n') { if (*b == '\n') { b++; break; } b++; } break; }
                }
            }
        }
        // cut at the part boundary or the final dot, and strip dot-stuffing
        int bl = (int)strlen(b);
        char* raw = (char*)kmalloc(bl + 4);
        int o = 0;
        for (int i = 0; i < bl; i++) {
            if (b[i] == '-' && b[i + 1] == '-' && (i == 0 || b[i - 1] == '\n') && str_icontains(ctype, "multipart")) break;
            if (b[i] == '.' && (i == 0 || b[i - 1] == '\n') && (b[i + 1] == '\r' || b[i + 1] == '\n' || b[i + 1] == 0)) { if (b[i + 1] != '.') break; }
            if (b[i] == '.' && (i == 0 || b[i - 1] == '\n') && b[i + 1] == '.') continue;
            if (b[i] != '\r') raw[o++] = b[i];
        }
        raw[o] = 0;
        char* dec = raw;
        if (str_icontains(cte, "base64")) { dec = (char*)kmalloc(o + 8); b64_decode(raw, o, dec, o + 8); kfree(raw); }
        else if (str_icontains(cte, "quoted")) { dec = (char*)kmalloc(o + 8); qp_decode(raw, o, dec, o + 8, false); kfree(raw); }
        int dl = (int)strlen(dec);
        char* sil = (char*)kmalloc(dl * 2 + 8);
        utf8_to_silicon(dec, dl, sil, dl * 2 + 8);
        kfree(dec);
        if (m->body) kfree(m->body);
        m->body = sil;
    }

    void finish() {
        bool ok = sess->st == Session::DONE;
        if (!ok) {
            snprintf(status, sizeof(status), "%s", sess->err);
            if (purpose == 3 && outgoing) { kfree(outgoing); outgoing = nullptr; }
        } else if (purpose == 1) {
            // steps: greeting, USER, PASS, STAT
            int total = 0;
            const char* st = sess->steps[3].resp;
            if (st) { const char* sp = strchr(st, ' '); total = sp ? atoi(sp + 1) : 0; }
            if (total <= 0) {
                for (int i = 0; i < msgs.len; i++) if (msgs[i].body) kfree(msgs[i].body);
                msgs.clear();
                sel = -1;
                snprintf(status, sizeof(status), "no messages");
            } else { fetch_headers(total); ui_invalidate(); return; }
        } else if (purpose == 4) {
            for (int i = 0; i < msgs.len; i++) if (msgs[i].body) kfree(msgs[i].body);
            msgs.clear();
            int n = sess->steps.len;
            for (int i = 3; i < n - 1; i++) {
                if (!sess->steps[i].resp) continue;
                Msg m;
                memset(&m, 0, sizeof(m));
                parse_message(sess->steps[i].resp, fetch_total - (i - 3), false, &m);
                msgs.push(m);
            }
            sel = msgs.len ? 0 : -1;
            snprintf(status, sizeof(status), "%d message%s", msgs.len, msgs.len == 1 ? "" : "s");
        } else if (purpose == 2) {
            if (want_msg >= 0 && want_msg < msgs.len && sess->steps.len > 3 && sess->steps[3].resp) {
                parse_message(sess->steps[3].resp, msgs[want_msg].number, true, &msgs[want_msg]);
                status[0] = 0;
            }
        } else if (purpose == 3) {
            snprintf(status, sizeof(status), "message sent");
            ui_notify("Mail", "message sent", "mail");
            mode = 0;
            c_to.set(""); c_subject.set(""); c_body.clear();
            if (outgoing) { kfree(outgoing); outgoing = nullptr; }
        }
        delete sess;
        sess = nullptr;
        purpose = 0;
        ui_invalidate();
    }
    void tick() override {
        if (sc.step() || bsc.step()) ui_invalidate();
        if (!sess) return;
        // the SMTP message body is sent when its step comes up
        if (purpose == 3 && sess->st == Session::RUN && sess->cur < sess->steps.len && !sess->sent && !strcmp(sess->steps[sess->cur].cmd, "<<DATA>>")) {
            u32 n = (u32)strlen(outgoing);
            sess->send_raw(outgoing, n);
            sess->send_raw("\r\n.\r\n", 5);
            sess->steps[sess->cur].cmd[0] = 0;
            sess->sent = true;
            sess->rxn = 0; sess->rx[0] = 0;
            sess->last = ticks();
            kfree(outgoing); outgoing = nullptr;
        }
        bool busy = sess->tick();
        if (!busy) finish();
        else ui_invalidate();
    }

    // ---- drawing
    void draw(Canvas& c, int w, int h) override {
        cw = w; ch = h;
        c.fill(0, 0, w, BAR, th::panel);
        c.hline(0, BAR - 1, w, th::border);
        c.blit(icon("mail")->small, 10, 10);
        draw_button(c, 34, 7, 80, 22, sess && purpose == 1 ? "..." : "Refresh", hot(34, 7, 80, 22), false, !sess && acct.pop_host[0]);
        draw_button(c, 120, 7, 80, 22, "Compose", hot(120, 7, 80, 22), mode == 2, acct.smtp_host[0] != 0);
        draw_button(c, w - 100, 7, 90, 22, "Account", hot(w - 100, 7, 90, 22), mode == 1);
        if (mode == 1) { draw_setup(c, w, h); return; }
        if (mode == 2) { draw_compose(c, w, h); return; }
        // inbox
        c.fill(0, BAR, LISTW, h - BAR - 20, th::paper);
        c.vline(LISTW, BAR, h - BAR - 20, th::border);
        c.set_clip({0, BAR, LISTW, h - BAR - 20});
        for (int i = 0; i < msgs.len; i++) {
            int y = BAR + i * ROWH - sc.pos();
            if (y + ROWH < BAR || y > h) continue;
            bool on = i == sel, hv = hot(0, y, LISTW, ROWH);
            if (on) c.fill(0, y, LISTW, ROWH, blend(th::paper, accent(), 70));
            else if (hv) c.fill(0, y, LISTW, ROWH, th::panel);
            c.text_n(8, y + 6, msgs[i].subject[0] ? msgs[i].subject : "(no subject)", (LISTW - 16) / 8, th::text, 1, true);
            c.text_n(8, y + 22, msgs[i].from, (LISTW - 16) / 8, th::muted);
            c.hline(0, y + ROWH - 1, LISTW, 0xFFE0DAEE);
        }
        if (!msgs.len) c.text_centered(0, BAR + 60, LISTW, sess ? "" : "no messages", th::muted);
        c.set_clip({0, 0, w, h});
        // message
        c.set_clip({LISTW + 1, BAR, w - LISTW - 1, h - BAR - 20});
        if (sel >= 0 && sel < msgs.len) {
            Msg& m = msgs[sel];
            int x = LISTW + 14, y = BAR + 10 - bsc.pos();
            c.text_n(x, y, m.subject, (w - LISTW - 30) / 16, th::text, 2, true);
            c.text_n(x, y + 30, m.from, (w - LISTW - 30) / 8, th::muted);
            c.text_n(x, y + 44, m.date, (w - LISTW - 30) / 8, th::muted);
            c.hline(x, y + 62, w - LISTW - 30, th::border);
            int cols = max(10, (w - LISTW - 40) / 8);
            int yy = y + 72;
            for (const char* p = m.body ? m.body : ""; *p;) {
                const char* e = strchr(p, '\n');
                int l = e ? (int)(e - p) : (int)strlen(p);
                if (!l) { yy += 14; }
                while (l > 0) {
                    int take = min(l, cols);
                    if (take < l) { int k = take; while (k > 0 && p[k] != ' ') k--; if (k > cols / 3) take = k; }
                    if (yy > BAR - 14 && yy < h) c.text_n(x, yy, p, take, th::text);
                    yy += 14;
                    p += take; l -= take;
                    while (*p == ' ' && l > 0) { p++; l--; }
                }
                if (!e) break;
                p = e + 1;
            }
            body_h = yy - y + 20;
            if (m.body == nullptr) c.text(x, y + 80, "loading" G_ELLIP, th::muted);
            else if (!m.full && sess == nullptr) c.text(x, yy + 6, "(click the message again to download all of it)", th::muted);
        } else c.text_centered(LISTW, h / 2, w - LISTW, "select a message", th::muted);
        c.set_clip({0, 0, w, h});
        c.fill(0, h - 20, w, 20, th::panel);
        c.hline(0, h - 20, w, th::border);
        c.text_n(10, h - 15, status[0] ? status : acct.addr, (w - 20) / 8, status[0] && sess == nullptr && str_icontains(status, "server") ? th::red : th::muted);
    }
    int body_h = 0;
    void draw_setup(Canvas& c, int w, int h) {
        c.text(24, BAR + 14, "Account", th::text, 2, true);
        c.text(24, BAR + 40, "POP3 for receiving, SMTP for sending. Ports 995 and 465 use TLS.", th::muted);
        struct Row { const char* label; TextField* f; } rows[8] = {{"your name", &f_name}, {"email address", &f_addr}, {"user name", &f_user}, {"password", &f_pass},
                                                                  {"POP3 server", &f_pop}, {"POP3 port", &f_popport}, {"SMTP server", &f_smtp}, {"SMTP port", &f_smtpport}};
        for (int i = 0; i < 8; i++) {
            int y = BAR + 66 + i * 30;
            c.text(24, y + 5, rows[i].label, th::muted);
            rows[i].f->focused = focus == i;
            rows[i].f->draw(c, 160, y, 300);
        }
        draw_checkbox(c, 490, BAR + 66 + 4 * 30 + 4, "TLS", acct.pop_tls);
        draw_checkbox(c, 490, BAR + 66 + 6 * 30 + 4, "TLS", acct.smtp_tls);
        draw_button(c, 160, BAR + 66 + 8 * 30 + 6, 100, 24, "Save", hot(160, BAR + 66 + 8 * 30 + 6, 100, 24), true);
        draw_button(c, 270, BAR + 66 + 8 * 30 + 6, 100, 24, "Cancel", hot(270, BAR + 66 + 8 * 30 + 6, 100, 24), false, acct.pop_host[0] != 0);
    }
    void draw_compose(Canvas& c, int w, int h) {
        c.text(24, BAR + 14, "New message", th::text, 2, true);
        c.text(24, BAR + 50, "to", th::muted);
        c_to.focused = focus == 0;
        c_to.draw(c, 90, BAR + 44, w - 120);
        c.text(24, BAR + 80, "subject", th::muted);
        c_subject.focused = focus == 1;
        c_subject.draw(c, 90, BAR + 74, w - 120);
        draw_bevel(c, 24, BAR + 108, w - 48, h - BAR - 108 - 70, th::white, true);
        int cols = max(8, (w - 70) / 8), y = BAR + 114, x = 32, col = 0;
        c.set_clip({26, BAR + 110, w - 52, h - BAR - 112 - 70});
        for (int i = 0; i < c_body.len; i++) {
            char ch2 = c_body[i];
            if (ch2 == '\n' || col >= cols) { y += 14; col = 0; if (ch2 == '\n') continue; }
            char s[2] = {ch2, 0};
            c.text(x + col * 8, y, s, th::text);
            col++;
        }
        if (focus == 2 && (ticks() / 500) % 2 == 0) c.fill(x + col * 8, y, 2, 12, accent());
        c.set_clip({0, 0, w, h});
        draw_button(c, 24, h - 56, 100, 26, "Send", hot(24, h - 56, 100, 26), true, !sess);
        draw_button(c, 134, h - 56, 100, 26, "Discard", hot(134, h - 56, 100, 26));
        if (status[0]) c.text_n(250, h - 48, status, (w - 270) / 8, th::muted);
    }

    // ---- input
    void on_mouse(const MouseEv& e) override {
        if (e.kind == M_WHEEL) {
            if (mode == 0 && e.x < LISTW) sc.set(clamp(sc.target - e.wheel * 50, 0, max(0, msgs.len * ROWH - (ch - BAR - 20))));
            else bsc.set(clamp(bsc.target - e.wheel * 50, 0, max(0, body_h - (ch - BAR - 20))));
            ui_invalidate();
            return;
        }
        if (e.kind == M_MOVE) { if (e.left) drag_field(e); ui_invalidate(); return; }
        if (e.kind == M_DOWN && e.button == 1) { click_field(e); ui_invalidate(); return; }
        if (e.kind != M_UP || e.button != 1) return;
        if (e.y < BAR) {
            if (Rect{34, 7, 80, 22}.contains(e.x, e.y)) { mode = 0; refresh(); }
            else if (Rect{120, 7, 80, 22}.contains(e.x, e.y) && acct.smtp_host[0]) { mode = 2; focus = 0; }
            else if (Rect{cw - 100, 7, 90, 22}.contains(e.x, e.y)) setup_open();
            ui_invalidate();
            return;
        }
        if (mode == 1) {
            if (Rect{490, BAR + 66 + 4 * 30, 60, 22}.contains(e.x, e.y)) acct.pop_tls = !acct.pop_tls;
            else if (Rect{490, BAR + 66 + 6 * 30, 60, 22}.contains(e.x, e.y)) acct.smtp_tls = !acct.smtp_tls;
            else if (Rect{160, BAR + 66 + 8 * 30 + 6, 100, 24}.contains(e.x, e.y)) setup_save();
            else if (Rect{270, BAR + 66 + 8 * 30 + 6, 100, 24}.contains(e.x, e.y) && acct.pop_host[0]) mode = 0;
        } else if (mode == 2) {
            if (Rect{24, ch - 56, 100, 26}.contains(e.x, e.y)) send_mail();
            else if (Rect{134, ch - 56, 100, 26}.contains(e.x, e.y)) { mode = 0; c_body.clear(); c_to.set(""); c_subject.set(""); }
        } else if (e.x < LISTW) {
            int i = (e.y - BAR + sc.pos()) / ROWH;
            if (i >= 0 && i < msgs.len) open_message(i);
        } else if (sel >= 0 && sel < msgs.len && !msgs[sel].full) open_message(sel);
        ui_invalidate();
    }
    void click_field(const MouseEv& e) {
        if (mode == 1) {
            for (int i = 0; i < 8; i++) {
                int y = BAR + 66 + i * 30;
                if (Rect{160, y, 300, 22}.contains(e.x, e.y)) { focus = i; TextField* fl[8] = {&f_name, &f_addr, &f_user, &f_pass, &f_pop, &f_popport, &f_smtp, &f_smtpport}; fl[i]->click(e.x - 160, 300, kbd_shift_down()); }
            }
        } else if (mode == 2) {
            if (Rect{90, BAR + 44, ch ? cw - 120 : 300, 22}.contains(e.x, e.y)) { focus = 0; c_to.click(e.x - 90, cw - 120, kbd_shift_down()); }
            else if (Rect{90, BAR + 74, cw - 120, 22}.contains(e.x, e.y)) { focus = 1; c_subject.click(e.x - 90, cw - 120, kbd_shift_down()); }
            else if (e.y > BAR + 108) focus = 2;
        }
    }
    void drag_field(const MouseEv& e) {
        if (mode == 1) { TextField* fl[8] = {&f_name, &f_addr, &f_user, &f_pass, &f_pop, &f_popport, &f_smtp, &f_smtpport}; fl[focus & 7]->drag(e.x - 160, 300); }
        else if (mode == 2 && focus == 0) c_to.drag(e.x - 90, cw - 120);
        else if (mode == 2 && focus == 1) c_subject.drag(e.x - 90, cw - 120);
    }
    void on_key(const KeyEvent& e) override {
        if (!e.down) return;
        if (mode == 1) {
            TextField* fl[8] = {&f_name, &f_addr, &f_user, &f_pass, &f_pop, &f_popport, &f_smtp, &f_smtpport};
            if (e.key == K_TAB) { focus = (focus + (e.shift ? 7 : 1)) % 8; ui_invalidate(); return; }
            if (e.key == K_ENTER) { setup_save(); ui_invalidate(); return; }
            if (fl[focus]->on_key(e)) ui_invalidate();
        } else if (mode == 2) {
            if (e.key == K_TAB) { focus = (focus + 1) % 3; ui_invalidate(); return; }
            if (focus == 0) { if (c_to.on_key(e)) ui_invalidate(); }
            else if (focus == 1) { if (c_subject.on_key(e)) ui_invalidate(); }
            else {
                if (e.key == K_BACKSPACE && c_body.len) c_body.len--;
                else if (e.key == K_ENTER) c_body.push('\n');
                else if (e.ctrl && e.key == 'v') {
                    u32 n = clip_len();
                    char* t = (char*)kmalloc(n + 8);
                    utf8_to_silicon(clip_get(), n, t, n + 8);
                    for (char* p = t; *p; p++) if (*p != '\r') c_body.push(*p);
                    kfree(t);
                } else if (e.ch >= 32 && !e.ctrl && c_body.len < 20000) c_body.push(e.ch);
                ui_invalidate();
            }
        } else {
            if (e.key == K_DOWN && sel + 1 < msgs.len) open_message(sel + 1);
            else if (e.key == K_UP && sel > 0) open_message(sel - 1);
            else if (e.key == 'r' || e.key == K_F5) refresh();
            else if (e.key == 'c' && acct.smtp_host[0]) { mode = 2; focus = 0; }
            ui_invalidate();
        }
    }
};

App* new_mail(const char*) { return new MailApp(); }
