// silicon - PCI bus
#pragma once
#include "../core.h"

struct PciDevice {
    u8 bus, dev, fn;
    u16 vendor, device;
    u8 cls, subcls, irq;
    u32 bar[6];
    const char* name;
};

u32  pci_read(u8 bus, u8 dev, u8 fn, u8 off);
void pci_write(u8 bus, u8 dev, u8 fn, u8 off, u32 v);
void pci_scan();
int  pci_count();
PciDevice* pci_get(int i);
PciDevice* pci_find(u16 vendor, u16 device);
void pci_enable_bus_master(PciDevice* d);
