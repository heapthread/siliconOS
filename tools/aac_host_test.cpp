// Host test for src/kernel/media/aac.cpp:  g++ -O2 -DAAC_HOST -x c++ tools/aac_host_test.cpp -o aac_test
//   aac_test in.aac out.s16   decodes an ADTS stream to raw 16-bit stereo (compare with ffmpeg)
#define AAC_HOST 1
#include "../src/kernel/media/aac.cpp"
int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8* d = (u8*)malloc(n); fread(d, 1, n, f); fclose(f);
    FILE* o = fopen(argv[2], "wb");
    long pos = 0; AacDec* dec = nullptr; int frames = 0, bad = 0;
    char err[80];
    while (pos + 7 < n) {
        if (d[pos] != 0xFF || (d[pos + 1] & 0xF6) != 0xF0) { pos++; continue; }
        int prot = d[pos + 1] & 1, prof = (d[pos + 2] >> 6) & 3, sri = (d[pos + 2] >> 2) & 15, ch = ((d[pos + 2] & 1) << 2) | (d[pos + 3] >> 6);
        long fl = ((d[pos + 3] & 3) << 11) | (d[pos + 4] << 3) | (d[pos + 5] >> 5);
        int hdr = prot ? 7 : 9;
        if (!dec) { dec = aac_open(prof + 1, sri, ch, err, sizeof(err)); if (!dec) { printf("open: %s\n", err); return 1; } printf("rate %d ch %d\n", aac_rate(dec), ch); }
        static i16 pcm[2048];
        if (!aac_decode(dec, d + pos + hdr, fl - hdr, pcm)) { bad++; memset(pcm, 0, sizeof(pcm)); }
        fwrite(pcm, 4, 1024, o);
        frames++; pos += fl;
    }
    printf("%d frames, %d bad\n", frames, bad);
    fclose(o);
    return 0;
}
