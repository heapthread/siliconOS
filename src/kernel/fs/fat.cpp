// silicon - FAT16 / FAT32 (see fat.h)
#include "fat.h"
#include "../text.h"
#include "../drv/drivers.h"

static const u32 MAX_FILE = 48u << 20;           // bigger files are listed but not loaded
static const u32 MAX_IMPORT = 200u << 20;

struct DirEnt { char name[FS_NAME_MAX]; bool dir; u32 first, size, mtime; u8 attr; };
struct Del { u32 dir_cluster; char name[FS_NAME_MAX]; };

struct FatVol {
    int id;
    BlkDev* dev;
    u32 part_lba, spc, reserved, nfats, fat_sectors, root_cluster, root_dir_sector, root_dir_sectors, data_start, clusters;
    int type;                                    // 16 or 32
    u32* fat;
    u8* fat_dirty;                               // one flag per FAT sector
    u32 hint;
    bool ro, dirty;
    FsNode* root;
    Vec<Del> dels;
    Vec<u32> to_free;                            // chains to free on the next sync
    u32 imported;
    u32 fsinfo;
    char label[12];
};
static FatVol* vols[4];

static inline u32 rd16(const u8* p) { return p[0] | (p[1] << 8); }
static inline u32 rd32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static inline void wr16(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static inline void wr32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

FatVol* fat_vol_by_id(int id) { return id >= 1 && id <= 4 ? vols[id - 1] : nullptr; }
bool fat_readonly(FatVol* v) { return v->ro; }
bool fat_dirty(FatVol* v) { return v->dirty || v->dels.len || v->to_free.len; }
const char* fat_type_name(FatVol* v) { return v->type == 32 ? "FAT32" : "FAT16"; }

static bool dread(FatVol* v, u32 sector, u32 count, void* buf) { return v->dev->read(v->dev, (u64)v->part_lba + sector, count, buf); }
static bool dwrite(FatVol* v, u32 sector, u32 count, const void* buf) { return v->dev->write(v->dev, (u64)v->part_lba + sector, count, buf); }

// ---------------------------------------------------------------- dates
static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static u32 fat_to_stamp(u32 date, u32 time) {
    int y = 1980 + (int)(date >> 9), m = (int)((date >> 5) & 15), d = (int)(date & 31);
    if (y < 2000 || m < 1 || m > 12) return 0;
    static const int md[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    u32 days = 0;
    for (int i = 2000; i < y; i++) days += leap(i) ? 366 : 365;
    for (int i = 1; i < m; i++) days += md[i - 1] + (i == 2 && leap(y));
    days += (u32)max(d, 1) - 1;
    return days * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
}
static void stamp_to_fat(u32 st, u16* date, u16* time) {
    DateTime dt;
    stamp_to_datetime(st, dt);
    *date = (u16)(((dt.year - 1980) << 9) | (dt.month << 5) | dt.day);
    *time = (u16)((dt.hour << 11) | (dt.minute << 5) | (dt.second / 2));
}

// ---------------------------------------------------------------- the FAT in memory
static u32 fat_get(FatVol* v, u32 c) { return c < v->clusters + 2 ? v->fat[c] : 0x0FFFFFFF; }
static void fat_set(FatVol* v, u32 c, u32 val) {
    if (c >= v->clusters + 2) return;
    v->fat[c] = val;
    u32 sec = (c * (v->type == 32 ? 4 : 2)) / 512;
    if (sec < v->fat_sectors) v->fat_dirty[sec] = 1;
    v->dirty = true;
}
static bool is_eoc(FatVol* v, u32 c) { return c >= (v->type == 32 ? 0x0FFFFFF8u : 0xFFF8u); }
static u32 eoc(FatVol* v) { return v->type == 32 ? 0x0FFFFFFFu : 0xFFFFu; }

static u32 alloc_cluster(FatVol* v) {
    for (u32 n = 0; n < v->clusters; n++) {
        u32 c = 2 + (v->hint + n) % v->clusters;
        if (v->fat[c] == 0) { v->hint = c - 1; return c; }
    }
    return 0;
}

static u32 alloc_chain(FatVol* v, u32 n) {                // first cluster, or 0
    u32 first = 0, prev = 0;
    for (u32 i = 0; i < n; i++) {
        u32 c = alloc_cluster(v);
        if (!c) {                                          // out of space: give back what we took
            if (first) { for (u32 x = first; x && !is_eoc(v, x);) { u32 nx = fat_get(v, x); fat_set(v, x, 0); x = nx; } }
            return 0;
        }
        fat_set(v, c, eoc(v));
        if (prev) fat_set(v, prev, c); else first = c;
        prev = c;
    }
    return first;
}

static void free_chain_all(FatVol* v, u32 c) {            // including the final cluster
    u32 guard = 0;
    while (c >= 2 && c < v->clusters + 2 && guard++ < v->clusters) {
        u32 nx = fat_get(v, c);
        bool last = is_eoc(v, nx) || nx == 0 || nx < 2;
        fat_set(v, c, 0);
        if (last) break;
        c = nx;
    }
}

static u32 csize(FatVol* v) { return v->spc * 512; }
static u32 csector(FatVol* v, u32 c) { return v->data_start + (c - 2) * v->spc; }

static bool read_chain(FatVol* v, u32 first, u8* out, u32 bytes) {
    u32 got = 0, c = first, guard = 0;
    while (got < bytes && c >= 2 && c < v->clusters + 2 && guard++ < v->clusters + 2) {
        // merge physically contiguous clusters into one read
        u32 run = 1;
        while (got + run * csize(v) < bytes) {
            u32 nx = fat_get(v, c + run - 1);
            if (nx != c + run) break;
            run++;
        }
        u32 n = min(run * csize(v), bytes - got);
        u32 secs = (n + 511) / 512;
        u8* tmp = (n % 512 || true) ? (u8*)kmalloc_try(secs * 512) : nullptr;
        if (!tmp) return false;
        bool ok = dread(v, csector(v, c), secs, tmp);
        memcpy(out + got, tmp, n);
        kfree(tmp);
        if (!ok) return false;
        got += n;
        c = fat_get(v, c + run - 1);
    }
    return got >= bytes;
}

static bool write_chain(FatVol* v, u32 first, const u8* data, u32 bytes) {
    u32 put = 0, c = first, guard = 0;
    while (put < bytes && c >= 2 && c < v->clusters + 2 && guard++ < v->clusters + 2) {
        u32 run = 1;
        while (put + run * csize(v) < bytes) {
            if (fat_get(v, c + run - 1) != c + run) break;
            run++;
        }
        u32 n = min(run * csize(v), bytes - put);
        u32 secs = (n + 511) / 512;
        u8* tmp = (u8*)kcalloc_try(secs * 512);
        if (!tmp) return false;
        memcpy(tmp, data + put, n);
        bool ok = dwrite(v, csector(v, c), secs, tmp);
        kfree(tmp);
        if (!ok) return false;
        put += n;
        c = fat_get(v, c + run - 1);
    }
    return true;
}

// ---------------------------------------------------------------- directories
// raw bytes of a directory: its cluster chain, or the fixed root area of FAT16 (cluster 0)
static bool dir_load(FatVol* v, u32 cluster, u8** out, u32* len) {
    if (cluster == 0 && v->type == 16) {
        *len = v->root_dir_sectors * 512;
        *out = (u8*)kmalloc_try(*len + 32);
        return *out && dread(v, v->root_dir_sector, v->root_dir_sectors, *out);
    }
    u32 n = 0, c = cluster, guard = 0;
    while (c >= 2 && c < v->clusters + 2 && guard++ < 100000) { n++; c = fat_get(v, c); if (is_eoc(v, c)) break; }
    if (!n) return false;
    *len = n * csize(v);
    *out = (u8*)kmalloc_try(*len + 32);
    return *out && read_chain(v, cluster, *out, *len);
}

static bool dir_store(FatVol* v, u32 cluster, const u8* raw, u32 len) {
    if (cluster == 0 && v->type == 16) return dwrite(v, v->root_dir_sector, v->root_dir_sectors, raw);
    return write_chain(v, cluster, raw, len);
}

static void utf16_to_name(const u16* u, int n, char* out) {
    int o = 0;
    for (int i = 0; i < n && o < FS_NAME_MAX - 4; i++) {
        u32 cp = u[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n) { cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00); i++; }
        o += utf8_encode(cp, out + o);
    }
    out[o] = 0;
}

static Vec<DirEnt> dir_entries(const u8* raw, u32 len) {
    Vec<DirEnt> out;
    u16 lfn[260];
    int lfn_n = 0;
    bool have_lfn = false;
    for (u32 i = 0; i + 32 <= len; i += 32) {
        const u8* e = raw + i;
        if (e[0] == 0) break;
        if (e[0] == 0xE5) { have_lfn = false; continue; }
        u8 attr = e[11];
        if (attr == 0x0F) {
            int seq = e[0] & 0x1F;
            if (e[0] & 0x40) { have_lfn = true; lfn_n = seq * 13; memset(lfn, 0, sizeof(lfn)); }
            if (have_lfn && seq >= 1 && seq <= 20) {
                static const int offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int k = 0; k < 13; k++) lfn[(seq - 1) * 13 + k] = (u16)rd16(e + offs[k]);
            }
            continue;
        }
        if (attr & 0x08) { have_lfn = false; continue; }                  // volume label
        DirEnt d;
        memset(&d, 0, sizeof(d));
        if (have_lfn) {
            int n = 0;
            while (n < lfn_n && lfn[n] && lfn[n] != 0xFFFF) n++;
            utf16_to_name(lfn, n, d.name);
        } else {
            char base[9], ext[4];
            memcpy(base, e, 8); base[8] = 0;
            memcpy(ext, e + 8, 3); ext[3] = 0;
            if ((u8)base[0] == 0x05) base[0] = (char)0xE5;
            for (int k = 7; k >= 0 && base[k] == ' '; k--) base[k] = 0;
            for (int k = 2; k >= 0 && ext[k] == ' '; k--) ext[k] = 0;
            if (e[12] & 0x08) for (char* p = base; *p; p++) *p = to_lower(*p);
            if (e[12] & 0x10) for (char* p = ext; *p; p++) *p = to_lower(*p);
            snprintf(d.name, sizeof(d.name), "%s%s%s", base, ext[0] ? "." : "", ext);
        }
        have_lfn = false;
        if (!strcmp(d.name, ".") || !strcmp(d.name, "..") || !d.name[0]) continue;
        d.attr = attr;
        d.dir = (attr & 0x10) != 0;
        d.first = rd16(e + 26) | (rd16(e + 20) << 16);
        d.size = d.dir ? 0 : rd32(e + 28);
        d.mtime = fat_to_stamp(rd16(e + 24), rd16(e + 22));
        out.push(d);
    }
    return out;
}

