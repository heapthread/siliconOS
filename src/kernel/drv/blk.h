// silicon - block devices other than the system disk: USB mass storage drives
#pragma once
#include "../core.h"

struct BlkDev {
    char name[12];                  // "usb0"
    char model[40];
    u64  sectors;
    u32  sector_size;
    bool present;                   // plugged in right now
    bool removable;
    u32  serial;                    // bumps on every new insertion
    bool (*read)(BlkDev* d, u64 lba, u32 count, void* buf);
    bool (*write)(BlkDev* d, u64 lba, u32 count, const void* buf);
    void* priv;
};

int     blk_count();
BlkDev* blk_get(int i);
BlkDev* blk_add(const char* name);          // a slot for this name (reused when it was seen before)
u32     blk_generation();                   // changes when a device appears or disappears
void    blk_changed();

void usb_init();                            // find USB controllers (UHCI)
void usb_poll();                            // notice drives being plugged in or pulled out
int  usb_port_count();
