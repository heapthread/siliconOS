// Host test for src/kernel/net/x509.cpp:
//   g++ -O2 -DX509_HOST -x c++ tools/x509_host_test.cpp -o x509_test
//   x509_test chain <host> <leaf.der> [intermediate.der ...]       verify a chain as a server would send it
//   x509_test sig <cert.der> <scheme-hex> <message-file> <sig-file> verify one TLS signature with the certificate's key
#define X509_HOST 1
#include "../src/kernel/net/x509.cpp"
#include <time.h>
static u8* slurp(const char* path, u32* len) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "can't read %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8* b = (u8*)malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) exit(2);
    fclose(f);
    *len = (u32)n;
    return b;
}
int main(int argc, char** argv) {
    if (argc >= 4 && !strcmp(argv[1], "chain")) {
        const u8* certs[10]; u32 lens[10];
        int n = 0;
        for (int i = 3; i < argc && n < 10; i++) certs[n] = slurp(argv[i], &lens[n]), n++;
        char err[128] = "";
        u8 fp[32];
        i64 now = (i64)time(nullptr) - 946684800;
        bool ok = x509_verify_chain(certs, lens, n, argv[2], now, err, sizeof(err), fp);
        printf("%s %s %s\n", ok ? "TRUSTED" : "REJECTED", argv[2], ok ? "" : err);
        return ok ? 0 : 1;
    }
    if (argc == 6 && !strcmp(argv[1], "sig")) {
        u32 cl, ml, sl;
        u8* cert = slurp(argv[2], &cl);
        u8* msg = slurp(argv[4], &ml);
        u8* sig = slurp(argv[5], &sl);
        PubKey k;
        if (!x509_public_key(cert, cl, &k)) { printf("BADKEY\n"); return 1; }
        bool ok = x509_verify_tls_sig(k, (u16)strtol(argv[3], nullptr, 16), msg, ml, sig, sl);
        printf("%s\n", ok ? "VALID" : "INVALID");
        return ok ? 0 : 1;
    }
    if (argc == 4 && !strcmp(argv[1], "ecdh")) {                 // ecdh <priv hex> <peer pub hex>: prints our public key and the shared secret
        u8 priv[32], peer[65], pub[65], shared[32];
        for (int i = 0; i < 32; i++) sscanf(argv[2] + i * 2, "%2hhx", &priv[i]);
        for (int i = 0; i < 65; i++) sscanf(argv[3] + i * 2, "%2hhx", &peer[i]);
        bool a = p256_public(priv, pub), b = p256_shared(priv, peer, shared);
        printf("%d %d ", a, b);
        for (int i = 0; i < 65; i++) printf("%02x", pub[i]);
        printf(" ");
        for (int i = 0; i < 32; i++) printf("%02x", shared[i]);
        printf("\n");
        return 0;
    }
    return 2;
}
