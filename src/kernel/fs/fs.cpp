// silicon - SiliconFS
#include "fs.h"
#include "../drv/drivers.h"
#include "fat.h"
#include "drives.h"

// ---------------------------------------------------------------- on-disk format
// sector FS_LBA: header, followed by node records, followed by file data.
struct __attribute__((packed)) DiskHeader {
    char magic[8];          // "SILFS1\0\0"
    u32  version;
    u32  node_count;
    u32  data_bytes;
    u32  total_bytes;       // header + records + data
    u32  checksum;          // FNV-1a over everything after the header
    u32  generation;
    u8   pad[480];
};
struct __attribute__((packed)) DiskNode {
    char name[FS_NAME_MAX];
    u32  parent;            // record index, 0xFFFFFFFF for the root
    u32  flags;             // bit0 = dir
    u32  size;
    u32  data_off;
    u32  ctime, mtime;
    char origin[FS_ORIGIN_MAX];
};
// version 2 adds ownership and permissions
struct __attribute__((packed)) DiskNode2 {
    DiskNode n;
    u16  mode, uid;
    u32  reserved;
};
static const u32 FS_VERSION = 2;
u16 fs_uid = FS_UID_ROOT;
static_assert(sizeof(DiskHeader) == 512, "header must be one sector");

static FsNode* root = nullptr;
static bool persistent = false;
static bool dirty = false;
static u64 dirty_since = 0;
static u32 generation = 0;
static u32 last_disk_bytes = 0;

static u32 fnv(const u8* p, u32 n) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static FsNode* new_node(const char* name, bool dir) {
    FsNode* n = new FsNode();
    strlcpy(n->name, name, FS_NAME_MAX);
    n->dir = dir;
    n->mode = dir ? 0755 : 0644;
    n->uid = fs_uid;
    n->ctime = n->mtime = rtc_stamp();
    return n;
}

// ---------------------------------------------------------------- permissions
bool fs_allowed(FsNode* n, int want) {
    if (n && (want & 2) && n->fat_vol) { FatVol* v = fat_vol_by_id(n->fat_vol); if (v && fat_readonly(v)) return false; }   // read-only drives (exFAT)
    if (!n || fs_uid == FS_UID_ROOT) return true;
    int bits = n->uid == fs_uid ? (n->mode >> 6) & 7 : (n->mode | (n->mode >> 3)) & 7;
    return (bits & want) == want;
}

bool fs_can_modify(FsNode* n) {
    if (!n) return false;
    if (fs_uid == FS_UID_ROOT) return true;
    return fs_allowed(n->parent ? n->parent : n, 2) && (n->dir || fs_allowed(n, 2));
}

void fs_mode_string(FsNode* n, char out[11]) {
    out[0] = n->dir ? 'd' : '-';
    static const char r[] = "rwx";
    for (int i = 0; i < 9; i++) out[1 + i] = (n->mode & (0400 >> i)) ? r[i % 3] : '-';
    out[10] = 0;
}

bool fs_chmod(FsNode* n, u16 mode) {
    if (!n || (fs_uid != FS_UID_ROOT && n->uid != fs_uid)) return false;
    n->mode = mode & 0777;
    fs_mark_dirty();
    return true;
}

bool fs_chown(FsNode* n, u16 uid, bool recursive) {
    if (!n || fs_uid != FS_UID_ROOT) return false;
    n->uid = uid;
    if (recursive) for (int i = 0; i < n->children.len; i++) fs_chown(n->children[i], uid, true);
    fs_mark_dirty();
    return true;
}

static void free_node(FsNode* n) {
    for (int i = 0; i < n->children.len; i++) free_node(n->children[i]);
    n->children.free();
    if (n->data) kfree(n->data);
    delete n;
}

static void attach(FsNode* dir, FsNode* n) {
    n->parent = dir;
    dir->children.push(n);
    dir->mtime = rtc_stamp();
}

