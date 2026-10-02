// silicon - boot information handed over by stage 2 (see src/boot/stage2.asm)
#pragma once
#include "core.h"

struct __attribute__((packed)) E820Entry {
    u64 base;
    u64 length;
    u32 type;
    u32 acpi;
};

struct __attribute__((packed)) BootInfo {
    u64 fb_addr;
    u32 fb_width;
    u32 fb_height;
    u32 fb_pitch;
    u32 fb_bpp;
    u32 boot_drive;
    u32 kernel_sectors;
    u32 e820_count;
    u32 reserved;
    E820Entry e820[64];
};
