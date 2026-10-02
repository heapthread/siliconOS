// silicon - Realtek RTL8139 fast ethernet driver
// RX: a 64 KiB ring (packets may wrap around its end) the card DMAs into; TX: four 2 KiB descriptors used round-robin.
// The stack polls rtl8139_poll() from the main loop, so no protocol code runs in interrupt context.
#include "pci.h"
#include "../net/net.h"

enum {
    R_MAC = 0x00, R_MAR = 0x08, R_TSD0 = 0x10, R_TSAD0 = 0x20, R_RBSTART = 0x30, R_CMD = 0x37,
    R_CAPR = 0x38, R_CBR = 0x3A, R_IMR = 0x3C, R_ISR = 0x3E, R_TCR = 0x40, R_RCR = 0x44,
    R_CONFIG1 = 0x52, R_MSR = 0x58, R_BMCR = 0x62,
};
static const u32 RX_SIZE = 65536;
static u8 rx_tmp[1800];

static u16 io = 0;
static u8* rx_buf = nullptr;
static u8* tx_buf[4];
static int tx_cur = 0;
static u32 rx_off = 0;
static bool present = false;
static u64 tx_packets = 0, rx_packets = 0, tx_bytes = 0, rx_bytes = 0;

static void* alloc_dma(u32 size) {
    // the heap is identity mapped below 4 GiB, so heap addresses are physical addresses
    u8* p = (u8*)kcalloc(size + 16);
    return (void*)(((u64)p + 15) & ~15ull);
}

bool rtl8139_init(u8 mac_out[6]) {
    PciDevice* d = pci_find(0x10EC, 0x8139);
    if (!d) return false;
    pci_enable_bus_master(d);
    io = d->bar[0] & ~3u;
    outb(io + R_CONFIG1, 0x00);                  // power on
    outb(io + R_CMD, 0x10);                      // software reset
    for (u64 end = ticks() + 200; (inb(io + R_CMD) & 0x10) && ticks() < end;) {}
    rx_buf = (u8*)alloc_dma(RX_SIZE + 16 + 1536);
    for (int i = 0; i < 4; i++) tx_buf[i] = (u8*)alloc_dma(2048);
    outl(io + R_RBSTART, (u32)(u64)rx_buf);
    outw(io + R_IMR, 0x0000);                    // we poll
    outl(io + R_RCR, 0x0F | (3 << 11));          // accept all/physical/multicast/broadcast, 64K ring
    outl(io + R_TCR, 0x03000700);                // standard IFG, max DMA burst
    outb(io + R_CMD, 0x0C);                      // RX + TX enable
    rx_off = 0;
    for (int i = 0; i < 6; i++) mac_out[i] = inb(io + R_MAC + i);
    present = true;
    klog("rtl8139: io %x irq %u mac %x:%x:%x:%x:%x:%x\n", io, d->irq, mac_out[0], mac_out[1], mac_out[2],
         mac_out[3], mac_out[4], mac_out[5]);
    return true;
}

bool rtl8139_present() { return present; }

bool rtl8139_link() {
    if (!present) return false;
    return !(inb(io + R_MSR) & 0x04);            // LINKB is active low
}

bool rtl8139_send(const void* data, u32 len) {
    if (!present || len > 1792) return false;
    // wait for the descriptor to be ours again (OWN bit)
    u32 tsd = io + R_TSD0 + tx_cur * 4;
    for (u64 end = ticks() + 50; ticks() < end;) {
        u32 st = inl(tsd);
        if ((st & (1 << 13)) || !(st & 0x1FFF)) break;
    }
    memcpy(tx_buf[tx_cur], data, len);
    if (len < 60) { memset(tx_buf[tx_cur] + len, 0, 60 - len); len = 60; }
    outl(io + R_TSAD0 + tx_cur * 4, (u32)(u64)tx_buf[tx_cur]);
    outl(tsd, len);
    tx_cur = (tx_cur + 1) % 4;
    tx_packets++;
    tx_bytes += len;
    return true;
}

// deliver every waiting frame to the stack
void rtl8139_poll() {
    if (!present) return;
    u16 isr = inw(io + R_ISR);
    if (isr) outw(io + R_ISR, isr);
    int budget = 256;
    while (!(inb(io + R_CMD) & 0x01) && budget--) {      // BUFE clear = data waiting
        u8* p = rx_buf + rx_off;
        u16 status = p[0] | (p[1] << 8);
        u16 length = p[2] | (p[3] << 8);                 // includes the 4-byte CRC
        if (!(status & 0x01) || length < 18 || length > 1792) {
            // ring got confused: reset the receiver
            outb(io + R_CMD, 0x04);
            outl(io + R_RBSTART, (u32)(u64)rx_buf);
            rx_off = 0;
            outb(io + R_CMD, 0x0C);
            break;
        }
        rx_packets++;
        rx_bytes += length;
        u32 body = rx_off + 4;
        if (body >= RX_SIZE) body -= RX_SIZE;
        if (body + length - 4 <= RX_SIZE) net_receive_frame(rx_buf + body, length - 4);
        else {                                           // wrapped: stitch the two halves together
            u32 first = RX_SIZE - body;
            memcpy(rx_tmp, rx_buf + body, first);
            memcpy(rx_tmp + first, rx_buf, length - 4 - first);
            net_receive_frame(rx_tmp, length - 4);
        }
        rx_off = (rx_off + length + 4 + 3) & ~3u;
        if (rx_off >= RX_SIZE) rx_off -= RX_SIZE;
        outw(io + R_CAPR, (u16)(rx_off - 16));
    }
}

void rtl8139_stats(u64* txp, u64* rxp, u64* txb, u64* rxb) {
    *txp = tx_packets; *rxp = rx_packets; *txb = tx_bytes; *rxb = rx_bytes;
}