static void detach(FsNode* n) {
    if (!n->parent) return;
    int i = n->parent->children.find(n);
    if (i >= 0) n->parent->children.remove(i);
    n->parent->mtime = rtc_stamp();
    n->parent = nullptr;
}

FsNode* fs_root() { return root; }
bool fs_persistent() { return persistent; }
FsNode* fs_home() { return fs_child(root, "home"); }
FsNode* fs_trash_dir() {
    FsNode* t = fs_child(root, "trash");
    return t ? t : fs_create(root, "trash", true);
}

FsNode* fs_child(FsNode* dir, const char* name) {
    if (!dir || !dir->dir) return nullptr;
    for (int i = 0; i < dir->children.len; i++)
        if (!strcmp(dir->children[i]->name, name)) return dir->children[i];
    return nullptr;
}

FsNode* fs_lookup(const char* path, FsNode* cwd) {
    if (!path) return nullptr;
    FsNode* cur = (path[0] == '/' || !cwd) ? root : cwd;
    if (path[0] == '~') { cur = fs_home(); path++; }
    char part[FS_NAME_MAX];
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        int n = 0;
        while (*path && *path != '/') { if (n < FS_NAME_MAX - 1) part[n++] = *path; path++; }
        part[n] = 0;
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) { if (cur->parent) cur = cur->parent; continue; }
        cur = fs_child(cur, part);
        if (!cur) return nullptr;
    }
    return cur;
}

bool fs_valid_name(const char* name) {
    if (!name || !*name || strlen(name) >= (size_t)FS_NAME_MAX) return false;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (const char* p = name; *p; p++) if (*p == '/' || (u8)*p < 32) return false;
    return true;
}

FsNode* fs_create(FsNode* dir, const char* name, bool is_dir) {
    if (!dir || !dir->dir || !fs_valid_name(name)) return nullptr;
    FsNode* e = fs_child(dir, name);
    if (e) return e->dir == is_dir ? e : nullptr;
    if (!fs_allowed(dir, 2)) return nullptr;
    FsNode* n = new_node(name, is_dir);
    attach(dir, n);
    if (dir->fat_vol) { n->fat_vol = dir->fat_vol; n->fat_state = FAT_NEW; fat_note_modified(dir); }
    fs_mark_dirty();
    return n;
}

// split "a/b/c" into parent path + leaf
static FsNode* parent_of(const char* path, FsNode* cwd, char* leaf) {
    const char* slash = strrchr(path, '/');
    if (!slash) { strlcpy(leaf, path, FS_NAME_MAX); return cwd ? cwd : root; }
    strlcpy(leaf, slash + 1, FS_NAME_MAX);
    if (slash == path) return root;
    char dirpath[256];
    int n = min((int)(slash - path), 255);
    memcpy(dirpath, path, n);
    dirpath[n] = 0;
    return fs_lookup(dirpath, cwd);
}

FsNode* fs_create_path(const char* path, bool is_dir, FsNode* cwd) {
    char leaf[FS_NAME_MAX];
    FsNode* dir = parent_of(path, cwd, leaf);
    return dir ? fs_create(dir, leaf, is_dir) : nullptr;
}

FsNode* fs_mkdirs(const char* path) {
    FsNode* cur = root;
    char part[FS_NAME_MAX];
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        int n = 0;
        while (*path && *path != '/') { if (n < FS_NAME_MAX - 1) part[n++] = *path; path++; }
        part[n] = 0;
        cur = fs_create(cur, part, true);
        if (!cur) return nullptr;
    }
    return cur;
}

static bool has_oversize(FsNode* n) {
    if (n->fat_state == FAT_OVERSIZE) return true;
    for (int i = 0; i < n->children.len; i++) if (has_oversize(n->children[i])) return true;
    return false;
}
// a node moved into (or out of) a drive: everything below it has to be written afresh
static void reown(FsNode* n, u8 vol) {
    n->fat_vol = vol;
    n->fat_state = vol ? FAT_NEW : 0;
    n->fat_first = 0;
    for (int i = 0; i < n->children.len; i++) reown(n->children[i], vol);
}

