// silicon - GDT, IDT, PIC, PIT, exceptions
#include "arch.h"

// ---------------------------------------------------------------- GDT
static u64 gdt[5] = {
    0,
    0x00AF9A000000FFFFull,   // 0x08 kernel code 64
    0x00CF92000000FFFFull,   // 0x10 kernel data
    0,
    0,
};
struct __attribute__((packed)) DescPtr { u16 limit; u64 base; };
extern "C" void gdt_load(void* desc, u16 code, u16 data);

void gdt_init() {
    static DescPtr d;
    d.limit = sizeof(gdt) - 1;
    d.base = (u64)gdt;
    gdt_load(&d, 0x08, 0x10);
}

// ---------------------------------------------------------------- IDT
struct __attribute__((packed)) IdtEntry {
    u16 off_lo, sel;
    u8  ist, flags;
    u16 off_mid;
    u32 off_hi, zero;
};
static IdtEntry idt[256];
extern "C" u64 isr_table[48];
static IrqHandler irq_handlers[16];

static void pic_remap() {
    u8 m1 = 0xFF, m2 = 0xFF;
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();
    outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    outb(0x21, m1 & ~0x04);   // cascade line open
    outb(0xA1, m2);
}

void irq_unmask(int irq) {
    if (irq < 8) outb(0x21, inb(0x21) & ~(1 << irq));
    else outb(0xA1, inb(0xA1) & ~(1 << (irq - 8)));
}

void irq_set_handler(int irq, IrqHandler h) { irq_handlers[irq] = h; }

void idt_init() {
    for (int i = 0; i < 48; i++) {
        u64 a = isr_table[i];
        idt[i].off_lo = a & 0xFFFF;
        idt[i].sel = 0x08;
        idt[i].ist = 0;
        idt[i].flags = 0x8E;
        idt[i].off_mid = (a >> 16) & 0xFFFF;
        idt[i].off_hi = a >> 32;
        idt[i].zero = 0;
    }
    static DescPtr d;
    d.limit = sizeof(idt) - 1;
    d.base = (u64)idt;
    __asm__ volatile("lidt %0" :: "m"(d));
    pic_remap();
}

static const char* exc_names[32] = {
    "divide error", "debug", "nmi", "breakpoint", "overflow", "bound range", "invalid opcode",
    "device not available", "double fault", "coproc overrun", "invalid tss", "segment not present",
    "stack fault", "general protection fault", "page fault", "reserved", "x87 fault",
    "alignment check", "machine check", "simd fault", "virtualization", "control protection",
    "reserved", "reserved", "reserved", "reserved", "reserved", "reserved", "hypervisor injection",
    "vmm communication", "security", "reserved",
};

void panic_frame(const char* what, IntFrame* f);

