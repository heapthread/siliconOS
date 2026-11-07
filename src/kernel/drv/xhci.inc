// silicon - xHCI (USB 3) host controller, included into usb.cpp. Polled, one transfer at a time.
// Layout: command ring, one event ring, a device context per slot, and a transfer ring per endpoint.
// Written from the xHCI specification and tested against QEMU's qemu-xhci with a usb-storage drive.

struct Trb { u64 param; u32 status; u32 ctrl; };
struct XRing { Trb* trb; int n, enq; u8 cycle; };

struct XDev {
    int  slot;                   // 0 = not in use
    XRing ep0, bin, bout;
    u8*  inctx;
    u8*  devctx;
    int  dci_in, dci_out;
    u8   speed;
};

static const int XMAXDEV = 8;
static XDev xdev[XMAXDEV];
static volatile u8* xcap = nullptr;
static volatile u8* xop = nullptr;
static volatile u8* xrt = nullptr;
static volatile u32* xdb = nullptr;
static int xports = 0, xslots = 0, xctx = 32;
static XRing xcmd_ring;
static Trb* xev = nullptr;
static int xev_n = 256, xev_deq = 0;
static u8 xev_cycle = 1;
static u64* xdcbaa = nullptr;
static u8* xdma = nullptr;                                  // bounce buffer for transfers (64 KiB aligned)
static bool have_xhci = false;

static inline u32 xr32(volatile u8* base, u32 off) { return *(volatile u32*)(base + off); }
static inline void xw32(volatile u8* base, u32 off, u32 v) { *(volatile u32*)(base + off) = v; }
static inline void xw64(volatile u8* base, u32 off, u64 v) { xw32(base, off, (u32)v); xw32(base, off + 4, (u32)(v >> 32)); }

static void* dma_alloc(u32 size, u32 align) {
    u8* p = (u8*)kcalloc(size + align);
    return (void*)(((u64)p + align - 1) & ~(u64)(align - 1));
}

static Trb* ring_push(XRing& r, u64 param, u32 status, u32 ctrl) {
    Trb* t = &r.trb[r.enq];
    t->param = param;
    t->status = status;
    __sync_synchronize();
    t->ctrl = ctrl | r.cycle;
    r.enq++;
    if (r.enq == r.n - 1) {                                  // the link TRB at the end sends us back to the start
        Trb* l = &r.trb[r.enq];
        l->param = (u64)r.trb;
        l->status = 0;
        l->ctrl = (6u << 10) | (1u << 1) | r.cycle;
        r.cycle ^= 1;
        r.enq = 0;
    }
    return t;
}

static void ring_init(XRing& r, int n) {
    if (!r.trb) r.trb = (Trb*)dma_alloc(n * sizeof(Trb), 4096);
    else memset(r.trb, 0, n * sizeof(Trb));
    r.n = n; r.enq = 0; r.cycle = 1;
}

static bool ev_next(Trb* out) {
    volatile Trb* t = &xev[xev_deq];
    u32 c = t->ctrl;
    if ((c & 1) != xev_cycle) return false;
    __sync_synchronize();
    out->param = t->param;
    out->status = t->status;
    out->ctrl = c;
    xev_deq++;
    if (xev_deq == xev_n) { xev_deq = 0; xev_cycle ^= 1; }
    xw64(xrt, 0x20 + 0x18, (u64)&xev[xev_deq] | 8);          // new dequeue pointer, clear "event handler busy"
    return true;
}

// wait for an event of one type (type 33 command completion, 32 transfer); other events are dropped
static bool wait_event(u32 type, u32 timeout_ms, Trb* out) {
    u64 end = ticks() + timeout_ms;
    for (;;) {
        Trb t;
        while (ev_next(&t)) {
            if (((t.ctrl >> 10) & 63) == type) { *out = t; return true; }
        }
        if (ticks() > end) return false;
    }
}

// run one command; returns the completion code (1 = success) or -1 on a timeout. *slot gets the slot id from the event
static int xcommand(u32 type, u64 param, u32 extra, int* slot = nullptr) {
    ring_push(xcmd_ring, param, 0, (type << 10) | extra);
    xdb[0] = 0;
    Trb ev;
    if (!wait_event(33, 2000, &ev)) { klog("xhci: command %u timed out\n", type); return -1; }
    if (slot) *slot = (int)(ev.ctrl >> 24);
    return (int)(ev.status >> 24);
}