// ---------------------------------------------------------------- import
static void import_dir(FatVol* v, FsNode* node, u32 cluster, int depth) {
    u8* raw = nullptr;
    u32 len = 0;
    if (!dir_load(v, cluster, &raw, &len)) { klog("fat: unreadable directory %u\n", cluster); return; }
    Vec<DirEnt> ents = dir_entries(raw, len);
    kfree(raw);
    for (int i = 0; i < ents.len; i++) {
        DirEnt& d = ents[i];
        char nm[FS_NAME_MAX];
        fs_unique_name(node, d.name, nm, sizeof(nm));
        if (!fs_valid_name(nm)) continue;
        FsNode* n = fs_create(node, nm, d.dir);
        if (!n) continue;
        n->fat_vol = (u8)v->id;
        n->fat_first = d.first;
        n->fat_dir = cluster;
        n->fat_state = FAT_SYNCED;
        n->mtime = n->ctime = d.mtime ? d.mtime : n->mtime;
        if (d.dir) {
            if (depth < 24 && d.first >= 2) import_dir(v, n, d.first, depth + 1);
            continue;
        }
        if (d.size > MAX_FILE || v->imported + d.size > MAX_IMPORT) { n->fat_state = FAT_OVERSIZE; continue; }
        if (d.size && d.first >= 2) {
            u8* buf = (u8*)kmalloc_try(d.size + 512);
            if (!buf) { n->fat_state = FAT_OVERSIZE; continue; }
            if (read_chain(v, d.first, buf, d.size)) {
                fs_write(n, buf, d.size);
                n->fat_state = FAT_SYNCED;
                n->mtime = d.mtime ? d.mtime : n->mtime;
                v->imported += d.size;
            } else n->fat_state = FAT_OVERSIZE;
            kfree(buf);
        }
    }
    ents.free();
}

