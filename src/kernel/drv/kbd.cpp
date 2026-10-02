// silicon - PS/2 keyboard (scancode set 1, US layout)
#include "drivers.h"
#include "../arch/arch.h"

static const char map_lo[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 10, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0,
};
static const char map_hi[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 9,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 10, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0,
};

static const int QN = 64;
static KeyEvent queue[QN];
static volatile int q_head = 0, q_tail = 0;
static bool shift_l, shift_r, ctrl, alt, caps, ext;

static void push(const KeyEvent& e) {
    int n = (q_head + 1) % QN;
    if (n == q_tail) return;
    queue[q_head] = e;
    q_head = n;
}

static void kbd_irq(IntFrame*) {
    u8 sc = inb(0x60);
    if (sc == 0xE0) { ext = true; return; }
    bool down = !(sc & 0x80);
    u8 code = sc & 0x7F;
    KeyEvent e{};
    e.down = down;

    if (ext) {
        ext = false;
        switch (code) {
        case 0x48: e.key = K_UP; break;
        case 0x50: e.key = K_DOWN; break;
        case 0x4B: e.key = K_LEFT; break;
        case 0x4D: e.key = K_RIGHT; break;
        case 0x47: e.key = K_HOME; break;
        case 0x4F: e.key = K_END; break;
        case 0x49: e.key = K_PGUP; break;
        case 0x51: e.key = K_PGDN; break;
        case 0x52: e.key = K_INSERT; break;
        case 0x53: e.key = K_DELETE; break;
        case 0x1C: e.key = K_ENTER; e.ch = '\n'; break;
        case 0x1D: ctrl = down; e.key = K_RCTRL; break;
        case 0x38: alt = down; e.key = K_RALT; break;
        case 0x35: e.key = '/'; e.ch = '/'; break;
        case 0x5B: case 0x5C: e.key = K_SUPER; break;
        default: return;
        }
    } else {
        switch (code) {
        case 0x2A: shift_l = down; e.key = K_LSHIFT; break;
        case 0x36: shift_r = down; e.key = K_RSHIFT; break;
        case 0x1D: ctrl = down; e.key = K_LCTRL; break;
        case 0x38: alt = down; e.key = K_LALT; break;
        case 0x3A: if (down) caps = !caps; e.key = K_CAPS; break;
        case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44:
            e.key = K_F1 + (code - 0x3B); break;
        case 0x57: e.key = K_F11; break;
        case 0x58: e.key = K_F12; break;
        // keypad without numlock -> navigation
        case 0x48: e.key = K_UP; break;
        case 0x50: e.key = K_DOWN; break;
        case 0x4B: e.key = K_LEFT; break;
        case 0x4D: e.key = K_RIGHT; break;
        default: {
            bool shift = shift_l || shift_r;
            char c = shift ? map_hi[code] : map_lo[code];
            if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c = shift ? to_lower(c) : to_upper(c);
            if (!c) return;
            e.ch = c;
            e.key = (u16)(u8)c;
            if (c == 8) e.key = K_BACKSPACE;
            else if (c == 9) e.key = K_TAB;
            else if (c == 10) e.key = K_ENTER;
            else if (c == 27) { e.key = K_ESC; e.ch = 0; }
        }
        }
    }
    e.shift = shift_l || shift_r;
    e.ctrl = ctrl;
    e.alt = alt;
    if (e.ctrl && e.ch >= 'a' && e.ch <= 'z') e.key = (u16)e.ch;   // ctrl combos keep the letter
    if (e.ctrl && e.ch >= 'A' && e.ch <= 'Z') e.key = (u16)to_lower(e.ch);
    push(e);
}

void kbd_init() {
    while (inb(0x64) & 1) inb(0x60);
    irq_set_handler(1, kbd_irq);
    irq_unmask(1);
}

bool kbd_poll(KeyEvent& ev) {
    u64 f = irq_save();
    if (q_tail == q_head) { irq_restore(f); return false; }
    ev = queue[q_tail];
    q_tail = (q_tail + 1) % QN;
    irq_restore(f);
    return true;
}