static u32 xportsc(int p) { return xr32(xop, 0x400 + p * 0x10); }
static void xportsc_set(int p, u32 set) { xw32(xop, 0x400 + p * 0x10, (xportsc(p) & 0x0E000200) | set); }   // keeps power and wake bits, never writes the enable bit
static const u32 PORT_CHANGES = 0x00FE0000;

static u8* ctx_at(u8* base, int i) { return base + (size_t)i * xctx; }

static bool xhci_ep_recover(XDev* x, XRing& r, int dci) {
    u32 e = ((u32)x->slot << 24) | ((u32)dci << 16);
    xcommand(15, 0, e);                                       // stop (fails harmlessly if it is not running)
    xcommand(14, 0, e);                                       // reset
    int c = xcommand(16, (u64)&r.trb[r.enq] | r.cycle, e);    // dequeue pointer: skip the failed transfer
    return c == 1;
}

static int xhci_control(UsbDev* d, u8 type, u8 req, u16 val, u16 idx, u16 len, void* data, bool in) {
    XDev* x = (XDev*)d->x;
    u8* buf = xdma;
    u64 setup = (u64)type | ((u64)req << 8) | ((u64)val << 16) | ((u64)idx << 32) | ((u64)len << 48);
    u32 trt = len == 0 ? 0 : in ? 3 : 2;
    ring_push(x->ep0, setup, 8, (2u << 10) | (1u << 6) | (trt << 16));
    if (len) {
        if (!in) memcpy(buf, data, len);
        ring_push(x->ep0, (u64)buf, len, (3u << 10) | (in ? (1u << 16) : 0));
    }
    ring_push(x->ep0, 0, 0, (4u << 10) | (1u << 5) | ((len && in) ? 0 : (1u << 16)));
    xdb[x->slot] = 1;
    Trb ev;
    if (!wait_event(32, 1500, &ev)) { klog("xhci: control transfer timed out\n"); xhci_ep_recover(x, x->ep0, 1); return -1; }
    u32 code = ev.status >> 24;
    if (code != 1 && code != 13) { klog("xhci: control transfer failed, code %u\n", code); xhci_ep_recover(x, x->ep0, 1); return -1; }
    if (in && len) memcpy(data, buf, len);
    return len;
}

static int xhci_bulk(UsbDev* d, bool in, void* data, u32 len) {
    XDev* x = (XDev*)d->x;
    XRing& r = in ? x->bin : x->bout;
    int dci = in ? x->dci_in : x->dci_out;
    int total = 0;
    u32 off = 0;
    while (off < len) {
        u32 chunk = min<u32>(len - off, 16384);
        if (!in) memcpy(xdma, (u8*)data + off, chunk);
        ring_push(r, (u64)xdma, chunk, (1u << 10) | (1u << 5) | (1u << 2));           // normal TRB, interrupt on completion, interrupt on short packet
        xdb[x->slot] = (u32)dci;
        Trb ev;
        if (!wait_event(32, 3000, &ev)) { klog("xhci: bulk transfer timed out\n"); xhci_ep_recover(x, r, dci); return -1; }
        u32 code = ev.status >> 24;
        u32 got;
        if (code == 1) got = chunk;
        else if (code == 13) got = chunk - (ev.status & 0xFFFFFF);
        else { klog("xhci: bulk transfer failed, code %u\n", code); xhci_ep_recover(x, r, dci); return -1; }
        if (in) memcpy((u8*)data + off, xdma, got);
        off += chunk;
        total += (int)got;
        if (got < chunk) break;                                                      // short packet: the device is done
    }
    return total;
}

static void xhci_recover(UsbDev* d) {
    XDev* x = (XDev*)d->x;
    xhci_ep_recover(x, x->bin, x->dci_in);
    xhci_ep_recover(x, x->bout, x->dci_out);
}

