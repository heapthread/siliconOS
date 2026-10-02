// silicon - COM1 debug log and the panic screen
#include "drivers.h"
#include "../arch/arch.h"

static bool serial_ok = false;

void serial_init() {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x01);     // 115200 baud
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
    serial_ok = true;
}

void serial_putc(char c) {
    if (!serial_ok) return;
    for (int i = 0; i < 10000 && !(inb(0x3F8 + 5) & 0x20); i++) {}
    outb(0x3F8, (u8)c);
}

void klog(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (char* p = buf; *p; p++) {
        if (*p == '\n') serial_putc('\r');
        serial_putc(*p);
    }
}

// ---------------------------------------------------------------- panic
static void panic_screen(const char* title, const char* msg, IntFrame* f) {
    if (!fb.lfb) for (;;) hlt();
    Canvas c;
    c.init(fb.lfb, fb.width, fb.height, fb.pitch);   // draw straight to the screen
    u32 bg = rgb(28, 20, 44), fg = rgb(240, 230, 255), accent = rgb(255, 110, 140);
    c.clear(bg);
    for (int y = 0; y < fb.height; y += 4) c.hline(0, y, fb.width, rgb(32, 24, 50));
    int x = 64, y = 64;
    c.text(x, y, G_CHIP " silicon has stopped", accent, 3, true);
    y += 60;
    c.text(x, y, title, fg, 2, true);
    y += 34;
    c.text(x, y, msg, fg, 1);
    y += 30;
    if (f) {
        char line[128];
        const char* names[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                               "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
        u64 vals[] = {f->rax, f->rbx, f->rcx, f->rdx, f->rsi, f->rdi, f->rbp, f->rsp,
                      f->r8, f->r9, f->r10, f->r11, f->r12, f->r13, f->r14, f->r15};
        snprintf(line, sizeof(line), "rip %016lx  err %016lx  rflags %08lx", f->rip, f->err, f->rflags);
        c.text(x, y, line, accent); y += 18;
        u64 cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        snprintf(line, sizeof(line), "cr2 %016lx  int %u", cr2, (u32)f->int_no);
        c.text(x, y, line, accent); y += 24;
        for (int i = 0; i < 16; i += 2) {
            snprintf(line, sizeof(line), "%-3s %016lx    %-3s %016lx", names[i], vals[i], names[i + 1], vals[i + 1]);
            c.text(x, y, line, fg);
            y += 16;
        }
    }
    c.text(x, fb.height - 48, "the machine is halted. restart qemu to try again.", rgb(150, 140, 180));
}

void panic_frame(const char* what, IntFrame* f) {
    cli();
    klog("\n*** CPU EXCEPTION: %s at rip=%lx err=%lx\n", what, f->rip, f->err);
    panic_screen("cpu exception", what, f);
    for (;;) hlt();
}

void panic(const char* fmt, ...) {
    cli();
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    klog("\n*** PANIC: %s\n", buf);
    panic_screen("kernel panic", buf, nullptr);
    for (;;) hlt();
}
