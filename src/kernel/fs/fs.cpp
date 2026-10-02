// silicon - SiliconFS
#include "fs.h"
#include "../drv/drivers.h"

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
    n->ctime = n->mtime = rtc_stamp();
    return n;
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
    FsNode* n = new_node(name, is_dir);
    attach(dir, n);
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
    if (!f || f->dir) return false;
    reserve(f, len);
    memcpy(f->data, data, len);
    f->size = len;
    f->data[len] = 0;
    f->mtime = rtc_stamp();
    fs_mark_dirty();
    return true;
}

bool fs_append(FsNode* f, const void* data, u32 len) {
    if (!f || f->dir) return false;
    reserve(f, f->size + len);
    memcpy(f->data + f->size, data, len);
    f->size += len;
    f->data[f->size] = 0;
    f->mtime = rtc_stamp();
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
    if (!n || n == root) return;
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
    const char* nm = new_name ? new_name : n->name;
    if (!fs_valid_name(nm)) return false;
    FsNode* clash = fs_child(np, nm);
    if (clash && clash != n) return false;
    detach(n);
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
    for (int i = 0; i < n->children.len; i++) collect(n->children[i], out);
}

u32 fs_disk_capacity() { return persistent ? (ata_sectors() - FS_LBA) * 512 : 0; }
u32 fs_disk_used() { return last_disk_bytes; }

// what's on disk right now, so a sync only writes the sectors that changed
static u8* disk_img = nullptr;
static u32 disk_img_sectors = 0;

static bool write_changes(u8* buf, u32 sectors) {
    const u32 CH = 16;                               // compare in 8 KiB chunks
    bool ok = true;
    u32 written = 0;
    for (u32 s = 0; s < sectors;) {
        u32 n = min(CH, sectors - s);
        bool same = disk_img && s + n <= disk_img_sectors && !memcmp(disk_img + s * 512, buf + s * 512, n * 512);
        if (same) { s += n; continue; }
        // extend the run over following changed chunks
        u32 e = s + n;
        while (e < sectors) {
            u32 m = min(CH, sectors - e);
            if (disk_img && e + m <= disk_img_sectors && !memcmp(disk_img + e * 512, buf + e * 512, m * 512)) break;
            e += m;
        }
        ok &= ata_write(FS_LBA + s, e - s, buf + s * 512);
        written += e - s;
        s = e;
    }
    if (disk_img) kfree(disk_img);
    disk_img = buf;
    disk_img_sectors = sectors;
    if (written) klog("fs: wrote %u of %u sectors\n", written, sectors);
    return ok;
}

bool fs_sync() {
    dirty = false;
    if (!persistent) return false;
    Vec<FsNode*> nodes;
    collect(root, nodes);
    u32 data_bytes = 0;
    for (int i = 0; i < nodes.len; i++) data_bytes += nodes[i]->dir ? 0 : nodes[i]->size;
    u32 rec_bytes = nodes.len * sizeof(DiskNode);
    u32 total = sizeof(DiskHeader) + rec_bytes + data_bytes;
    u32 sectors = (total + 511) / 512;
    if (sectors * 512 > fs_disk_capacity()) { klog("fs: disk full\n"); nodes.free(); return false; }
    u8* buf = (u8*)kcalloc(sectors * 512);
    DiskHeader* h = (DiskHeader*)buf;
    DiskNode* recs = (DiskNode*)(buf + sizeof(DiskHeader));
    u8* data = buf + sizeof(DiskHeader) + rec_bytes;
    u32 off = 0;
    for (int i = 0; i < nodes.len; i++) {
        FsNode* n = nodes[i];
        DiskNode& r = recs[i];
        strlcpy(r.name, n->name, FS_NAME_MAX);
        r.parent = 0xFFFFFFFF;
        if (n->parent) for (int j = 0; j < i; j++) if (nodes[j] == n->parent) { r.parent = j; break; }
        r.flags = n->dir ? 1 : 0;
        r.size = n->dir ? 0 : n->size;
        r.data_off = off;
        r.ctime = n->ctime;
        r.mtime = n->mtime;
        strlcpy(r.origin, n->origin, FS_ORIGIN_MAX);
        if (!n->dir && n->size) { memcpy(data + off, n->data, n->size); off += n->size; }
    }
    memcpy(h->magic, "SILFS1\0\0", 8);
    h->version = 1;
    h->node_count = nodes.len;
    h->data_bytes = data_bytes;
    h->total_bytes = total;
    h->generation = ++generation;
    h->checksum = fnv(buf + sizeof(DiskHeader), total - sizeof(DiskHeader));
    bool ok = write_changes(buf, sectors);         // keeps buf as the new disk image
    last_disk_bytes = total;
    nodes.free();
    klog("fs: synced %u bytes (%s)\n", total, ok ? "ok" : "FAILED");
    return ok;
}

static bool load() {
    DiskHeader h;
    if (!ata_read(FS_LBA, 1, &h)) return false;
    if (memcmp(h.magic, "SILFS1\0\0", 8) || h.version != 1) return false;
    if (h.total_bytes > fs_disk_capacity() || h.node_count == 0) return false;
    u32 sectors = (h.total_bytes + 511) / 512;
    u8* buf = (u8*)kmalloc(sectors * 512);
    if (!ata_read(FS_LBA, sectors, buf)) { kfree(buf); return false; }
    if (fnv(buf + sizeof(DiskHeader), h.total_bytes - sizeof(DiskHeader)) != h.checksum) {
        klog("fs: checksum mismatch\n");
        kfree(buf);
        return false;
    }
    DiskNode* recs = (DiskNode*)(buf + sizeof(DiskHeader));
    u8* data = buf + sizeof(DiskHeader) + h.node_count * sizeof(DiskNode);
    FsNode** nodes = (FsNode**)kmalloc(sizeof(FsNode*) * h.node_count);
    for (u32 i = 0; i < h.node_count; i++) {
        DiskNode& r = recs[i];
        r.name[FS_NAME_MAX - 1] = 0;
        r.origin[FS_ORIGIN_MAX - 1] = 0;
        FsNode* n = new FsNode();
        strlcpy(n->name, r.name, FS_NAME_MAX);
        n->dir = r.flags & 1;
        n->ctime = r.ctime;
        n->mtime = r.mtime;
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
    last_disk_bytes = h.total_bytes;
    klog("fs: loaded %u nodes, %u bytes\n", h.node_count, h.total_bytes);
    kfree(nodes);
    if (disk_img) kfree(disk_img);
    disk_img = buf;
    disk_img_sectors = sectors;
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
    if (root) free_node(root);
    root = new_node("", true);
    fs_mkdirs("/home/notes");
    fs_mkdirs("/home/pictures");
    fs_mkdirs("/home/games");
    fs_mkdirs("/system");
    fs_mkdirs("/trash");
    fs_write_text("/home/README.txt", README);
    fs_write_text("/home/notes/todo.txt",
                  "- learn the silicon shell\n- paint something in Pixel\n- beat minesweeper on hard\n");
    fs_write_text("/home/notes/ideas.txt", "an OS named after the stuff it runs on.\n");
    fs_write_text("/system/version", "silicon " SILICON_VERSION "\n");
    fs_sync();
}

void fs_init() {
    persistent = ata_init() && ata_sectors() > FS_LBA + 64;
    if (persistent && load()) return;
    klog("fs: formatting %s filesystem\n", persistent ? "disk" : "RAM-only");
    fs_format();
}