static bool reserve(FsNode* f, u32 len) {
    if (len + 1 <= f->cap) return true;
    u32 nc = max(len + 1, f->cap * 2);
    nc = max(nc, 64u);
    u8* nd = (u8*)kmalloc(nc);
    if (f->data) { memcpy(nd, f->data, f->size); kfree(f->data); }
    f->data = nd;
    f->cap = nc;
    return true;
}

bool fs_write(FsNode* f, const void* data, u32 len) {
    if (!f || f->dir || !fs_allowed(f, 2) || f->fat_state == FAT_OVERSIZE) return false;
    reserve(f, len);
    memcpy(f->data, data, len);
    f->size = len;
    f->data[len] = 0;
    f->mtime = rtc_stamp();
    if (f->fat_vol) fat_note_modified(f);
    fs_mark_dirty();
    return true;
}

bool fs_append(FsNode* f, const void* data, u32 len) {
    if (!f || f->dir || !fs_allowed(f, 2) || f->fat_state == FAT_OVERSIZE) return false;
    reserve(f, f->size + len);
    memcpy(f->data + f->size, data, len);
    f->size += len;
    f->data[f->size] = 0;
    f->mtime = rtc_stamp();
    if (f->fat_vol) fat_note_modified(f);
    fs_mark_dirty();
    return true;
}

bool fs_write_text(const char* path, const char* text) {
    FsNode* f = fs_create_path(path, false);
    return f && fs_write(f, text, strlen(text));
}

const char* fs_read_text(FsNode* f) {
    if (!f || f->dir) return "";
    if (!f->data) reserve(f, 0), f->data[0] = 0;
    return (const char*)f->data;
}

void fs_remove(FsNode* n) {
    if (!n || n == root || !fs_can_modify(n) || n->mount) return;
    if (n->fat_vol) fat_note_removed(n);
    detach(n);
    free_node(n);
    fs_mark_dirty();
}

bool fs_is_ancestor(FsNode* a, FsNode* n) {
    for (FsNode* p = n; p; p = p->parent) if (p == a) return true;
    return false;
}

bool fs_move(FsNode* n, FsNode* np, const char* new_name) {
    if (!n || n == root || !np || !np->dir) return false;
    if (n->dir && fs_is_ancestor(n, np)) return false;
    if (!fs_can_modify(n) || !fs_allowed(np, 2) || n->mount) return false;
    const char* nm = new_name ? new_name : n->name;
    if (!fs_valid_name(nm)) return false;
    FsNode* clash = fs_child(np, nm);
    if (clash && clash != n) return false;
    if (n->fat_vol && has_oversize(n)) return false;          // files that were too big to load can't be moved around
    if (n->fat_vol) fat_note_removed(n);
    detach(n);
    reown(n, np->fat_vol);
    strlcpy(n->name, nm, FS_NAME_MAX);
    attach(np, n);
    fs_mark_dirty();
    return true;
}

FsNode* fs_copy(FsNode* n, FsNode* np, const char* new_name) {
    if (!n || !np || !np->dir) return nullptr;
    if (n->dir && fs_is_ancestor(n, np)) return nullptr;
    const char* nm = new_name ? new_name : n->name;
    if (fs_child(np, nm)) return nullptr;
    FsNode* c = fs_create(np, nm, n->dir);
    if (!c) return nullptr;
    if (!n->dir) fs_write(c, n->data ? n->data : (const u8*)"", n->size);
    else for (int i = 0; i < n->children.len; i++) fs_copy(n->children[i], c, nullptr);
    return c;
}

