// silicon - USB: UHCI (USB 1, QEMU's piix3-usb-uhci) and xHCI (USB 3, qemu-xhci) host controllers, device
// enumeration and the bulk-only mass storage class with SCSI read/write, so USB drives show up as
// block devices. Everything is polled.
#include "blk.h"
#include "pci.h"
#include "../arch/arch.h"

// ---------------------------------------------------------------- block device registry
static BlkDev devs[6];
static int ndevs = 0;
static u32 gen = 1;
int blk_count() { return ndevs; }
BlkDev* blk_get(int i) { return &devs[i]; }
u32 blk_generation() { return gen; }
void blk_changed() { gen++; }
BlkDev* blk_add(const char* name) {
    for (int i = 0; i < ndevs; i++) if (!strcmp(devs[i].name, name)) return &devs[i];
    if (ndevs >= 6) return nullptr;
    BlkDev* d = &devs[ndevs++];
    memset(d, 0, sizeof(*d));
    strlcpy(d->name, name, sizeof(d->name));
    return d;
}

// ---------------------------------------------------------------- UHCI
struct __attribute__((aligned(16))) Td { u32 link, ctrl, token, buf; u32 pad[4]; };
struct __attribute__((aligned(16))) Qh { u32 link, elem; u32 pad[2]; };

static u16 io = 0;
static u32* frame_list = nullptr;
static Qh* qh = nullptr;
static Td* tds = nullptr;
static const int NTD = 130;
static u8* dma = nullptr;                       // bounce buffer: 8 KiB of data, then the setup packet
static bool have_hc = false;

enum { PID_SETUP = 0x2D, PID_IN = 0x69, PID_OUT = 0xE1 };
static inline void w16(u16 off, u16 v) { outw(io + off, v); }
static inline u16 r16(u16 off) { return inw(io + off); }

struct UsbDev {
    bool used;
    bool xhci;                                  // on the xHCI controller (else UHCI)
    void* x;                                    // its XDev
    int port;
    u8 addr;
    u8 maxpkt0;
    bool lowspeed;
    u8 ep_in, ep_out;
    u16 mps_in, mps_out;
    u8 tog_in, tog_out;
    BlkDev* blk;
    u32 tag;
};
static UsbDev udev[10];                   // 0-1: UHCI ports, 2-9: xHCI ports

static u32 td_ctrl(bool lowspeed, bool spd) {
    return (1u << 23) | (3u << 27) | (lowspeed ? (1u << 26) : 0) | (spd ? (1u << 29) : 0);
}
static u32 td_token(u8 pid, u8 addr, u8 ep, u8 toggle, u32 len) {
    return pid | ((u32)addr << 8) | ((u32)ep << 15) | ((u32)toggle << 19) | ((len ? len - 1 : 0x7FF) << 21);
}
static u32 td_actual(u32 ctrl) { return (ctrl & 0x7FF) == 0x7FF ? 0 : (ctrl & 0x7FF) + 1; }
static u32 td_want(u32 token) { return ((token >> 21) & 0x7FF) == 0x7FF ? 0 : ((token >> 21) & 0x7FF) + 1; }

// run TDs [0, n) and wait; returns the bytes received by IN TDs, or -1 on an error or timeout
static int run_chain(int n, u32 timeout_ms) {
    for (int i = 0; i < n; i++) tds[i].link = (i + 1 < n) ? ((u32)(u64)&tds[i + 1] | 4) : 1;
    qh->elem = (u32)(u64)&tds[0];
    u64 end = ticks() + timeout_ms;
    int result = -1;
    for (;;) {
        bool pending = false, failed = false;
        int got = 0;
        for (int i = 0; i < n; i++) {
            u32 c = tds[i].ctrl;
            if (c & (1u << 23)) { pending = true; break; }
            if (c & 0x7E0000) { failed = true; break; }                // stalled, babble, timeout...
            bool in = (tds[i].token & 0xFF) == PID_IN;
            if (in) got += (int)td_actual(c);
            if (in && (c & (1u << 29)) && td_actual(c) < td_want(tds[i].token)) break;     // a short packet ends the transfer
        }
        if (failed) {
            for (int i = 0; i < n; i++) if (tds[i].ctrl & 0x7E0000) { klog("usb: td %d/%d failed, status %x token %x\n", i, n, tds[i].ctrl, tds[i].token); break; }
            break;
        }
        if (!pending) { result = got; break; }
        if (ticks() > end) {
            for (int i = 0; i < n; i++) if (tds[i].ctrl & (1u << 23)) { klog("usb: td %d/%d timed out, status %x token %x\n", i, n, tds[i].ctrl, tds[i].token); break; }
            break;
        }
    }
    qh->elem = 1;
    for (int i = 0; i < n; i++) tds[i].ctrl &= ~(1u << 23);
    return result;
}

