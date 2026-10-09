// silicon - drive shell commands: lsblk, blkdump
#include "shell.h"
#include "../drv/blk.h"

void cmd_lsblk(Shell& sh, int, char**) {
    if (!usb_port_count()) sh.out(C_GRAY "no USB controller found (start QEMU with -device piix3-usb-uhci)\n" C_RESET);
    sh.out(C_BWHITE "name   size        state     model\n" C_RESET);
    for (int i = 0; i < blk_count(); i++) {
        BlkDev* d = blk_get(i);
        sh.outf("%-6s %6u MiB  %-8s  %s\n", d->name, (u32)(d->sectors / 2048), d->present ? "present" : "removed", d->model);
    }
    if (!blk_count()) sh.out(C_GRAY "no removable drives\n" C_RESET);
}

void cmd_blkdump(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: blkdump <drive> [sector]\n"); return; }
    for (int i = 0; i < blk_count(); i++) {
        BlkDev* d = blk_get(i);
        if (strcmp(d->name, argv[1]) || !d->present) continue;
        u8 buf[512];
        if (!d->read(d, argc > 2 ? (u64)atoi(argv[2]) : 0, 1, buf)) { sh.err("read failed\n"); return; }
        for (int r = 0; r < 8; r++) {
            sh.outf(C_GRAY "%04x  " C_RESET, r * 16);
            for (int c = 0; c < 16; c++) sh.outf("%02x ", buf[r * 16 + c]);
            sh.out(" ");
            for (int c = 0; c < 16; c++) { char ch[2] = {(char)(buf[r * 16 + c] >= 32 && buf[r * 16 + c] < 127 ? buf[r * 16 + c] : '.'), 0}; sh.out(ch); }
            sh.out("\n");
        }
        return;
    }
    sh.err("no such drive\n");
}

#include "../fs/drives.h"

void cmd_mount(Shell& sh, int, char**) {
    if (!drives_count()) { sh.out(C_GRAY "no drives mounted (plug one in; see lsblk)\n" C_RESET); return; }
    for (int i = 0; i < drives_count(); i++) {
        DriveInfo d;
        if (!drives_info(i, &d)) continue;
        sh.outf("%-6s on %-22s %s  %u of %u MB free%s\n", d.device, d.path, d.type, d.free_kb / 1024, d.total_kb / 1024, d.dirty ? C_BYEL "  (writing)" C_RESET : "");
    }
}

void cmd_eject(Shell& sh, int argc, char** argv) {
    if (argc < 2) { sh.out("usage: eject <drive name, label or /mnt path>\n"); return; }
    char msg[96];
    if (drive_eject(argv[1], msg, sizeof(msg))) sh.outf("%s\n", msg);
    else sh.err("%s\n", msg);
}