extern "C" void isr_dispatch(IntFrame* f) {
    if (f->int_no < 32) {
        panic_frame(exc_names[f->int_no], f);
    }
    int irq = (int)f->int_no - 32;
    if (irq_handlers[irq]) irq_handlers[irq](f);
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

// ---------------------------------------------------------------- PIT + TSC clock
// Time comes from the TSC, calibrated against PIT channel 2 at boot. Counting PIT interrupts
// isn't reliable: under WHPX on Windows the host timer only fires ~64 times a second, so a
// 1 kHz tick count runs 15-20x slow. The PIT interrupt is still used to wake up from hlt.
static volatile u64 tick_ms = 0;
static u64 tsc0 = 0, tsc_per_ms = 0;
static volatile u64 idle_tsc = 0;

static inline u64 rdtsc() {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

void sound_tick();
static void pit_irq(IntFrame*) {
    tick_ms++;
    sound_tick();
}

// read PIT channel 2's counter together with the TSC at that moment. A port access can take a
// long time when the hypervisor is busy, so only reads that came back quickly are trusted
// (the TSC is taken as the midpoint of the read).
static bool pit2_read(u16* count, u64* tsc) {
    u64 best = ~0ull;
    for (int tries = 0; tries < 200; tries++) {
        u64 a = rdtsc();
        outb(0x43, 0x80);                           // latch counter 2
        u8 lo = inb(0x42), hi = inb(0x42);
        u64 b = rdtsc();
        if (b - a < best) { best = b - a; *count = (u16)(lo | (hi << 8)); *tsc = a + (b - a) / 2; }
        if (b - a < 200000) return true;
    }
    return best != ~0ull;
}

static u64 measure_tsc() {
    u16 c0, c;
    u64 t0, t;
    if (!pit2_read(&c0, &t0)) return 0;
    for (int i = 0; i < 2000000; i++) {
        if (!pit2_read(&c, &t)) return 0;
        u32 elapsed = (u16)(c0 - c);                // counts down, 16-bit wrap
        if (elapsed >= 36000) return (t - t0) * 1193182 / elapsed / 1000;   // ~30 ms
    }
    return 0;
}

static void calibrate_tsc() {
    // channel 2 free-running (mode 0 from 0xFFFF wraps every 55 ms), speaker output off
    u8 p = inb(0x61);
    outb(0x61, (p & ~0x02) | 0x01);
    outb(0x43, 0xB0);
    outb(0x42, 0xFF);
    outb(0x42, 0xFF);
    // three measurements, keep the median
    u64 m[3];
    for (int i = 0; i < 3; i++) m[i] = measure_tsc();
    outb(0x61, p & ~0x03);
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 3; j++)
            if (m[j] < m[i]) { u64 x = m[i]; m[i] = m[j]; m[j] = x; }
    if (m[1] > 100000) tsc_per_ms = m[1];
    klog("tsc: %u MHz%s\n", (u32)(tsc_per_ms / 1000), tsc_per_ms ? "" : " (calibration failed, using pit ticks)");
}

void pit_init(u32 hz) {
    calibrate_tsc();
    tsc0 = rdtsc();
    u32 div = 1193182 / hz;
    outb(0x43, 0x36);
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    irq_set_handler(0, pit_irq);
    irq_unmask(0);
}

u32 cpu_mhz() { return (u32)(tsc_per_ms / 1000); }

u64 ticks() { return tsc_per_ms ? (rdtsc() - tsc0) / tsc_per_ms : tick_ms; }
u64 idle_ticks() { return tsc_per_ms ? idle_tsc / tsc_per_ms : 0; }

// halt until the next interrupt (input, timer, ...): use when there's nothing to animate
void cpu_idle() {
    u64 a = rdtsc();
    __asm__ volatile("sti; hlt");
    idle_tsc += rdtsc() - a;
}

// stay awake but rest for about a millisecond: use while animating or moving data, because a
// hlt can oversleep by a whole host timer period (~15 ms on Windows)
void cpu_rest() {
    if (!tsc_per_ms) { cpu_idle(); return; }
    u64 a = rdtsc(), end = a + tsc_per_ms;
    while (rdtsc() < end) __asm__ volatile("pause");
    idle_tsc += rdtsc() - a;
}

void sleep_ms(u32 ms) {
    u64 end = ticks() + ms;
    while (ticks() < end) {
        if (end - ticks() > 25) cpu_idle();
        else cpu_rest();
    }
}

// ---------------------------------------------------------------- cpuid
static char vendor[13];
static char brand[49];

static void cpuid(u32 leaf, u32* a, u32* b, u32* c, u32* d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

const char* cpu_vendor() {
    if (!vendor[0]) {
        u32 a, b, c, d;
        cpuid(0, &a, &b, &c, &d);
        memcpy(vendor, &b, 4); memcpy(vendor + 4, &d, 4); memcpy(vendor + 8, &c, 4);
        vendor[12] = 0;
    }
    return vendor;
}

const char* cpu_brand() {
    if (!brand[0]) {
        u32 a, b, c, d;
        cpuid(0x80000000, &a, &b, &c, &d);
        if (a < 0x80000004) { strlcpy(brand, "unknown x86_64", sizeof(brand)); return brand; }
        u32* p = (u32*)brand;
        for (u32 leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid(leaf, &a, &b, &c, &d);
            *p++ = a; *p++ = b; *p++ = c; *p++ = d;
        }
        brand[48] = 0;
        char* s = brand;
        while (*s == ' ') s++;
        if (s != brand) memmove(brand, s, strlen(s) + 1);
    }
    return brand;
}