// a control transfer on endpoint 0: bytes received for an IN data stage, 0 otherwise, -1 on failure
static int control(u8 addr, u8 mps, bool ls, u8 type, u8 req, u16 val, u16 idx, u16 len, void* data, bool in) {
    u8* setup = dma + 8192;
    setup[0] = type; setup[1] = req;
    setup[2] = val & 0xFF; setup[3] = val >> 8; setup[4] = idx & 0xFF; setup[5] = idx >> 8;
    setup[6] = len & 0xFF; setup[7] = len >> 8;
    int n = 0;
    tds[n].ctrl = td_ctrl(ls, false);
    tds[n].token = td_token(PID_SETUP, addr, 0, 0, 8);
    tds[n].buf = (u32)(u64)setup;
    n++;
    u8 tog = 1;
    u32 off = 0;
    while (off < len && n < NTD - 2) {
        u32 chunk = min<u32>(mps, len - off);
        tds[n].ctrl = td_ctrl(ls, in);
        tds[n].token = td_token(in ? PID_IN : PID_OUT, addr, 0, tog, chunk);
        tds[n].buf = (u32)(u64)(dma + off);
        if (!in) memcpy(dma + off, (u8*)data + off, chunk);
        tog ^= 1; off += chunk; n++;
    }
    tds[n].ctrl = td_ctrl(ls, false);
    tds[n].token = td_token(len && in ? PID_OUT : PID_IN, addr, 0, 1, 0);
    tds[n].buf = 0;
    n++;
    int r = run_chain(n, 1500);
    if (r < 0) return -1;
    if (in && len) memcpy(data, dma, min<u32>((u32)r, len));
    return r;
}

// a bulk transfer on one endpoint, in runs of up to 8 KiB; returns the bytes moved or -1
static int bulk(UsbDev* d, bool in, void* data, u32 len) {
    u8 ep = in ? d->ep_in : d->ep_out;
    u16 mps = in ? d->mps_in : d->mps_out;
    u8& tog = in ? d->tog_in : d->tog_out;
    int total = 0;
    u32 off = 0;
    while (off < len) {
        int n = 0;
        u32 start = off;
        u8 tog0 = tog;
        while (off < len && n < NTD - 1 && off - start < 8192) {
            u32 chunk = min<u32>(mps, len - off);
            tds[n].ctrl = td_ctrl(d->lowspeed, in);
            tds[n].token = td_token(in ? PID_IN : PID_OUT, d->addr, ep, tog, chunk);
            tds[n].buf = (u32)(u64)(dma + (off - start));
            if (!in) memcpy(dma + (off - start), (u8*)data + off, chunk);
            tog ^= 1; off += chunk; n++;
        }
        int r = run_chain(n, 3000);
        if (r < 0) { tog = tog0; return -1; }
        if (in) {
            memcpy((u8*)data + start, dma, r);
            total += r;
            if ((u32)r < off - start) {                             // short packet: the device is done
                tog = (u8)(tog0 ^ ((r + mps - 1) / mps & 1));
                break;
            }
        } else total += (int)(off - start);
    }
    return total;
}

#include "xhci.inc"

// control and bulk transfers on whichever controller the device hangs off
static int dctl(UsbDev* d, u8 type, u8 req, u16 val, u16 idx, u16 len, void* data, bool in) {
    return d->xhci ? xhci_control(d, type, req, val, idx, len, data, in) : control(d->addr, d->maxpkt0, d->lowspeed, type, req, val, idx, len, data, in);
}
static int dbulk(UsbDev* d, bool in, void* data, u32 len) { return d->xhci ? xhci_bulk(d, in, data, len) : bulk(d, in, data, len); }

// ---------------------------------------------------------------- mass storage (bulk-only, SCSI)
struct __attribute__((packed)) Cbw { u32 sig, tag, len; u8 flags, lun, cblen; u8 cb[16]; };
struct __attribute__((packed)) Csw { u32 sig, tag, residue; u8 status; };

