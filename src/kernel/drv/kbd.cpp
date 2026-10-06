// silicon - PS/2 keyboard (scancode set 1) with several layouts, AltGr and dead keys.
// Every key press becomes a Unicode code point (KeyEvent::cp); `ch` and `key` carry the closest
// silicon-codepage byte so older apps keep working with accented letters collapsed to plain ones.
#include "drivers.h"
#include "../arch/arch.h"
#include "../settings.h"
#include "../text.h"

enum : char16_t { D_ACUTE = 0xE001, D_GRAVE, D_CIRC, D_DIAER, D_TILDE };      // dead keys (private use)

// Each layout has four levels (plain, shift, AltGr, AltGr+shift) of 48 keys in this scancode order:
//   0x02-0x0D (12), 0x10-0x1B (12), 0x1E-0x29 (12), 0x2B (1), 0x2C-0x35 (10), 0x56 (1)
struct Layout { const char* name; const char* label; const char16_t* lv[4]; bool altgr; };

#define Z u"\0"
static const Layout layouts[] = {
    {"us", "English (US)",
     {u"1234567890-=" u"qwertyuiop[]" u"asdfghjkl;'`" u"\\" u"zxcvbnm,./" u"\0",
      u"!@#$%^&*()_+" u"QWERTYUIOP{}" u"ASDFGHJKL:\"~" u"|" u"ZXCVBNM<>?" u"\0",
      nullptr, nullptr}, false},
    {"uk", "English (UK)",
     {u"1234567890-=" u"qwertyuiop[]" u"asdfghjkl;'`" u"#" u"zxcvbnm,./" u"\\",
      u"!\"£$%^&*()_+" u"QWERTYUIOP{}" u"ASDFGHJKL:@¬" u"~" u"ZXCVBNM<>?" u"|",
      u"\0\0\0€\0\0\0\0\0\0\0\0" u"\0\0é\0\0\0úíó\0\0\0" u"á\0\0\0\0\0\0\0\0\0\0\0" u"\0" u"\0\0\0\0\0\0\0\0\0\0" u"\0",
      nullptr}, true},
    {"de", "Deutsch (QWERTZ)",
     {u"1234567890ß" u"qwertzuiopü+" u"asdfghjklöä" u"#" u"yxcvbnm,.-" u"<",
      u"!\"§$%&/()=?" u"QWERTZUIOPÜ*" u"ASDFGHJKLÖÄ°" u"'" u"YXCVBNM;:_" u">",
      u"\0²³\0\0\0{[]}\\\0" u"@\0€\0\0\0\0\0\0\0\0~" u"\0\0\0\0\0\0\0\0\0\0\0\0" u"\0" u"\0\0\0\0\0\0µ\0\0\0" u"|",
      nullptr}, true},
    {"fr", "Francais (AZERTY)",
     {u"&é\"'(-è_çà)=" u"azertyuiop$" u"qsdfghjklmù²" u"*" u"wxcvbn,;:!" u"<",
      u"1234567890°+" u"AZERTYUIOP£" u"QSDFGHJKLM%\0" u"µ" u"WXCVBN?./§" u">",
      u"\0~#{[|`\\^@]}" u"\0\0€\0\0\0\0\0\0\0\0\0" u"\0\0\0\0\0\0\0\0\0\0\0\0" u"\0" u"\0\0\0\0\0\0\0\0\0\0" u"\0",
      nullptr}, true},
    {"es", "Espanol",
     {u"1234567890'¡" u"qwertyuiop+" u"asdfghjklñº" u"ç" u"zxcvbnm,.-" u"<",
      u"!\"·$%&/()=?¿" u"QWERTYUIOP*" u"ASDFGHJKLÑª" u"Ç" u"ZXCVBNM;:_" u">",
      u"|@#~€¬\0\0\0\0\0\0" u"\0\0€\0\0\0\0\0\0\0[]" u"\0\0\0\0\0\0\0\0\0\0{\\" u"}" u"\0\0\0\0\0\0\0\0\0\0" u"\0",
      nullptr}, true},
    {"dvorak", "Dvorak",
     {u"1234567890[]" u"',.pyfgcrl/=" u"aoeuidhtns-`" u"\\" u";qjkxbmwvz" u"\0",
      u"!@#$%^&*(){}" u"\"<>PYFGCRL?+" u"AOEUIDHTNS_~" u"|" u":QJKXBMWVZ" u"\0",
      nullptr, nullptr}, false},
};
static const int N_LAYOUTS = (int)(sizeof(layouts) / sizeof(layouts[0]));