// reset the port, give the device a slot and an address; fills d->speed, d->maxpkt0
static bool xhci_address(UsbDev* d, int p) {
    XDev* x = &xdev[p];
    if (!(xportsc(p) & 2)) {                                  // USB 2 ports need a reset (USB 3 ports come up enabled)
        xportsc_set(p, (1u << 4));
        u64 end = ticks() + 500;
        while (ticks() < end && !(xportsc(p) & (1u << 21))) sleep_ms(2);
        xportsc_set(p, PORT_CHANGES);
        sleep_ms(10);
    }
    u32 sc = xportsc(p);
    if (!(sc & 1) || !(sc & 2)) { klog("xhci: port %d would not enable (%x)\n", p, sc); return false; }
    x->speed = (u8)((sc >> 10) & 15);
    int slot = 0;
    int c = xcommand(9, 0, 0, &slot);
    if (c != 1 || slot <= 0 || slot > xslots) { klog("xhci: no slot (code %d)\n", c); return false; }
    x->slot = slot;
    memset(x->devctx, 0, 4096);
    memset(x->inctx, 0, 4096);
    ring_init(x->ep0, 64);
    xdcbaa[slot] = (u64)x->devctx;
    u32 mps = x->speed == 4 ? 512 : x->speed == 2 ? 8 : 64;
    u32* icc = (u32*)x->inctx;
    icc[1] = 3;
    u32* sctx = (u32*)ctx_at(x->inctx, 1);
    sctx[0] = ((u32)x->speed << 20) | (1u << 27);
    sctx[1] = (u32)(p + 1) << 16;
    u32* e0 = (u32*)ctx_at(x->inctx, 2);
    e0[1] = (3u << 1) | (4u << 3) | (mps << 16);
    *(u64*)&e0[2] = (u64)x->ep0.trb | 1;
    e0[4] = 8;
    c = xcommand(11, (u64)x->inctx, (u32)slot << 24);
    if (c != 1) { klog("xhci: address device failed (code %d)\n", c); return false; }
    d->x = x;
    d->xhci = true;
    d->lowspeed = x->speed == 2;
    d->maxpkt0 = (u8)min<u32>(mps, 255);
    // learn the real packet size of endpoint 0
    u8 desc[18];
    if (xhci_control(d, 0x80, 6, 0x0100, 0, 8, desc, true) < 8) { klog("xhci: no device descriptor\n"); return false; }
    u32 real = x->speed == 4 ? (1u << desc[7]) : desc[7];
    if (real && real != mps) {
        memset(x->inctx, 0, 4096);
        icc[1] = 2;
        e0[1] = (3u << 1) | (4u << 3) | (real << 16);
        if (xcommand(13, (u64)x->inctx, (u32)slot << 24) != 1) klog("xhci: evaluate context failed\n");
        d->maxpkt0 = (u8)min<u32>(real, 255);
    }
    return true;
}

// after the endpoints are known: add the two bulk endpoints
static bool xhci_configure(UsbDev* d) {
    XDev* x = (XDev*)d->x;
    x->dci_in = d->ep_in * 2 + 1;
    x->dci_out = d->ep_out * 2;
    int maxdci = max(x->dci_in, x->dci_out);
    ring_init(x->bin, 64);
    ring_init(x->bout, 64);
    memset(x->inctx, 0, 4096);
    u32* icc = (u32*)x->inctx;
    icc[1] = 1u | (1u << x->dci_in) | (1u << x->dci_out);
    memcpy(ctx_at(x->inctx, 1), x->devctx, 32);
    u32* sctx = (u32*)ctx_at(x->inctx, 1);
    sctx[0] = (sctx[0] & 0x07FFFFFF) | ((u32)maxdci << 27);
    auto ep = [&](int dci, XRing& r, u32 type, u32 mps) {
        u32* e = (u32*)ctx_at(x->inctx, dci + 1);
        e[0] = 0;
        e[1] = (3u << 1) | (type << 3) | (mps << 16);
        *(u64*)&e[2] = (u64)r.trb | 1;
        e[4] = mps;
    };
    ep(x->dci_in, x->bin, 6, d->mps_in);
    ep(x->dci_out, x->bout, 2, d->mps_out);
    int c = xcommand(12, (u64)x->inctx, (u32)x->slot << 24);
    if (c != 1) { klog("xhci: configure endpoints failed (code %d)\n", c); return false; }
    return true;
}

static void xhci_release(UsbDev* d) {
    XDev* x = (XDev*)d->x;
    if (!x || !x->slot) return;
    xcommand(10, 0, (u32)x->slot << 24);
    xdcbaa[x->slot] = 0;
    x->slot = 0;
}