FatVol* fat_mount(BlkDev* dev, FsNode* mount_point, char* label_out, char* err, int errlen) {
    int slot = -1;
    for (int i = 0; i < 4; i++) if (!vols[i]) { slot = i; break; }
    if (slot < 0) { snprintf(err, errlen, "too many drives mounted"); return nullptr; }
    u8 sec[512];
    FatVol* v = new FatVol();
    v->dev = dev;
    if (!dev->read(dev, 0, 1, sec)) { snprintf(err, errlen, "can't read the drive"); delete v; return nullptr; }
    auto is_bpb = [](const u8* s) { return s[510] == 0x55 && s[511] == 0xAA && rd16(s + 11) == 512 && s[13] && !(s[13] & (s[13] - 1)) && s[16] > 0; };
    if (!is_bpb(sec)) {
        if (sec[510] != 0x55 || sec[511] != 0xAA) { snprintf(err, errlen, "no file system found"); delete v; return nullptr; }
        u32 lba = 0;
        for (int p = 0; p < 4; p++) {
            const u8* pe = sec + 446 + p * 16;
            u8 t = pe[4];
            if (t == 0x0B || t == 0x0C || t == 0x0E || t == 0x06 || t == 0x04 || t == 0x01) { lba = rd32(pe + 8); break; }
        }
        if (!lba || !dev->read(dev, lba, 1, sec) || !is_bpb(sec)) { snprintf(err, errlen, "no FAT partition found"); delete v; return nullptr; }
        v->part_lba = lba;
    }
    v->spc = sec[13];
    v->reserved = rd16(sec + 14);
    v->nfats = sec[16];
    u32 root_entries = rd16(sec + 17);
    u32 total = rd16(sec + 19) ? rd16(sec + 19) : rd32(sec + 32);
    u32 fatsz = rd16(sec + 22) ? rd16(sec + 22) : rd32(sec + 36);
    v->fat_sectors = fatsz;
    v->root_dir_sectors = (root_entries * 32 + 511) / 512;
    v->root_dir_sector = v->reserved + v->nfats * fatsz;
    v->data_start = v->root_dir_sector + v->root_dir_sectors;
    if (total <= v->data_start) { snprintf(err, errlen, "damaged file system"); delete v; return nullptr; }
    v->clusters = (total - v->data_start) / v->spc;
    v->type = v->clusters < 4085 ? 12 : v->clusters < 65525 ? 16 : 32;
    if (v->type == 12) { snprintf(err, errlen, "FAT12 drives are not supported"); delete v; return nullptr; }
    v->root_cluster = v->type == 32 ? rd32(sec + 44) : 0;
    v->fsinfo = v->type == 32 ? rd16(sec + 48) : 0;
    const u8* lab = sec + (v->type == 32 ? 71 : 43);
    memcpy(v->label, lab, 11);
    v->label[11] = 0;
    for (int k = 10; k >= 0 && v->label[k] == ' '; k--) v->label[k] = 0;
    // the FAT, decoded into 32-bit entries
    u32 entries = v->clusters + 2;
    if (entries * 4 > (96u << 20) || fatsz * 512 < entries * (v->type == 32 ? 4 : 2)) { snprintf(err, errlen, "drive too large"); delete v; return nullptr; }
    v->fat = (u32*)kmalloc_try(entries * 4 + 16);
    v->fat_dirty = (u8*)kcalloc_try(fatsz + 1);
    u8* chunk = (u8*)kmalloc_try(64 * 512);
    if (!v->fat || !v->fat_dirty || !chunk) { snprintf(err, errlen, "out of memory"); delete v; return nullptr; }
    for (u32 s = 0; s < fatsz; s += 64) {
        u32 n = min<u32>(64, fatsz - s);
        if (!dread(v, v->reserved + s, n, chunk)) { snprintf(err, errlen, "can't read the FAT"); kfree(chunk); delete v; return nullptr; }
        for (u32 b = 0; b < n * 512; b += (v->type == 32 ? 4 : 2)) {
            u32 c = (s * 512 + b) / (v->type == 32 ? 4 : 2);
            if (c >= entries) break;
            v->fat[c] = v->type == 32 ? (rd32(chunk + b) & 0x0FFFFFFF) : rd16(chunk + b);
        }
    }
    kfree(chunk);
    v->id = slot + 1;
    v->root = mount_point;
    vols[slot] = v;
    mount_point->fat_vol = (u8)v->id;
    mount_point->fat_first = v->root_cluster;
    mount_point->fat_state = FAT_SYNCED;
    mount_point->mount = (u8)v->id;
    // a volume label entry in the root directory beats the one in the boot sector
    u8* rraw = nullptr;
    u32 rlen = 0;
    if (dir_load(v, v->root_cluster, &rraw, &rlen)) {
        for (u32 i = 0; i + 32 <= rlen && rraw[i]; i += 32)
            if (rraw[i] != 0xE5 && (rraw[i + 11] & 0x08) && rraw[i + 11] != 0x0F) { memcpy(v->label, rraw + i, 11); v->label[11] = 0; for (int k = 10; k >= 0 && v->label[k] == ' '; k--) v->label[k] = 0; }
        kfree(rraw);
    }
    import_dir(v, mount_point, v->root_cluster, 0);
    v->dirty = false;
    strlcpy(label_out, v->label, 12);
    klog("fat: mounted %s (%s, %u clusters of %u bytes), label '%s'\n", dev->name, fat_type_name(v), v->clusters, csize(v), v->label);
    return v;
}