int kbd_layout_count() { return N_LAYOUTS; }
const char* kbd_layout_name(int i) { return layouts[clamp(i, 0, N_LAYOUTS - 1)].name; }
const char* kbd_layout_label(int i) { return layouts[clamp(i, 0, N_LAYOUTS - 1)].label; }

static int key_index(u8 code) {
    if (code >= 0x02 && code <= 0x0D) return code - 0x02;
    if (code >= 0x10 && code <= 0x1B) return 12 + code - 0x10;
    if (code >= 0x1E && code <= 0x29) return 24 + code - 0x1E;
    if (code == 0x2B) return 36;
    if (code >= 0x2C && code <= 0x35) return 37 + code - 0x2C;
    if (code == 0x56) return 47;
    return -1;
}

// dead key + letter -> composed character
static u32 compose(u32 dead, u32 base) {
    static const struct { char16_t dead; char16_t base; char16_t out; } t[] = {
        {D_ACUTE, 'a', 0xE1}, {D_ACUTE, 'e', 0xE9}, {D_ACUTE, 'i', 0xED}, {D_ACUTE, 'o', 0xF3}, {D_ACUTE, 'u', 0xFA}, {D_ACUTE, 'y', 0xFD},
        {D_ACUTE, 'A', 0xC1}, {D_ACUTE, 'E', 0xC9}, {D_ACUTE, 'I', 0xCD}, {D_ACUTE, 'O', 0xD3}, {D_ACUTE, 'U', 0xDA}, {D_ACUTE, 'Y', 0xDD},
        {D_GRAVE, 'a', 0xE0}, {D_GRAVE, 'e', 0xE8}, {D_GRAVE, 'i', 0xEC}, {D_GRAVE, 'o', 0xF2}, {D_GRAVE, 'u', 0xF9},
        {D_GRAVE, 'A', 0xC0}, {D_GRAVE, 'E', 0xC8}, {D_GRAVE, 'I', 0xCC}, {D_GRAVE, 'O', 0xD2}, {D_GRAVE, 'U', 0xD9},
        {D_CIRC, 'a', 0xE2}, {D_CIRC, 'e', 0xEA}, {D_CIRC, 'i', 0xEE}, {D_CIRC, 'o', 0xF4}, {D_CIRC, 'u', 0xFB},
        {D_CIRC, 'A', 0xC2}, {D_CIRC, 'E', 0xCA}, {D_CIRC, 'I', 0xCE}, {D_CIRC, 'O', 0xD4}, {D_CIRC, 'U', 0xDB},
        {D_DIAER, 'a', 0xE4}, {D_DIAER, 'e', 0xEB}, {D_DIAER, 'i', 0xEF}, {D_DIAER, 'o', 0xF6}, {D_DIAER, 'u', 0xFC}, {D_DIAER, 'y', 0xFF},
        {D_DIAER, 'A', 0xC4}, {D_DIAER, 'E', 0xCB}, {D_DIAER, 'I', 0xCF}, {D_DIAER, 'O', 0xD6}, {D_DIAER, 'U', 0xDC},
        {D_TILDE, 'a', 0xE3}, {D_TILDE, 'n', 0xF1}, {D_TILDE, 'o', 0xF5}, {D_TILDE, 'A', 0xC3}, {D_TILDE, 'N', 0xD1}, {D_TILDE, 'O', 0xD5},
    };
    for (auto& e : t) if (e.dead == dead && e.base == base) return e.out;
    return 0;
}
static u32 dead_literal(u32 dead) {
    switch (dead) { case D_ACUTE: return 0xB4; case D_GRAVE: return '`'; case D_CIRC: return '^'; case D_DIAER: return 0xA8; default: return '~'; }
}
static u32 upper_cp(u32 c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    return c;
}

static const int QN = 64;
static KeyEvent queue[QN];
static volatile int q_head = 0, q_tail = 0;
static bool shift_l, shift_r, ctrl, alt, altgr, caps, ext;
static u32 pending_dead = 0;

