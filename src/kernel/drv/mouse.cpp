// silicon - mouse: VMware/QEMU absolute pointer (vmmouse) with PS/2 relative fallback
#include "drivers.h"
#include "../arch/arch.h"

static volatile int mx, my, mwheel;
static volatile bool mleft, mright, mmiddle, changed;
static int sw = 1024, sh = 768;
static bool vmmouse = false;
static u8 packet[4];
static int pidx = 0, psize = 3;

// ---------------------------------------------------------------- vmmouse backdoor
struct VmRegs { u32 a, b, c, d; };
static VmRegs vmport(u32 cmd, u32 arg) {
    VmRegs r;
    __asm__ volatile("inl %%dx, %%eax"
                     : "=a"(r.a), "=b"(r.b), "=c"(r.c), "=d"(r.d)
                     : "a"(0x564D5868), "b"(arg), "c"(cmd), "d"(0x5658));
    return r;
}
enum { VM_GETVERSION = 10, VM_ABS_DATA = 39, VM_ABS_STATUS = 40, VM_ABS_COMMAND = 41 };

static bool vmmouse_enable() {
    VmRegs v = vmport(VM_GETVERSION, ~0u);
    if (v.b != 0x564D5868) return false;
    vmport(VM_ABS_COMMAND, 0x45414552);           // READ_ID
    v = vmport(VM_ABS_STATUS, 0);
    if ((v.a & 0xFFFF) == 0) return false;
    v = vmport(VM_ABS_DATA, 1);
    if (v.a != 0x3442554A) return false;
    vmport(VM_ABS_COMMAND, 0x53424152);           // REQUEST_ABSOLUTE
    return true;
}

static void vmmouse_poll() {
    for (;;) {
        VmRegs s = vmport(VM_ABS_STATUS, 0);
        if (s.a == 0xFFFF0000) { vmmouse_enable(); return; }
        if ((s.a & 0xFFFF) < 4) return;
        VmRegs d = vmport(VM_ABS_DATA, 4);
        u32 btn = d.a & 0xFFFF;
        mx = (int)((u64)d.b * sw / 0xFFFF);
        my = (int)((u64)d.c * sh / 0xFFFF);
        mleft = btn & 0x20;
        mright = btn & 0x10;
        mmiddle = btn & 0x08;
        mwheel -= (i8)(u8)d.d;          // positive = wheel up
        changed = true;
    }
}

// ---------------------------------------------------------------- PS/2
// time-bounded waits: under a busy hypervisor one port read can take milliseconds, so a fixed
// iteration count could stall boot for minutes when the controller never answers
static void wait_write() { u64 end = ticks() + 40; while ((inb(0x64) & 2) && ticks() < end) {} }
static void wait_read() { u64 end = ticks() + 40; while (!(inb(0x64) & 1) && ticks() < end) {} }
static void mouse_cmd(u8 v) {
    wait_write(); outb(0x64, 0xD4);
    wait_write(); outb(0x60, v);
    wait_read(); inb(0x60);   // ack
}

static void mouse_irq(IntFrame*) {
    u8 status = inb(0x64);
    if (!(status & 1)) return;
    u8 b = inb(0x60);
    if (vmmouse) vmmouse_poll();
    if (pidx == 0 && !(b & 0x08)) return;   // resync
    packet[pidx++] = b;
    if (pidx < psize) return;
    pidx = 0;
    int dx = packet[1] - ((packet[0] & 0x10) ? 256 : 0);
    int dy = packet[2] - ((packet[0] & 0x20) ? 256 : 0);
    if (packet[0] & 0xC0) { dx = dy = 0; }
    // relative packets also arrive in vmmouse mode (as empty "fake" packets, or from
    // hosts that only send relative motion), so motion is always applied
    if (dx || dy) {
        mx = clamp(mx + dx, 0, sw - 1);
        my = clamp(my - dy, 0, sh - 1);
        changed = true;
    }
    if (!vmmouse) {
        mleft = packet[0] & 1;
        mright = packet[0] & 2;
        mmiddle = packet[0] & 4;
        if (psize == 4) mwheel -= (i8)(packet[3] << 4) >> 4;
        changed = true;
    }
}

void mouse_init(int screen_w, int screen_h) {
    sw = screen_w; sh = screen_h;
    mx = sw / 2; my = sh / 2;
    wait_write(); outb(0x64, 0xA8);              // enable aux port
    wait_write(); outb(0x64, 0x20);
    wait_read(); u8 cfg = inb(0x60);
    cfg |= 0x02; cfg &= ~0x20;
    wait_write(); outb(0x64, 0x60);
    wait_write(); outb(0x60, cfg);
    mouse_cmd(0xF6);
    // IntelliMouse wheel magic: sample rate 200, 100, 80 then read id
    mouse_cmd(0xF3); mouse_cmd(200);
    mouse_cmd(0xF3); mouse_cmd(100);
    mouse_cmd(0xF3); mouse_cmd(80);
    wait_write(); outb(0x64, 0xD4);
    wait_write(); outb(0x60, 0xF2);
    wait_read(); inb(0x60);
    wait_read(); u8 id = inb(0x60);
    psize = (id == 3) ? 4 : 3;
    mouse_cmd(0xF4);
    vmmouse = vmmouse_enable();
    klog("mouse: %s, %d-byte packets\n", vmmouse ? "vmmouse absolute" : "ps/2 relative", psize);
    irq_set_handler(12, mouse_irq);
    irq_unmask(12);
}

bool mouse_update(MouseState& st) {
    u64 f = irq_save();
    bool c = changed;
    changed = false;
    st.x = mx; st.y = my;
    st.left = mleft; st.right = mright; st.middle = mmiddle;
    st.wheel = mwheel;
    mwheel = 0;
    irq_restore(f);
    return c;
}

bool mouse_absolute() { return vmmouse; }
void mouse_warp(int x, int y) { mx = x; my = y; }