// bulk-only reset recovery: reset the interface, clear both endpoints' halt, restart the data toggles
static void recover(UsbDev* d) {
    dctl(d, 0x21, 0xFF, 0, 0, 0, nullptr, false);
    dctl(d, 0x02, 1, 0, d->ep_in | 0x80, 0, nullptr, false);
    dctl(d, 0x02, 1, 0, d->ep_out, 0, nullptr, false);
    if (d->xhci) xhci_recover(d);
    d->tog_in = d->tog_out = 0;
}

static bool scsi_once(UsbDev* d, const u8* cdb, int cdblen, void* data, u32 len, bool in) {
    Cbw cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.sig = 0x43425355;
    cbw.tag = ++d->tag;
    cbw.len = len;
    cbw.flags = in ? 0x80 : 0;
    cbw.cblen = (u8)cdblen;
    memcpy(cbw.cb, cdb, cdblen);
    if (dbulk(d, false, &cbw, 31) != 31) { klog("usb: cbw failed\n"); return false; }
    if (len && dbulk(d, in, data, len) < 0) { klog("usb: data stage failed (%u bytes)\n", len); d->tog_in = d->tog_out = 0; return false; }
    Csw csw;
    memset(&csw, 0, sizeof(csw));
    int cr = dbulk(d, true, &csw, 13);
    if (cr != 13 || csw.sig != 0x53425355 || csw.tag != cbw.tag) { klog("usb: csw bad (%d, sig %x)\n", cr, csw.sig); return false; }
    return csw.status == 0;
}

static bool scsi(UsbDev* d, const u8* cdb, int cdblen, void* data, u32 len, bool in) {
    if (scsi_once(d, cdb, cdblen, data, len, in)) return true;
    // a failed transfer may have left the toggles out of step: recover once and try again
    recover(d);
    return scsi_once(d, cdb, cdblen, data, len, in);
}

static bool usb_read(BlkDev* b, u64 lba, u32 count, void* buf) {
    UsbDev* d = (UsbDev*)b->priv;
    if (!d || !b->present) return false;
    u8* p = (u8*)buf;
    while (count) {
        u32 n = min<u32>(count, 32);
        u8 cdb[10] = {0x28, 0, (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8), (u8)lba, 0, (u8)(n >> 8), (u8)n, 0};
        if (!scsi(d, cdb, 10, p, n * 512, true)) return false;
        p += n * 512; lba += n; count -= n;
    }
    return true;
}

static bool usb_write(BlkDev* b, u64 lba, u32 count, const void* buf) {
    UsbDev* d = (UsbDev*)b->priv;
    if (!d || !b->present) return false;
    const u8* p = (const u8*)buf;
    while (count) {
        u32 n = min<u32>(count, 32);
        u8 cdb[10] = {0x2A, 0, (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8), (u8)lba, 0, (u8)(n >> 8), (u8)n, 0};
        if (!scsi(d, cdb, 10, (void*)p, n * 512, false)) return false;
        p += n * 512; lba += n; count -= n;
    }
    return true;
}

// ---------------------------------------------------------------- enumeration
static bool port_reset(int port) {
    u16 reg = 0x10 + port * 2;
    w16(reg, 0x0200);                                          // reset
    sleep_ms(60);
    w16(reg, 0x0000);
    sleep_ms(10);
    w16(reg, 0x000E);                                          // enable, clear the change bits
    for (int i = 0; i < 50; i++) { if (r16(reg) & 4) return true; sleep_ms(2); }
    return false;
}

static void remove_dev(UsbDev& d) {
    if (d.blk) { d.blk->present = false; blk_changed(); klog("usb: %s removed\n", d.blk->name); }
    if (d.xhci) xhci_release(&d);
    d.used = false;
    d.blk = nullptr;
}

static bool identify(UsbDev& d, int idx);

static bool enumerate(int port) {
    UsbDev& d = udev[port];
    memset(&d, 0, sizeof(d));
    d.port = port;
    u16 st = r16(0x10 + port * 2);
    d.lowspeed = (st & 0x100) != 0;
    if (!port_reset(port)) { klog("usb: port %d would not reset\n", port); return false; }
    u8 desc[64];
    if (control(0, 8, d.lowspeed, 0x80, 6, 0x0100, 0, 8, desc, true) < 8) { klog("usb: no answer from the device\n"); return false; }
    d.maxpkt0 = desc[7] ? desc[7] : 8;
    d.addr = (u8)(port + 1);
    if (control(0, d.maxpkt0, d.lowspeed, 0x00, 5, d.addr, 0, 0, nullptr, false) < 0) { klog("usb: set address failed\n"); return false; }
    sleep_ms(10);
    return identify(d, port);
}