static void push(const KeyEvent& e) {
    int n = (q_head + 1) % QN;
    if (n == q_tail) return;
    queue[q_head] = e;
    q_head = n;
}

static void set_char(KeyEvent& e, u32 cp) {
    e.cp = cp;
    if (cp < 128) { e.ch = (char)cp; return; }
    char g[4];
    int n = glyph_for(cp, g);
    e.ch = n == 1 ? g[0] : 0;
}

static void kbd_irq(IntFrame*) {
    u8 sc = inb(0x60);
    if (sc == 0xE0) { ext = true; return; }
    bool down = !(sc & 0x80);
    u8 code = sc & 0x7F;
    KeyEvent e{};
    e.down = down;
    const Layout& L = layouts[clamp(settings.kbd_layout, 0, N_LAYOUTS - 1)];

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
        case 0x1C: e.key = K_ENTER; e.ch = '\n'; e.cp = '\n'; break;
        case 0x1D: ctrl = down; e.key = K_RCTRL; break;
        case 0x38:
            if (L.altgr) altgr = down; else alt = down;
            e.key = K_RALT;
            break;
        case 0x35: e.key = '/'; e.ch = '/'; e.cp = '/'; break;
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
        case 0x01: e.key = K_ESC; break;
        case 0x0E: e.key = K_BACKSPACE; e.ch = 8; e.cp = 8; break;
        case 0x0F: e.key = K_TAB; e.ch = 9; e.cp = 9; break;
        case 0x1C: e.key = K_ENTER; e.ch = 10; e.cp = 10; break;
        case 0x39: e.key = ' '; e.ch = ' '; e.cp = ' '; break;
        case 0x37: e.key = '*'; e.ch = '*'; e.cp = '*'; break;
        default: {
            int ix = key_index(code);
            if (ix < 0) return;
            bool shift = shift_l || shift_r;
            int level = (altgr && L.altgr) ? (shift ? 3 : 2) : (shift ? 1 : 0);
            const char16_t* tab = L.lv[level];
            if (!tab && level >= 2) tab = L.lv[shift ? 1 : 0];       // no AltGr table: the plain character
            u32 c = tab ? tab[ix] : 0;
            if (!c) return;
            if (!down) { e.key = (u16)(c < 128 ? c : 0); set_char(e, c < 0xE000 ? c : 0); break; }
            bool letter = (c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7);
            if (caps && letter && !shift) c = upper_cp(c);
            else if (caps && shift && ((c >= 'A' && c <= 'Z'))) c = c + 32;
            if (c >= 0xE001 && c <= 0xE005) {                       // a dead key: wait for the next one
                if (pending_dead) { u32 lit = dead_literal(pending_dead); pending_dead = 0; set_char(e, lit); e.key = (u16)(e.ch ? (u8)e.ch : 0); }
                else { pending_dead = c; return; }
            } else {
                if (pending_dead) {
                    u32 comp = compose(pending_dead, c);
                    if (!comp && c == ' ') comp = dead_literal(pending_dead);
                    if (!comp) {                                    // no such letter: the accent itself comes first
                        KeyEvent lit{};
                        lit.down = true;
                        set_char(lit, dead_literal(pending_dead));
                        lit.key = (u16)(lit.ch ? (u8)lit.ch : 0);
                        lit.shift = shift_l || shift_r; lit.ctrl = ctrl; lit.alt = alt;
                        push(lit);
                        comp = c;
                    }
                    pending_dead = 0;
                    c = comp;
                }
                set_char(e, c);
                e.key = (u16)(c < 128 ? c : (u8)e.ch);
            }
            if (e.ch == 8) e.key = K_BACKSPACE;
            else if (e.ch == 9) e.key = K_TAB;
            else if (e.ch == 10) e.key = K_ENTER;
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

bool kbd_shift_down() { return shift_l || shift_r; }
bool kbd_ctrl_down() { return ctrl; }
bool kbd_alt_down() { return alt; }

bool kbd_poll(KeyEvent& ev) {
    u64 f = irq_save();
    if (q_tail == q_head) { irq_restore(f); return false; }
    ev = queue[q_tail];
    q_tail = (q_tail + 1) % QN;
    irq_restore(f);
    return true;
}