// ---------------------------------------------------------------- writing
static void make_83(const char* name, u8 out[11], bool* lfn_needed, const u8* raw, u32 len) {
    char up[FS_NAME_MAX];
    strlcpy(up, name, sizeof(up));
    const char* dot = strrchr(up, '.');
    char base[FS_NAME_MAX], ext[FS_NAME_MAX];
    if (dot && dot != up) { int bl = (int)(dot - up); memcpy(base, up, bl); base[bl] = 0; strlcpy(ext, dot + 1, sizeof(ext)); }
    else { strlcpy(base, up, sizeof(base)); ext[0] = 0; }
    bool clean = true;
    auto fix = [&](char* s, int maxlen, char* o) -> int {
        int n = 0;
        for (const char* p = s; *p; p++) {
            char c = *p;
            if (c == ' ' || c == '.') { clean = false; continue; }
            if (c >= 'a' && c <= 'z') { c = (char)(c - 32); clean = false; }
            else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("$%'-_@~`!(){}^#&", c))) { c = '_'; clean = false; }
            if (n < maxlen) o[n++] = c; else clean = false;
        }
        return n;
    };
    char b8[9], e3[4];
    int bn = fix(base, 8, b8), en = fix(ext, 3, e3);
    if (!bn) { b8[0] = '_'; bn = 1; clean = false; }
    *lfn_needed = !clean;
    for (int tail = 0; tail < 1000; tail++) {
        memset(out, ' ', 11);
        if (!tail && clean) { memcpy(out, b8, bn); memcpy(out + 8, e3, en); }
        else {
            char t[8];
            int tl = snprintf(t, sizeof(t), "~%d", tail ? tail : 1);
            int keep = min(bn, 8 - tl);
            memcpy(out, b8, keep);
            memcpy(out + keep, t, tl);
            memcpy(out + 8, e3, en);
        }
        bool clash = false;
        for (u32 i = 0; i + 32 <= len && !clash; i += 32) {
            if (raw[i] == 0) break;
            if (raw[i] != 0xE5 && raw[i + 11] != 0x0F && !memcmp(raw + i, out, 11)) clash = true;
        }
        if (!clash) return;
        clean = false;
        *lfn_needed = true;
    }
}

