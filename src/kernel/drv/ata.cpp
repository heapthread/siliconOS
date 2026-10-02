// silicon - ATA disk (primary master, LBA28): bus-master IDE DMA when the PIIX controller is
// there (one VM exit per 64 KiB instead of one per word), PIO otherwise.
#include "drivers.h"
#include "pci.h"

static const u16 IO = 0x1F0, CTRL = 0x3F6;
static bool present = false;
static u32 sectors = 0;
static u16 bm = 0;                               // bus master io base (0 = PIO only)
static u8* bounce = nullptr;                     // 64 KiB, 64 KiB aligned (a PRD may not cross 64K)
static u32 prd[2] __attribute__((aligned(16)));

static bool wait_bsy() {
    u64 end = ticks() + 2000;
    while (ticks() < end) if (!(inb(IO + 7) & 0x80)) return true;
    return false;
}
static bool wait_drq() {
    u64 end = ticks() + 2000;
    while (ticks() < end) {
        u8 s = inb(IO + 7);
        if (s & 0x01) return false;
        if (!(s & 0x80) && (s & 0x08)) return true;
    }
    return false;
}

bool ata_init() {
    outb(CTRL, 0x02);                // no interrupts, we poll
    outb(IO + 6, 0xA0);
    io_wait(); io_wait(); io_wait(); io_wait();
    outb(IO + 2, 0); outb(IO + 3, 0); outb(IO + 4, 0); outb(IO + 5, 0);
    outb(IO + 7, 0xEC);              // IDENTIFY
    if (inb(IO + 7) == 0) return false;
    if (!wait_bsy()) return false;
    if (inb(IO + 4) || inb(IO + 5)) return false;   // not ATA
    if (!wait_drq()) return false;
    u16 id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(IO);
    sectors = id[60] | ((u32)id[61] << 16);
    present = true;
    klog("ata: primary master, %u sectors (%u MiB)\n", sectors, sectors / 2048);
    // bus mastering IDE controller (class 01:01, BAR4)
    if (!pci_count()) pci_scan();
    for (int i = 0; i < pci_count(); i++) {
        PciDevice* d = pci_get(i);
        if (d->cls == 1 && d->subcls == 1 && (d->bar[4] & 1) && (d->bar[4] & ~3u)) {
            pci_enable_bus_master(d);
            bm = (u16)(d->bar[4] & ~3u);
            u8* raw = (u8*)kmalloc(128 * 1024);
            bounce = (u8*)(((u64)raw + 0xFFFF) & ~0xFFFFull);
            klog("ata: bus master dma at io %x\n", bm);
            break;
        }
    }
    return true;
}

bool ata_present() { return present; }
u32 ata_sectors() { return sectors; }

static bool setup(u32 lba, u8 count, u8 cmd) {
    if (!wait_bsy()) return false;
    outb(IO + 6, 0xE0 | ((lba >> 24) & 0x0F));
    outb(IO + 2, count);
    outb(IO + 3, lba & 0xFF);
    outb(IO + 4, (lba >> 8) & 0xFF);
    outb(IO + 5, (lba >> 16) & 0xFF);
    outb(IO + 7, cmd);
    return true;
}

// one DMA command of up to 128 sectors through the bounce buffer
static bool dma(u32 lba, u32 n, bool write) {
    prd[0] = (u32)(u64)bounce;
    prd[1] = ((n * 512) & 0xFFFF) | 0x80000000u;  // 0 = 64 KiB, end of table
    outb(bm + 0, 0);
    outl(bm + 4, (u32)(u64)prd);
    outb(bm + 2, inb(bm + 2) | 0x06);             // clear interrupt + error
    u8 dir = write ? 0x00 : 0x08;
    outb(bm + 0, dir);
    if (!setup(lba, (u8)n, write ? 0xCA : 0xC8)) return false;
    outb(bm + 0, dir | 0x01);                     // go
    u64 t0 = ticks();
    u8 st;
    for (;;) {
        u8 bs = inb(bm + 2);
        st = inb(IO + 7);
        if (bs & 0x02) { outb(bm + 0, 0); return false; }
        if (!(bs & 0x01) && !(st & 0x80)) break;
        if (ticks() - t0 > 3000) { outb(bm + 0, 0); return false; }
    }
    outb(bm + 0, 0);
    return !(st & 0x01);
}

bool ata_read(u32 lba, u32 count, void* buf) {
    if (!present) return false;
    if (bm) {
        u8* p = (u8*)buf;
        while (count) {
            u32 n = count > 128 ? 128 : count;
            if (!dma(lba, n, false)) { bm = 0; klog("ata: dma read failed, using pio\n"); return ata_read(lba, count, p); }
            memcpy(p, bounce, n * 512);
            p += n * 512; lba += n; count -= n;
        }
        return true;
    }
    u16* p = (u16*)buf;
    while (count) {
        u32 n = count > 128 ? 128 : count;
        if (!setup(lba, (u8)n, 0x20)) return false;
        for (u32 s = 0; s < n; s++) {
            if (!wait_drq()) return false;
            u64 cnt = 256;
            __asm__ volatile("rep insw" : "+D"(p), "+c"(cnt) : "d"(IO) : "memory");
        }
        lba += n; count -= n;
    }
    return true;
}

bool ata_write(u32 lba, u32 count, const void* buf) {
    if (!present) return false;
    if (bm) {
        const u8* p = (const u8*)buf;
        while (count) {
            u32 n = count > 128 ? 128 : count;
            memcpy(bounce, p, n * 512);
            if (!dma(lba, n, true)) { bm = 0; klog("ata: dma write failed, using pio\n"); return ata_write(lba, count, p); }
            p += n * 512; lba += n; count -= n;
        }
        if (!wait_bsy()) return false;
        outb(IO + 7, 0xE7);                       // cache flush
        wait_bsy();
        return true;
    }
    const u16* p = (const u16*)buf;
    while (count) {
        u32 n = count > 128 ? 128 : count;
        if (!setup(lba, (u8)n, 0x30)) return false;
        for (u32 s = 0; s < n; s++) {
            if (!wait_drq()) return false;
            for (int i = 0; i < 256; i++) outw(IO, *p++);
        }
        lba += n; count -= n;
        outb(IO + 7, 0xE7);          // cache flush
        wait_bsy();
    }
    return true;
}