void fs_path(FsNode* n, char* buf, int len) {
    if (!n || n == root) { strlcpy(buf, "/", len); return; }
    char tmp[512];
    tmp[0] = 0;
    for (FsNode* p = n; p && p != root; p = p->parent) {
        char seg[FS_NAME_MAX + 2];
        snprintf(seg, sizeof(seg), "/%s", p->name);
        char old[512];
        strlcpy(old, tmp, sizeof(old));
        snprintf(tmp, sizeof(tmp), "%s%s", seg, old);
    }
    strlcpy(buf, tmp, len);
}

void fs_unique_name(FsNode* dir, const char* base, char* out, int len) {
    strlcpy(out, base, len);
    if (!fs_child(dir, out)) return;
    char stem[FS_NAME_MAX], ext[FS_NAME_MAX];
    strlcpy(stem, base, sizeof(stem));
    ext[0] = 0;
    char* dot = strrchr(stem, '.');
    if (dot && dot != stem) { strlcpy(ext, dot, sizeof(ext)); *dot = 0; }
    for (int i = 2; i < 1000; i++) {
        snprintf(out, len, "%s %d%s", stem, i, ext);
        if (!fs_child(dir, out)) return;
    }
}

u32 fs_tree_size(FsNode* n) {
    if (!n->dir) return n->size;
    u32 s = 0;
    for (int i = 0; i < n->children.len; i++) s += fs_tree_size(n->children[i]);
    return s;
}

int fs_count(FsNode* n) {
    int c = 1;
    for (int i = 0; i < n->children.len; i++) c += fs_count(n->children[i]);
    return c;
}

void fs_sort(FsNode* dir) {
    Vec<FsNode*>& v = dir->children;
    for (int i = 1; i < v.len; i++) {
        FsNode* k = v[i];
        int j = i - 1;
        auto less = [](FsNode* a, FsNode* b) {
            if (a->dir != b->dir) return a->dir;
            const char *x = a->name, *y = b->name;
            while (*x && to_lower(*x) == to_lower(*y)) { x++; y++; }
            return to_lower(*x) < to_lower(*y);
        };
        while (j >= 0 && less(k, v[j])) { v[j + 1] = v[j]; j--; }
        v[j + 1] = k;
    }
}

// ---------------------------------------------------------------- trash
FsNode* fs_trash(FsNode* n) {
    FsNode* t = fs_trash_dir();
    if (!n || n == root || n == t || fs_is_ancestor(n, t)) return nullptr;
    if (n->parent == t) { fs_remove(n); return nullptr; }
    char path[256], nm[FS_NAME_MAX];
    fs_path(n, path, sizeof(path));
    fs_unique_name(t, n->name, nm, sizeof(nm));
    if (!fs_move(n, t, nm)) return nullptr;
    strlcpy(n->origin, path, FS_ORIGIN_MAX);
    return n;
}

FsNode* fs_restore(FsNode* n) {
    if (!n || n->parent != fs_trash_dir()) return nullptr;
    char leaf[FS_NAME_MAX];
    FsNode* dir = n->origin[0] ? parent_of(n->origin, root, leaf) : nullptr;
    if (!dir) { dir = fs_home(); strlcpy(leaf, n->name, sizeof(leaf)); }
    char nm[FS_NAME_MAX];
    fs_unique_name(dir, leaf, nm, sizeof(nm));
    if (!fs_move(n, dir, nm)) return nullptr;
    n->origin[0] = 0;
    return n;
}

void fs_empty_trash() {
    FsNode* t = fs_trash_dir();
    while (t->children.len) fs_remove(t->children[0]);
}

// ---------------------------------------------------------------- persistence
static u32 change_gen = 0;
u32 fs_generation() { return change_gen; }

void fs_mark_dirty() {
    change_gen++;
    if (!dirty) dirty_since = ticks();
    dirty = true;
}

void fs_tick() {
    if (dirty && ticks() - dirty_since > 400) fs_sync();
}

static void collect(FsNode* n, Vec<FsNode*>& out) {
    out.push(n);
    for (int i = 0; i < n->children.len; i++) if (!n->children[i]->mount) collect(n->children[i], out);       // drives stay off the system disk
}