// map the controller's registers uncached (they sit in the PCI hole, which the boot code maps as ordinary memory)
static void map_uncached(u64 addr, u64 len) {
    u64* pml4 = (u64*)(read_cr3() & ~0xFFFull);
    u64* pdpt = (u64*)(pml4[0] & ~0xFFFull);
    for (u64 a = addr & ~0x1FFFFFull; a < addr + len; a += 0x200000) {
        u64* pd = (u64*)(pdpt[a >> 30] & ~0xFFFull);
        pd[(a >> 21) & 511] |= 0x18;                                    // write-through + cache disable
    }
    write_cr3(read_cr3());
}

static bool xhci_start(PciDevice* hc) {
    u64 base = hc->bar[0] & ~0xFull;
    if ((hc->bar[0] & 6) == 4) base |= (u64)hc->bar[1] << 32;
    if (!base || base >= (1ull << 32)) { klog("xhci: registers at %llx are out of reach\n", (unsigned long long)base); return false; }
    u32 cmd = pci_read(hc->bus, hc->dev, hc->fn, 0x04);
    pci_write(hc->bus, hc->dev, hc->fn, 0x04, cmd | 7);                // memory space, bus master
    map_uncached(base, 0x10000);
    xcap = (volatile u8*)base;
    u32 caplen = xcap[0];
    u32 hcs1 = xr32(xcap, 4), hcs2 = xr32(xcap, 8), hcc = xr32(xcap, 0x10);
    xslots = (int)min<u32>(hcs1 & 0xFF, 8);
    xports = (int)(hcs1 >> 24);
    xctx = (hcc & 4) ? 64 : 32;
    xop = xcap + caplen;
    xrt = xcap + (xr32(xcap, 0x18) & ~0x1Fu);
    xdb = (volatile u32*)(xcap + (xr32(xcap, 0x14) & ~3u));
    if (xports > XMAXDEV) xports = XMAXDEV;
    // stop it, reset it
    if (!(xr32(xop, 4) & 1)) {
        xw32(xop, 0, xr32(xop, 0) & ~1u);
        for (int i = 0; i < 200 && !(xr32(xop, 4) & 1); i++) sleep_ms(1);
    }
    xw32(xop, 0, 2);
    for (int i = 0; i < 500 && ((xr32(xop, 0) & 2) || (xr32(xop, 4) & (1u << 11))); i++) sleep_ms(1);
    if (xr32(xop, 0) & 2) { klog("xhci: controller would not reset\n"); return false; }
    xw32(xop, 0x38, (u32)xslots);
    xdcbaa = (u64*)dma_alloc(64 * 8 + 8, 64);
    u32 nsp = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
    if (nsp) {
        u64* sp = (u64*)dma_alloc(nsp * 8, 64);
        for (u32 i = 0; i < nsp; i++) sp[i] = (u64)dma_alloc(4096, 4096);
        xdcbaa[0] = (u64)sp;
    }
    xw64(xop, 0x30, (u64)xdcbaa);
    ring_init(xcmd_ring, 256);
    xw64(xop, 0x18, (u64)xcmd_ring.trb | 1);
    xev = (Trb*)dma_alloc(xev_n * sizeof(Trb), 4096);
    u64* erst = (u64*)dma_alloc(16, 64);
    erst[0] = (u64)xev;
    erst[1] = (u64)xev_n;
    xw32(xrt, 0x20 + 0x08, 1);
    xw64(xrt, 0x20 + 0x18, (u64)xev);
    xw64(xrt, 0x20 + 0x10, (u64)erst);
    xdma = (u8*)dma_alloc(65536, 65536);
    for (int i = 0; i < XMAXDEV; i++) {
        xdev[i].devctx = (u8*)dma_alloc(4096, 64);
        xdev[i].inctx = (u8*)dma_alloc(4096, 64);
    }
    xw32(xop, 0, 1);                                                    // run
    for (int i = 0; i < 100 && (xr32(xop, 4) & 1); i++) sleep_ms(1);
    if (xr32(xop, 4) & 1) { klog("xhci: controller would not start\n"); return false; }
    // a no-op command proves the rings work
    int c = xcommand(23, 0, 0);
    if (c != 1) { klog("xhci: command ring is not answering (%d)\n", c); return false; }
    klog("xhci: controller at %llx, %d ports, %d slots, %d byte contexts\n", (unsigned long long)base, xports, xslots, xctx);
    return true;
}