static bool name_eq(const char* a, const char* b) {
    while (*a && to_lower(*a) == to_lower(*b)) { a++; b++; }
    return *a == *b;
}

// index of the short entry for `name` in a raw directory (and where its long-name run starts), or -1
static int dir_find(const u8* raw, u32 len, const char* name, int* run_start) {
    u16 lfn[260];
    int lfn_n = 0;
    bool have = false;
    int start = -1;
    for (u32 i = 0; i + 32 <= len; i += 32) {
        const u8* e = raw + i;
        if (e[0] == 0) break;
        if (e[0] == 0xE5) { have = false; start = -1; continue; }
        if (e[11] == 0x0F) {
            int seq = e[0] & 0x1F;
            if (e[0] & 0x40) { have = true; lfn_n = seq * 13; memset(lfn, 0, sizeof(lfn)); start = (int)(i / 32); }
            if (have && seq >= 1 && seq <= 20) {
                static const int offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int k = 0; k < 13; k++) lfn[(seq - 1) * 13 + k] = (u16)rd16(e + offs[k]);
            }
            continue;
        }
        if (e[11] & 0x08) { have = false; start = -1; continue; }
        char nm[FS_NAME_MAX];
        if (have) { int n = 0; while (n < lfn_n && lfn[n] && lfn[n] != 0xFFFF) n++; utf16_to_name(lfn, n, nm); }
        else {
            DirEnt tmp[1];
            (void)tmp;
            char base[9], ext[4];
            memcpy(base, e, 8); base[8] = 0; memcpy(ext, e + 8, 3); ext[3] = 0;
            for (int k = 7; k >= 0 && base[k] == ' '; k--) base[k] = 0;
            for (int k = 2; k >= 0 && ext[k] == ' '; k--) ext[k] = 0;
            snprintf(nm, sizeof(nm), "%s%s%s", base, ext[0] ? "." : "", ext);
        }
        int idx = (int)(i / 32);
        if (name_eq(nm, name)) { *run_start = have ? start : idx; return idx; }
        have = false; start = -1;
    }
    return -1;
}

