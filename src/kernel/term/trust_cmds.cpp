// silicon - the trust shell command: certificate checking for HTTPS, mail and other TLS connections
#include "shell.h"
#include "../net/net.h"
#include "../fs/fs.h"

void cmd_trust(Shell& sh, int argc, char** argv) {
    const char* sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
        tls_verify_set(!strcmp(sub, "on"));
        sh.outf("certificate checking is now %s\n", sub);
        return;
    }
    if (!strcmp(sub, "add") && argc > 2) {
        char msg[160];
        if (tls_trust_add(argv[2], msg, sizeof(msg))) sh.outf("%s\n", msg);
        else sh.err("%s\n", msg);
        return;
    }
    if (!strcmp(sub, "remove") && argc > 2) {
        if (tls_trust_remove(argv[2])) sh.outf("%s is no longer pinned\n", argv[2]);
        else sh.err("nothing pinned for %s\n", argv[2]);
        return;
    }
    if (strcmp(sub, "status") && strcmp(sub, "list")) {
        sh.out("usage: trust [status|on|off|add <host>|remove <host>]\n"
               "  Certificates are checked against the built-in list of authorities. A server that fails can be\n"
               "  pinned: try to connect, then `trust add <host>` accepts exactly the certificate it just showed.\n");
        return;
    }
    sh.outf("certificate checking: %s\n", tls_verify_enabled() ? "on" : "off");
    FsNode* f = fs_lookup("/home/.config/tls_trust");
    if (!f || !f->size) { sh.out("no pinned certificates\n"); return; }
    sh.out("pinned certificates:\n");
    const char* t = fs_read_text(f);
    while (*t) {
        const char* e = strchr(t, '\n');
        int l = e ? (int)(e - t) : (int)strlen(t);
        char line[220];
        memcpy(line, t, min(l, 219));
        line[min(l, 219)] = 0;
        char* sp = strchr(line, ' ');
        if (sp) { *sp = 0; sh.outf("  %s  %.16s...\n", line, sp + 1); }
        if (!e) break;
        t = e + 1;
    }
}