// xHCI: the controller resets the port and addresses the device for us
static bool enumerate_x(int p) {
    UsbDev& d = udev[2 + p];
    memset(&d, 0, sizeof(d));
    d.port = p;
    d.xhci = true;
    if (!xhci_address(&d, p)) { if (d.x) xhci_release(&d); return false; }
    if (!identify(d, 2 + p)) { xhci_release(&d); return false; }
    return true;
}

// from the device descriptor on, the same for every controller: find the bulk endpoints, set the
// configuration, wait for the medium and register a block device
static bool identify(UsbDev& d, int idx) {
    u8 desc[64];
    if (dctl(&d, 0x80, 6, 0x0100, 0, 18, desc, true) < 18) return false;
    u16 vid = desc[8] | (desc[9] << 8), pid = desc[10] | (desc[11] << 8);
    u8 cfg[256];
    if (dctl(&d, 0x80, 6, 0x0200, 0, 9, cfg, true) < 9) return false;
    u16 total = min<u16>(cfg[2] | (cfg[3] << 8), sizeof(cfg));
    if (dctl(&d, 0x80, 6, 0x0200, 0, total, cfg, true) < 9) return false;
    bool msc = false;
    for (u32 o = 9; o + 2 <= total;) {
        u8 l = cfg[o], t = cfg[o + 1];
        if (l < 2) break;
        if (t == 4 && l >= 9) msc = cfg[o + 5] == 8 && cfg[o + 6] == 6 && cfg[o + 7] == 0x50;
        if (t == 5 && l >= 7 && msc && (cfg[o + 3] & 3) == 2) {
            u8 ea = cfg[o + 2];
            u16 mps = cfg[o + 4] | (cfg[o + 5] << 8);
            if (ea & 0x80) { if (!d.ep_in) { d.ep_in = ea & 15; d.mps_in = mps; } }
            else if (!d.ep_out) { d.ep_out = ea & 15; d.mps_out = mps; }
        }
        o += l;
    }
    if (!d.ep_in || !d.ep_out) { klog("usb: device %04x:%04x is not a mass storage drive\n", vid, pid); return false; }
    if (d.xhci && !xhci_configure(&d)) return false;
    if (dctl(&d, 0x00, 9, cfg[5], 0, 0, nullptr, false) < 0) return false;
    u8 inq[36];
    memset(inq, 0, sizeof(inq));
    u8 c_inq[6] = {0x12, 0, 0, 0, 36, 0};
    if (!scsi(&d, c_inq, 6, inq, 36, true)) { klog("usb: INQUIRY failed\n"); return false; }
    u8 c_tur[6] = {0, 0, 0, 0, 0, 0};
    bool ready = false;
    for (int i = 0; i < 20 && !ready; i++) {
        ready = scsi(&d, c_tur, 6, nullptr, 0, true);
        if (!ready) { u8 sense[18]; u8 c_rs[6] = {3, 0, 0, 0, 18, 0}; scsi(&d, c_rs, 6, sense, 18, true); sleep_ms(50); }
    }
    u8 cap[8];
    u8 c_cap[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (!ready || !scsi(&d, c_cap, 10, cap, 8, true)) { klog("usb: drive not ready\n"); return false; }
    u32 last = ((u32)cap[0] << 24) | (cap[1] << 16) | (cap[2] << 8) | cap[3];
    u32 bs = ((u32)cap[4] << 24) | (cap[5] << 16) | (cap[6] << 8) | cap[7];
    if (bs != 512) { klog("usb: %u byte sectors are not supported\n", bs); return false; }
    char name[12];
    snprintf(name, sizeof(name), "usb%d", idx);
    BlkDev* b = blk_add(name);
    if (!b) return false;
    b->sectors = (u64)last + 1;
    b->sector_size = 512;
    b->removable = true;
    b->read = usb_read;
    b->write = usb_write;
    b->priv = &d;
    char model[40];
    memcpy(model, inq + 8, 8);
    model[8] = ' ';
    memcpy(model + 9, inq + 16, 16);
    model[25] = 0;
    for (int i = 24; i > 0 && model[i] == ' '; i--) model[i] = 0;
    strlcpy(b->model, model, sizeof(b->model));
    b->serial++;
    b->present = true;
    d.blk = b;
    d.used = true;
    blk_changed();
    klog("usb: %s %s, %u MiB\n", name, b->model, (u32)(b->sectors / 2048));
    return true;
}

int usb_port_count() { return (have_hc ? 2 : 0) + (have_xhci ? xports : 0); }

void usb_init() {
    if (!pci_count()) pci_scan();
    PciDevice* hc = nullptr;
    for (int i = 0; i < pci_count(); i++) {
        PciDevice* p = pci_get(i);
        if (p->cls == 0x0C && p->subcls == 0x03) {
            u32 pi = (pci_read(p->bus, p->dev, p->fn, 8) >> 8) & 0xFF;
            if (pi == 0x00 && (p->bar[4] & 1)) { hc = p; break; }
        }
    }
    for (int i = 0; i < pci_count(); i++) {
        PciDevice* p = pci_get(i);
        if (p->cls == 0x0C && p->subcls == 0x03 && ((pci_read(p->bus, p->dev, p->fn, 8) >> 8) & 0xFF) == 0x30) {
            have_xhci = xhci_start(p);
            break;
        }
    }
    if (!hc) { klog("usb: no UHCI controller\n"); if (have_xhci) usb_poll(); return; }
    io = (u16)(hc->bar[4] & ~3u);
    pci_enable_bus_master(hc);
    pci_write(hc->bus, hc->dev, hc->fn, 0xC0, 0x8F00);          // legacy support off
    u8* raw = (u8*)kmalloc(4096 * 2);
    frame_list = (u32*)(((u64)raw + 4095) & ~4095ull);
    qh = (Qh*)(((u64)kmalloc(sizeof(Qh) + 16) + 15) & ~15ull);
    tds = (Td*)(((u64)kmalloc(sizeof(Td) * NTD + 16) + 15) & ~15ull);
    dma = (u8*)kcalloc(8192 + 64);
    memset(tds, 0, sizeof(Td) * NTD);
    qh->link = 1;
    qh->elem = 1;
    for (int i = 0; i < 1024; i++) frame_list[i] = (u32)(u64)qh | 2;
    w16(0x00, 0x0004);                                           // global reset
    sleep_ms(20);
    w16(0x00, 0x0000);
    sleep_ms(5);
    w16(0x00, 0x0002);                                           // host controller reset
    for (int i = 0; i < 100 && (r16(0x00) & 2); i++) sleep_ms(1);
    w16(0x04, 0);                                                // no interrupts: we poll
    w16(0x06, 0);
    outl(io + 0x08, (u32)(u64)frame_list);
    outb(io + 0x0C, 0x40);
    w16(0x02, 0xFFFF);
    w16(0x00, 0x00C1);                                           // run, configure flag, max packet 64
    have_hc = true;
    klog("usb: UHCI at io %x\n", io);
    usb_poll();
}

static void xhci_poll() {
    static u64 xretry[XMAXDEV];
    for (int p = 0; p < xports; p++) {
        u32 sc = xportsc(p);
        bool connected = sc & 1;
        UsbDev& d = udev[2 + p];
        if (!connected && d.used) remove_dev(d);
        else if (connected && !d.used && ticks() >= xretry[p]) {
            if (!enumerate_x(p)) { xretry[p] = ticks() + 5000; d.used = false; }
        }
        if (sc & PORT_CHANGES) xportsc_set(p, PORT_CHANGES);
    }
    Trb ev;
    while (ev_next(&ev)) {}                                    // port status events: nothing to do, the ports are polled
}

void usb_poll() {
    if (!have_hc && !have_xhci) return;
    static u64 last = 0;
    static u64 retry[2];
    if (ticks() - last < 250) return;
    last = ticks();
    if (have_xhci) xhci_poll();
    if (!have_hc) return;
    for (int port = 0; port < 2; port++) {
        u16 st = r16(0x10 + port * 2);
        bool connected = st & 1;
        UsbDev& d = udev[port];
        if (!connected && d.used) { remove_dev(d); }
        else if (connected && !d.used && ticks() >= retry[port]) {
            if (!enumerate(port)) { retry[port] = ticks() + 5000; d.used = false; }
        }
        if (st & 2) w16(0x10 + port * 2, (r16(0x10 + port * 2) & 0x1FF5) | 2);      // acknowledge the change
    }
}
