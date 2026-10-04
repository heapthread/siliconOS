// silicon - host bridge on COM2. A small helper next to QEMU (tools/bridge.py, started by
// build.py) talks to it in text lines:
//   host -> silicon:  R <w> <h>   the QEMU window's size        H <hz>   the monitor's refresh rate
//                     C <base64>  the host clipboard changed
//   silicon -> host:  HI          silicon is up (host answers with R, H and C)
//                     C <base64>  silicon copied something
#include "drivers.h"
#include "../clipboard.h"
#include "../display.h"

static const u16 COM2 = 0x2F8;
static bool present = false, linked = false;
static char* line = nullptr;
static u32 line_len = 0, line_cap = 0;
static bool line_overflow = false;
static const u32 LINE_MAX = 400 * 1024;

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void tx(char c) {
    for (int i = 0; i < 20000 && !(inb(COM2 + 5) & 0x20); i++) {}
    outb(COM2, (u8)c);
}
static void tx_str(const char* s) { while (*s) tx(*s++); }

void bridge_init() {
    outb(COM2 + 7, 0xAE);
    if (inb(COM2 + 7) != 0xAE) return;               // no such UART
    outb(COM2 + 1, 0x00);                            // no interrupts: we poll
    outb(COM2 + 3, 0x80);
    outb(COM2 + 0, 0x01);
    outb(COM2 + 1, 0x00);
    outb(COM2 + 3, 0x03);                            // 8N1
    outb(COM2 + 2, 0xC7);                            // fifo on, cleared
    outb(COM2 + 4, 0x03);
    present = true;
    tx_str("HI\n");
}

bool bridge_linked() { return linked; }

void bridge_send_clip(const char* text, u32 len) {
    if (!present || !linked) return;
    tx_str("C ");
    for (u32 i = 0; i < len; i += 3) {
        u32 v = (u8)text[i] << 16;
        int n = 1;
        if (i + 1 < len) { v |= (u8)text[i + 1] << 8; n = 2; }
        if (i + 2 < len) { v |= (u8)text[i + 2]; n = 3; }
        tx(B64[(v >> 18) & 63]);
        tx(B64[(v >> 12) & 63]);
        tx(n > 1 ? B64[(v >> 6) & 63] : '=');
        tx(n > 2 ? B64[v & 63] : '=');
    }
    tx('\n');
}

static int b64v(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void handle(char* l, u32 n) {
    l[n] = 0;
    if (n < 1) return;
    linked = true;
    if (l[0] == 'R' && n > 2) {
        int w = atoi(l + 2);
        const char* sp = strchr(l + 2, ' ');
        int h = sp ? atoi(sp + 1) : 0;
        if (w > 0 && h > 0) display_bridge_size(w, h);
    } else if (l[0] == 'H' && n > 2) {
        display_bridge_hz(atoi(l + 2));
    } else if (l[0] == 'C' && n > 2) {
        u32 cap = (n / 4) * 3 + 4;
        char* out = (char*)kmalloc_try(cap);
        if (!out) return;
        u32 o = 0;
        int acc = 0, bits = 0;
        for (u32 i = 2; i < n; i++) {
            int v = b64v(l[i]);
            if (v < 0) continue;
            acc = (acc << 6) | v;
            bits += 6;
            if (bits >= 8) { bits -= 8; out[o++] = (char)((acc >> bits) & 255); }
        }
        clip_from_host(out, o);
        kfree(out);
    }
}

void bridge_poll() {
    if (!present) return;
    for (int budget = 0; budget < 1024 && (inb(COM2 + 5) & 1); budget++) {
        char c = (char)inb(COM2);
        if (c == '\r') continue;
        if (c == '\n') {
            if (!line_overflow && line) handle(line, line_len);
            line_len = 0;
            line_overflow = false;
            continue;
        }
        if (line_overflow) continue;
        if (line_len + 2 > line_cap) {
            if (line_cap >= LINE_MAX) { line_overflow = true; continue; }
            u32 nc = line_cap ? line_cap * 2 : 512;
            char* nb = (char*)kmalloc_try(nc);
            if (!nb) { line_overflow = true; continue; }
            if (line) { memcpy(nb, line, line_len); kfree(line); }
            line = nb;
            line_cap = nc;
        }
        line[line_len++] = c;
    }
}