static void set_times(u8* e, u32 stamp) {
    u16 d, t;
    stamp_to_fat(stamp ? stamp : rtc_stamp(), &d, &t);
    wr16(e + 14, t); wr16(e + 16, d); wr16(e + 18, d); wr16(e + 22, t); wr16(e + 24, d);
}

static bool dir_add(FatVol* v, u32 dir_cluster, const char* name, u8 attr, u32 first, u32 size, u32 stamp) {
    u8* raw = nullptr;
    u32 len = 0;
    if (!dir_load(v, dir_cluster, &raw, &len)) return false;
    u8 s83[11];
    bool lfn_needed;
    make_83(name, s83, &lfn_needed, raw, len);
    // long name as UTF-16
    u16 u[260];
    int un = 0;
    const char* p = name;
    const char* end = name + strlen(name);
    while (p < end && un < 250) {
        u32 cp = utf8_decode(p, end);
        if (cp >= 0x10000) { cp -= 0x10000; u[un++] = (u16)(0xD800 + (cp >> 10)); u[un++] = (u16)(0xDC00 + (cp & 0x3FF)); }
        else u[un++] = (u16)cp;
    }
    int nl = lfn_needed ? (un + 12) / 13 : 0;
    u32 need = (u32)nl + 1;
    // a free run of `need` entries, growing the directory by clusters when it is full
    int slot = -1;
    u32 run = 0;
    bool found = false;
    for (u32 i = 0; i + 32 <= len; i += 32) {
        u8 b0 = raw[i];
        if (b0 == 0 || b0 == 0xE5) {
            if (!run) slot = (int)(i / 32);
            run++;
            if (run >= need) { found = true; break; }
            if (b0 == 0) { u32 rem = (len - i) / 32 - 1; if (run + rem >= need) found = true; break; }
        } else run = 0;
    }
    while (!found) {
        if (dir_cluster == 0) { kfree(raw); return false; }     // the FAT16 root can't grow
        u32 nc = alloc_chain(v, 1);
        if (!nc) { kfree(raw); return false; }
        u32 last = dir_cluster;
        while (!is_eoc(v, fat_get(v, last))) last = fat_get(v, last);
        fat_set(v, last, nc);
        u8* bigger = (u8*)kcalloc_try(len + csize(v) + 32);
        if (!bigger) { kfree(raw); return false; }
        memcpy(bigger, raw, len);
        kfree(raw);
        raw = bigger;
        if (!run) slot = (int)(len / 32);
        run += csize(v) / 32;
        len += csize(v);
        found = run >= need;
    }
    u8 csum = 0;
    for (int k = 0; k < 11; k++) csum = (u8)(((csum & 1) << 7) + (csum >> 1) + s83[k]);
    for (int k = 0; k < nl; k++) {
        int seq = nl - k;                                       // entries are stored last-first
        u8* e = raw + (slot + k) * 32;
        memset(e, 0, 32);
        e[0] = (u8)(seq | (k == 0 ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = csum;
        static const int offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (int c = 0; c < 13; c++) {
            int idx = (seq - 1) * 13 + c;
            u16 ch = idx < un ? u[idx] : (idx == un ? 0 : 0xFFFF);
            wr16(e + offs[c], ch);
        }
    }
    u8* e = raw + (slot + nl) * 32;
    memset(e, 0, 32);
    memcpy(e, s83, 11);
    e[11] = attr;
    wr16(e + 20, first >> 16);
    wr16(e + 26, first & 0xFFFF);
    wr32(e + 28, size);
    set_times(e, stamp);
    bool ok = dir_store(v, dir_cluster, raw, len);
    kfree(raw);
    return ok;
}

static bool dir_update(FatVol* v, u32 dir_cluster, const char* name, u32 first, u32 size, u32 stamp) {
    u8* raw = nullptr;
    u32 len = 0;
    if (!dir_load(v, dir_cluster, &raw, &len)) return false;
    int rs;
    int idx = dir_find(raw, len, name, &rs);
    bool ok = false;
    if (idx >= 0) {
        u8* e = raw + idx * 32;
        wr16(e + 20, first >> 16);
        wr16(e + 26, first & 0xFFFF);
        wr32(e + 28, size);
        u16 d, t;
        stamp_to_fat(stamp ? stamp : rtc_stamp(), &d, &t);
        wr16(e + 22, t); wr16(e + 24, d); wr16(e + 18, d);
        ok = dir_store(v, dir_cluster, raw, len);
    }
    kfree(raw);
    return ok;
}

static bool dir_remove(FatVol* v, u32 dir_cluster, const char* name) {
    u8* raw = nullptr;
    u32 len = 0;
    if (!dir_load(v, dir_cluster, &raw, &len)) return false;
    int rs;
    int idx = dir_find(raw, len, name, &rs);
    bool ok = true;
    if (idx >= 0) {
        for (int i = rs; i <= idx; i++) raw[i * 32] = 0xE5;
        ok = dir_store(v, dir_cluster, raw, len);
    }
    kfree(raw);
    return ok;
}

static u32 new_dir_cluster(FatVol* v, u32 parent_cluster) {
    u32 c = alloc_chain(v, 1);
    if (!c) return 0;
    u8* buf = (u8*)kcalloc_try(csize(v));
    if (!buf) { free_chain_all(v, c); return 0; }
    u16 d, t;
    stamp_to_fat(rtc_stamp(), &d, &t);
    memset(buf, ' ', 11); buf[0] = '.'; buf[11] = 0x10;
    wr16(buf + 20, c >> 16); wr16(buf + 26, c & 0xFFFF); wr16(buf + 22, t); wr16(buf + 24, d);
    u8* dd = buf + 32;
    memset(dd, ' ', 11); dd[0] = '.'; dd[1] = '.'; dd[11] = 0x10;
    u32 pc = parent_cluster == v->root_cluster ? 0 : parent_cluster;
    wr16(dd + 20, pc >> 16); wr16(dd + 26, pc & 0xFFFF); wr16(dd + 22, t); wr16(dd + 24, d);
    bool ok = write_chain(v, c, buf, csize(v));
    kfree(buf);
    if (!ok) { free_chain_all(v, c); return 0; }
    return c;
}

static bool write_file(FatVol* v, FsNode* n, u32* first) {
    *first = 0;
    if (!n->size) return true;
    u32 nclus = (n->size + csize(v) - 1) / csize(v);
    u32 c = alloc_chain(v, nclus);
    if (!c) return false;
    if (!write_chain(v, c, n->data, n->size)) { free_chain_all(v, c); return false; }
    *first = c;
    return true;
}

static bool sync_dir(FatVol* v, FsNode* dir, u32 dir_cluster) {
    bool ok = true;
    for (int i = 0; i < dir->children.len; i++) {
        FsNode* c = dir->children[i];
        if (c->fat_state == FAT_OVERSIZE) continue;
        if (c->dir) {
            if (c->fat_state == FAT_NEW) {
                u32 nc = new_dir_cluster(v, dir_cluster);
                if (!nc || !dir_add(v, dir_cluster, c->name, 0x10, nc, 0, c->mtime)) { ok = false; continue; }
                c->fat_first = nc; c->fat_dir = dir_cluster; c->fat_state = FAT_SYNCED;
            }
            ok &= sync_dir(v, c, c->fat_first);
            continue;
        }
        if (c->fat_state == FAT_NEW) {
            u32 first;
            if (!write_file(v, c, &first) || !dir_add(v, dir_cluster, c->name, 0x20, first, c->size, c->mtime)) { ok = false; continue; }
            c->fat_first = first; c->fat_dir = dir_cluster; c->fat_state = FAT_SYNCED;
        } else if (c->fat_state == FAT_MODIFIED) {
            u32 first;
            if (c->fat_first >= 2) free_chain_all(v, c->fat_first);
            if (!write_file(v, c, &first) || !dir_update(v, dir_cluster, c->name, first, c->size, c->mtime)) { ok = false; continue; }
            c->fat_first = first; c->fat_state = FAT_SYNCED;
        }
    }
    return ok;
}

static bool flush_fat(FatVol* v) {
    bool ok = true;
    u8 sec[512];
    for (u32 s = 0; s < v->fat_sectors; s++) {
        if (!v->fat_dirty[s]) continue;
        // encode this sector of the FAT
        u32 per = v->type == 32 ? 128 : 256;
        for (u32 k = 0; k < per; k++) {
            u32 c = s * per + k;
            u32 val = c < v->clusters + 2 ? v->fat[c] : 0;
            if (v->type == 32) wr32(sec + k * 4, val & 0x0FFFFFFF); else wr16(sec + k * 2, val);
        }
        for (u32 f = 0; f < v->nfats; f++) ok &= dwrite(v, v->reserved + f * v->fat_sectors + s, 1, sec);
        v->fat_dirty[s] = 0;
    }
    if (v->fsinfo && ok) {                                        // free-space hints become "unknown"
        if (dread(v, v->fsinfo, 1, sec) && rd32(sec) == 0x41615252) {
            wr32(sec + 488, 0xFFFFFFFF); wr32(sec + 492, 0xFFFFFFFF);
            dwrite(v, v->fsinfo, 1, sec);
        }
    }
    return ok;
}

bool fat_sync(FatVol* v) {
    if (!v || v->ro) return false;
    if (!fat_dirty(v)) return true;
    bool ok = true;
    for (int i = 0; i < v->to_free.len; i++) free_chain_all(v, v->to_free[i]);
    v->to_free.clear();
    for (int i = 0; i < v->dels.len; i++) ok &= dir_remove(v, v->dels[i].dir_cluster, v->dels[i].name);
    v->dels.clear();
    ok &= sync_dir(v, v->root, v->root_cluster);
    ok &= flush_fat(v);
    v->dirty = !ok;
    klog("fat: %s synced to %s (%s)\n", v->label, v->dev->name, ok ? "ok" : "FAILED");
    return ok;
}

void fat_unmount(FatVol* v, bool write_back) {
    if (!v) return;
    if (write_back && v->dev->present) fat_sync(v);
    vols[v->id - 1] = nullptr;
    v->dels.free();
    v->to_free.free();
    kfree(v->fat);
    kfree(v->fat_dirty);
    delete v;
}

u32 fat_total_kb(FatVol* v) { return v->clusters * csize(v) / 1024; }
u32 fat_free_kb(FatVol* v) {
    u32 f = 0;
    for (u32 c = 2; c < v->clusters + 2; c++) if (!v->fat[c]) f++;
    return f * csize(v) / 1024;
}

// ---------------------------------------------------------------- hooks from fs.cpp
void fat_note_modified(FsNode* n) {
    FatVol* v = fat_vol_by_id(n->fat_vol);
    if (!v) return;
    if (n->fat_state == FAT_SYNCED) n->fat_state = FAT_MODIFIED;
    v->dirty = true;
}

static void note_subtree_removed(FatVol* v, FsNode* n) {
    if (n->fat_state != FAT_NEW && n->fat_first >= 2) v->to_free.push(n->fat_first);
    for (int i = 0; i < n->children.len; i++) note_subtree_removed(v, n->children[i]);
}

void fat_note_removed(FsNode* n) {
    FatVol* v = fat_vol_by_id(n->fat_vol);
    if (!v || n->fat_state == FAT_NEW || n->mount) return;
    Del d;
    memset(&d, 0, sizeof(d));
    d.dir_cluster = n->parent ? n->parent->fat_first : v->root_cluster;
    strlcpy(d.name, n->name, sizeof(d.name));
    v->dels.push(d);
    note_subtree_removed(v, n);
    v->dirty = true;
}
