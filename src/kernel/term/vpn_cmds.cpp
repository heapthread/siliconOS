// silicon - vpn shell command
#include "shell.h"
#include "../net/net.h"

void vpn_save_text(const char* text);
void vpn_selftest(char* out, int n);

void cmd_vpn(Shell& sh, int argc, char** argv) {
    const char* sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "genkey")) {
        char priv[48], pub[48];
        vpn_make_keypair(priv, pub);
        sh.outf("private: %s\npublic:  %s\n", priv, pub);
        return;
    }
    if (!strcmp(sub, "selftest")) { char b[600]; vpn_selftest(b, sizeof(b)); sh.outf("%s\n", b); return; }
    if (!strcmp(sub, "setup")) {
        // vpn setup <endpoint host:port> <peer public key> <my address> [allowed ips] [private key]
        if (argc < 5) {
            sh.out("usage: vpn setup <host:port> <server public key> <my tunnel address> [allowed-ips] [my private key]\n"
                   "  allowed-ips defaults to 0.0.0.0/0 (everything goes through the VPN); a new private key is made when none is given\n");
            return;
        }
        char priv[48], pub[48];
        if (argc > 6) strlcpy(priv, argv[6], sizeof(priv)); else vpn_make_keypair(priv, pub);
        char cfg[600];
        snprintf(cfg, sizeof(cfg), "private=%s\npeer_public=%s\naddress=%s\nendpoint=%s\nallowed=%s\nkeepalive=15\nenabled=0\n", priv, argv[3], argv[4], argv[2],
                 argc > 5 ? argv[5] : "0.0.0.0/0");
        vpn_save_text(cfg);
        vpn_load();
        VpnInfo i;
        vpn_info(&i);
        sh.outf("saved. give the server this public key: " C_BGREEN "%s" C_RESET "\nthen run: vpn on\n", i.public_key);
        return;
    }
    if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
        VpnInfo i;
        vpn_info(&i);
        if (!i.configured) { sh.err("vpn: not set up (try: vpn setup)\n"); return; }
        vpn_set_enabled(sub[1] == 'n');
        sh.outf("vpn %s\n", sub[1] == 'n' ? "connecting" : "off");
        return;
    }
    VpnInfo i;
    vpn_info(&i);
    if (!i.configured) { sh.outf("vpn: %s\n(set it up with: vpn setup <host:port> <server public key> <my tunnel address>)\n", i.status); return; }
    sh.outf(C_BWHITE "vpn" C_RESET " %s  (%s)\n  server  %s\n  address %s\n  my key  %s\n", i.up ? C_BGREEN "connected" C_RESET : i.enabled ? C_BYEL "connecting" C_RESET : "off", i.status,
            i.endpoint, i.address, i.public_key);
    if (i.up) sh.outf("  sent %u bytes, received %u bytes, last packet from the server %u s ago\n", (u32)i.tx, (u32)i.rx, (u32)i.rx_age_s);
}