// bring an image from before permissions existed up to date: the system area is root's
static void default_ownership(FsNode* n, bool in_system) {
    bool sys = in_system || (n->parent == root && !strcmp(n->name, "system"));
    n->uid = sys ? FS_UID_ROOT : FS_UID_USER;
    n->mode = n->dir ? 0755 : 0644;
    if (n->parent == root && (!strcmp(n->name, "trash"))) n->mode = 0777;
    for (int i = 0; i < n->children.len; i++) default_ownership(n->children[i], sys);
}

// ---------------------------------------------------------------- crash-safe commits
// The disk holds two slots. A sync writes the whole new image into the slot that is NOT current,
// data sectors first and the header sector (with the generation number and a checksum over
// everything) last, so the header can only ever describe a complete image. On boot the newest slot
// whose checksum verifies is the file system; if power fails halfway through a sync, the torn slot
// fails its checksum and the previous commit is used. Nothing is ever half-written.
struct Slot { u8* img; u32 sectors; u32 gen; bool valid; };
static Slot slots[2];
static int active = -1;                           // the slot holding the newest valid commit

static u32 slot_sectors() { return persistent ? ((ata_sectors() - FS_LBA) / 2) : 0; }
static u32 slot_lba(int i) { return FS_LBA + (u32)i * slot_sectors(); }

u32 fs_disk_capacity() { return persistent ? slot_sectors() * 512 : 0; }     // the largest image a commit can hold
u32 fs_disk_used() { return last_disk_bytes; }
u32 fs_commit_generation() { return generation; }
int fs_active_slot() { return active; }

// write only the sectors that differ from what this slot held two commits ago; the header goes last
static bool write_changes(int slot, u8* buf, u32 sectors) {
    const u32 CH = 16;                            // compare in 8 KiB chunks
    bool ok = true;
    u32 written = 0;
    Slot& sl = slots[slot];
    u32 lba = slot_lba(slot);
    for (u32 s = 1; s < sectors;) {
        u32 n = min(CH, sectors - s);
        bool same = sl.img && s + n <= sl.sectors && !memcmp(sl.img + s * 512, buf + s * 512, n * 512);
        if (same) { s += n; continue; }
        u32 e = s + n;
        while (e < sectors) {
            u32 m = min(CH, sectors - e);
            if (sl.img && e + m <= sl.sectors && !memcmp(sl.img + e * 512, buf + e * 512, m * 512)) break;
            e += m;
        }
        ok &= ata_write(lba + s, e - s, buf + s * 512);
        written += e - s;
        s = e;
    }
    if (ok) { ok &= ata_write(lba, 1, buf); written++; }            // commit
    if (sl.img) kfree(sl.img);
    sl.img = buf;
    sl.sectors = sectors;
    if (written) klog("fs: wrote %u of %u sectors to slot %d\n", written, sectors, slot);
    return ok;
}

