// silicon - power off, reboot, pc speaker
#include "drivers.h"
#include "../arch/arch.h"

void power_off() {
    cli();
    outw(0x604, 0x2000);     // QEMU (piix4 / q35 ACPI)
    outw(0xB004, 0x2000);    // Bochs / old QEMU
    outw(0x4004, 0x3400);    // VirtualBox
    for (;;) hlt();
}

void power_reboot() {
    cli();
    for (int i = 0; i < 1000 && (inb(0x64) & 2); i++) {}
    outb(0x64, 0xFE);
    // fall back to a triple fault
    struct __attribute__((packed)) { u16 l; u64 b; } none = {0, 0};
    __asm__ volatile("lidt %0; int3" :: "m"(none));
    for (;;) hlt();
}

// ---------------------------------------------------------------- non-blocking pc speaker
// notes are queued and played from the timer interrupt (sound_tick); lengths are measured with
// ticks() so they stay right even when timer interrupts arrive late
struct Note { u16 hz, ms; };
static Note notes[64];
static volatile int n_head = 0, n_tail = 0;
static volatile u64 note_end = 0;
static volatile bool playing = false;

static void tone(u32 hz) {
    if (!hz) { outb(0x61, inb(0x61) & 0xFC); return; }
    u32 div = 1193180 / hz;
    outb(0x43, 0xB6);
    outb(0x42, div & 0xFF);
    outb(0x42, div >> 8);
    outb(0x61, inb(0x61) | 3);
}

void sound_tick() {
    u64 t = ticks();
    if (playing) {
        if (t < note_end) return;
        playing = false;
        tone(0);
    }
    if (n_tail == n_head) return;
    Note n = notes[n_tail];
    n_tail = (n_tail + 1) % 64;
    tone(n.hz);
    playing = true;
    note_end = t + (n.ms ? n.ms : 1);
}

void speaker_beep(u32 hz, u32 ms) {
    u64 f = irq_save();
    int nx = (n_head + 1) % 64;
    if (nx != n_tail) { notes[n_head] = Note{(u16)hz, (u16)ms}; n_head = nx; }
    irq_restore(f);
}

// play a note right now, cutting off whatever was queued (instruments want this)
void speaker_play_now(u32 hz, u32 ms) {
    u64 f = irq_save();
    n_head = n_tail = 0;
    tone(hz);
    playing = true;
    note_end = ticks() + ms;
    irq_restore(f);
}
