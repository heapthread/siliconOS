// silicon - PCI configuration space (mechanism #1) and bus scan
#include "pci.h"

u32 pci_read(u8 bus, u8 dev, u8 fn, u8 off) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}

void pci_write(u8 bus, u8 dev, u8 fn, u8 off, u32 v) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

static PciDevice devices[32];
static int device_count = 0;

static const char* class_name(u8 cls, u8 sub) {
    switch (cls) {
    case 0x01: return sub == 0x01 ? "IDE controller" : "storage controller";
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x04: return "multimedia device";
    case 0x06: return sub == 0x00 ? "host bridge" : sub == 0x01 ? "ISA bridge" : "bridge";
    case 0x0C: return "serial bus controller";
    }
    return "device";
}

void pci_scan() {
    device_count = 0;
    for (int bus = 0; bus < 4; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                u32 id = pci_read(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; continue; }
                if (device_count >= 32) return;
                PciDevice& d = devices[device_count++];
                d.bus = bus; d.dev = dev; d.fn = fn;
                d.vendor = id & 0xFFFF;
                d.device = id >> 16;
                u32 cls = pci_read(bus, dev, fn, 8);
                d.cls = cls >> 24;
                d.subcls = (cls >> 16) & 0xFF;
                d.irq = pci_read(bus, dev, fn, 0x3C) & 0xFF;
                for (int b = 0; b < 6; b++) d.bar[b] = pci_read(bus, dev, fn, 0x10 + b * 4);
                d.name = class_name(d.cls, d.subcls);
                // single-function device: skip the other functions
                if (fn == 0 && !(pci_read(bus, dev, 0, 0x0C) & 0x00800000)) break;
            }
    klog("pci: %d devices\n", device_count);
}

int pci_count() { return device_count; }
PciDevice* pci_get(int i) { return &devices[i]; }

PciDevice* pci_find(u16 vendor, u16 device) {
    for (int i = 0; i < device_count; i++)
        if (devices[i].vendor == vendor && devices[i].device == device) return &devices[i];
    return nullptr;
}

void pci_enable_bus_master(PciDevice* d) {
    u32 cmd = pci_read(d->bus, d->dev, d->fn, 0x04);
    cmd |= (1 << 0) | (1 << 2);          // io space + bus master
    pci_write(d->bus, d->dev, d->fn, 0x04, cmd);
}