bool fs_sync() {
    drives_sync_all();
    dirty = false;
    if (!persistent) return false;
    Vec<FsNode*> nodes;
    collect(root, nodes);
    u32 data_bytes = 0;
    for (int i = 0; i < nodes.len; i++) data_bytes += nodes[i]->dir ? 0 : nodes[i]->size;
    u32 rec_bytes = nodes.len * sizeof(DiskNode2);
    u32 total = sizeof(DiskHeader) + rec_bytes + data_bytes;
    u32 sectors = (total + 511) / 512;
    if (sectors > slot_sectors()) { klog("fs: disk full\n"); nodes.free(); return false; }
    u8* buf = (u8*)kcalloc(sectors * 512);
    DiskHeader* h = (DiskHeader*)buf;
    DiskNode2* recs = (DiskNode2*)(buf + sizeof(DiskHeader));
    u8* data = buf + sizeof(DiskHeader) + rec_bytes;
    u32 off = 0;
    for (int i = 0; i < nodes.len; i++) {
        FsNode* n = nodes[i];
        DiskNode2& r2 = recs[i];
        DiskNode& r = r2.n;
        strlcpy(r.name, n->name, FS_NAME_MAX);
        r.parent = 0xFFFFFFFF;
        if (n->parent) for (int j = 0; j < i; j++) if (nodes[j] == n->parent) { r.parent = j; break; }
        r.flags = n->dir ? 1 : 0;
        r.size = n->dir ? 0 : n->size;
        r.data_off = off;
        r.ctime = n->ctime;
        r.mtime = n->mtime;
        strlcpy(r.origin, n->origin, FS_ORIGIN_MAX);
        r2.mode = n->mode;
        r2.uid = n->uid;
        if (!n->dir && n->size) { memcpy(data + off, n->data, n->size); off += n->size; }
    }
    int target = active < 0 ? 0 : 1 - active;
    u32 gen = max(generation, slots[0].gen > slots[1].gen ? slots[0].gen : slots[1].gen) + 1;
    memcpy(h->magic, "SILFS1\0\0", 8);
    h->version = FS_VERSION;
    h->node_count = nodes.len;
    h->data_bytes = data_bytes;
    h->total_bytes = total;
    h->generation = gen;
    h->checksum = fnv(buf + sizeof(DiskHeader), total - sizeof(DiskHeader));
    bool ok = write_changes(target, buf, sectors);          // keeps buf as the slot's image
    if (ok) { active = target; generation = gen; slots[target].gen = gen; slots[target].valid = true; last_disk_bytes = total; }
    nodes.free();
    klog("fs: committed generation %u (%u bytes, %s)\n", gen, total, ok ? "ok" : "FAILED");
    return ok;
}

// read and verify one slot; the image stays in memory when valid
static bool read_slot(int i) {
    Slot& sl = slots[i];
    sl.valid = false;
    sl.gen = 0;
    DiskHeader h;
    if (!ata_read(slot_lba(i), 1, &h)) return false;
    if (memcmp(h.magic, "SILFS1\0\0", 8) || (h.version != 1 && h.version != 2)) return false;
    if (h.total_bytes > slot_sectors() * 512 || h.node_count == 0 || h.total_bytes < sizeof(DiskHeader)) return false;
    u32 sectors = (h.total_bytes + 511) / 512;
    u8* buf = (u8*)kmalloc(sectors * 512);
    if (!ata_read(slot_lba(i), sectors, buf)) { kfree(buf); return false; }
    if (fnv(buf + sizeof(DiskHeader), h.total_bytes - sizeof(DiskHeader)) != h.checksum) {
        klog("fs: slot %d fails its checksum (an interrupted write); ignoring it\n", i);
        kfree(buf);
        return false;
    }
    if (sl.img) kfree(sl.img);
    sl.img = buf;
    sl.sectors = sectors;
    sl.gen = h.generation;
    sl.valid = true;
    return true;
}

static bool load() {
    // v1 images were one big region starting at FS_LBA; slot 0 begins there too, so they still load
    bool a = read_slot(0), b = read_slot(1);
    if (!a && !b) return false;
    int best = (a && b) ? (slots[1].gen > slots[0].gen ? 1 : 0) : (a ? 0 : 1);
    if (a && b) klog("fs: slots hold generations %u and %u, using slot %d\n", slots[0].gen, slots[1].gen, best);
    u8* buf = slots[best].img;
    DiskHeader* hp = (DiskHeader*)buf;
    DiskHeader h = *hp;
    bool v2 = h.version >= 2;
    size_t rec_size = v2 ? sizeof(DiskNode2) : sizeof(DiskNode);
    u8* recs_raw = buf + sizeof(DiskHeader);
    u8* data = recs_raw + (size_t)h.node_count * rec_size;
    FsNode** nodes = (FsNode**)kmalloc(sizeof(FsNode*) * h.node_count);
    for (u32 i = 0; i < h.node_count; i++) {
        DiskNode& r = *(DiskNode*)(recs_raw + (size_t)i * rec_size);
        r.name[FS_NAME_MAX - 1] = 0;
        r.origin[FS_ORIGIN_MAX - 1] = 0;
        FsNode* n = new FsNode();
        strlcpy(n->name, r.name, FS_NAME_MAX);
        n->dir = r.flags & 1;
        n->ctime = r.ctime;
        n->mtime = r.mtime;
        n->mode = n->dir ? 0755 : 0644;
        n->uid = FS_UID_USER;
        if (v2) { DiskNode2& r2 = *(DiskNode2*)(recs_raw + (size_t)i * rec_size); n->mode = r2.mode; n->uid = r2.uid; }
        strlcpy(n->origin, r.origin, FS_ORIGIN_MAX);
        if (!n->dir) {
            reserve(n, r.size);
            memcpy(n->data, data + r.data_off, r.size);
            n->size = r.size;
            n->data[r.size] = 0;
        }
        nodes[i] = n;
        if (r.parent < i) { n->parent = nodes[r.parent]; nodes[r.parent]->children.push(n); }
    }
    root = nodes[0];
    generation = h.generation;
    active = best;
    last_disk_bytes = h.total_bytes;
    klog("fs: loaded %u nodes, %u bytes (generation %u, format %u)\n", h.node_count, h.total_bytes, h.generation, h.version);
    kfree(nodes);
    if (!v2) {                                          // an image from before permissions: the system area becomes root's
        u16 who = fs_uid;
        fs_uid = FS_UID_ROOT;
        default_ownership(root, false);
        if (!fs_lookup("/mnt")) fs_mkdirs("/mnt");
        fs_chown(fs_home(), FS_UID_USER, true);
        fs_uid = who;
        fs_mark_dirty();
        klog("fs: upgraded the image to the permissions format\n");
    }
    return true;
}

static const char* README =
    "Welcome to silicon!\n"
    "\n"
    "silicon boots into a terminal. Type 'help' to see every command,\n"
    "and type 'ui' to start the desktop.\n"
    "\n"
    "Everything under / is saved to disk automatically, so files you\n"
    "create here survive a reboot.\n"
    "\n"
    "Desktop tips:\n"
    "  - click the chip in the top-left corner for the system menu\n"
    "  - ctrl+space opens the launcher\n"
    "  - alt+tab cycles windows\n"
    "  - drag windows by their title bar, resize from the corner\n"
    "\n"
    "Have fun poking at the silicon.\n";

void fs_format() {
    u16 who = fs_uid;
    fs_uid = FS_UID_ROOT;
    if (root) free_node(root);
    root = new_node("", true);
    fs_mkdirs("/home/notes");
    fs_mkdirs("/home/pictures");
    fs_mkdirs("/home/games");
    fs_mkdirs("/system");
    fs_mkdirs("/trash");
    fs_mkdirs("/mnt");
    default_ownership(root, false);
    fs_chown(fs_home(), FS_UID_USER, true);
    fs_uid = who;
    fs_write_text("/home/README.txt", README);
    fs_write_text("/home/notes/todo.txt",
                  "- learn the silicon shell\n- paint something in Pixel\n- beat minesweeper on hard\n");
    fs_write_text("/home/notes/ideas.txt", "an OS named after the stuff it runs on.\n");
    fs_write_text("/system/version", "silicon " SILICON_VERSION "\n");
    fs_uid = FS_UID_ROOT;
    default_ownership(root, false);
    fs_chown(fs_home(), FS_UID_USER, true);
    fs_uid = who;
    fs_sync();
}

void fs_init() {
    persistent = ata_init() && ata_sectors() > FS_LBA + 64;
    if (persistent && load()) return;
    klog("fs: formatting %s filesystem\n", persistent ? "disk" : "RAM-only");
    fs_format();
}
