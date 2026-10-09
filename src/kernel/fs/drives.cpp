// silicon - removable drives: mounts new USB drives under /mnt, writes their changes back, and
// cleans up when they are ejected or pulled out.
#include "drives.h"
#include "fat.h"

void ui_notify(const char* title, const char* body, const char* icon_name);

struct Mount {
    BlkDev* dev;
    u32 serial;
    FatVol* vol;
    FsNode* node;
    char path[48];
    char label[12];
    bool ejected;                      // the user ejected it: leave it alone until it is plugged in again
    bool failed;                       // we tried and it didn't work: don't try again for this insertion
    u64 dirty_since;
};
static Mount mounts[6];

static Mount* mount_for(BlkDev* d) { for (auto& m : mounts) if (m.dev == d) return &m; return nullptr; }

static void clear_fat_flags(FsNode* n) {
    n->fat_vol = 0; n->fat_state = 0; n->mount = 0;
    for (int i = 0; i < n->children.len; i++) clear_fat_flags(n->children[i]);
}

static void drop_tree(Mount* m) {
    if (m->node) {
        clear_fat_flags(m->node);
        FsRoot as_root;
        fs_remove(m->node);
    }
    m->node = nullptr;
    m->vol = nullptr;
}

static void do_mount(Mount* m, BlkDev* d) {
    m->dev = d;
    m->serial = d->serial;
    m->ejected = m->failed = false;
    m->vol = nullptr;
    m->node = nullptr;
    // the label decides the folder name, so mount to a temporary name first
    char err[96] = "";
    char label[12] = "";
    FsNode* mnt = fs_lookup("/mnt");
    if (!mnt) { FsRoot as_root; mnt = fs_mkdirs("/mnt"); }
    char tmpname[24];
    snprintf(tmpname, sizeof(tmpname), "%s", d->name);
    FsNode* node;
    {
        FsRoot as_root;
        node = fs_create(mnt, tmpname, true);
        if (node) { node->uid = FS_UID_USER; node->mode = 0755; }
    }
    if (!node) { m->failed = true; return; }
    FatVol* v = fat_mount(d, node, label, err, sizeof(err));
    if (!v) {
        clear_fat_flags(node);
        { FsRoot as_root; fs_remove(node); }
        m->failed = true;
        ui_notify("Can't open the drive", err, "storage");
        klog("drives: %s: %s\n", d->name, err);
        return;
    }
    // rename to the volume label
    char nm[FS_NAME_MAX];
    strlcpy(nm, label[0] ? label : d->name, sizeof(nm));
    for (char* p = nm; *p; p++) if (*p == '/') *p = '_';
    if (strcmp(nm, node->name)) {
        char uniq[FS_NAME_MAX];
        fs_unique_name(mnt, nm, uniq, sizeof(uniq));
        strlcpy(node->name, uniq, FS_NAME_MAX);            // same parent: a plain rename
    }
    m->vol = v;
    m->node = node;
    fs_path(node, m->path, sizeof(m->path));
    strlcpy(m->label, label, sizeof(m->label));
    char body[96];
    snprintf(body, sizeof(body), "%s is open at %s", label[0] ? label : d->name, m->path);
    ui_notify("Drive connected", body, "storage");
}

static void do_unmount(Mount* m, bool write_back) {
    if (m->vol) fat_unmount(m->vol, write_back);
    drop_tree(m);
}

void drives_poll() {
    static u64 last = 0;
    if (ticks() - last < 300) return;
    last = ticks();
    for (int i = 0; i < blk_count(); i++) {
        BlkDev* d = blk_get(i);
        Mount* m = mount_for(d);
        if (d->present) {
            if (!m) { for (auto& s : mounts) if (!s.dev) { m = &s; break; } if (!m) continue; memset(m, 0, sizeof(*m)); m->dev = d; m->serial = 0; }
            if (m->serial != d->serial) {                          // a drive was just plugged in
                if (m->vol) do_unmount(m, false);
                do_mount(m, d);
            }
        } else if (m && (m->vol || m->node)) {
            char body[96];
            snprintf(body, sizeof(body), "%s was removed without being ejected; recent changes may be lost", m->label[0] ? m->label : d->name);
            bool had_changes = m->vol && fat_dirty(m->vol);
            do_unmount(m, false);
            ui_notify(had_changes ? "Drive removed too early" : "Drive removed", had_changes ? body : "the drive is gone", "storage");
        } else if (m && !d->present) { m->serial = 0; m->ejected = m->failed = false; }
    }
}

void drives_tick() {
    for (auto& m : mounts) {
        if (!m.vol) continue;
        if (!fat_dirty(m.vol)) { m.dirty_since = 0; continue; }
        if (!m.dirty_since) m.dirty_since = ticks();
        if (ticks() - m.dirty_since > 1200) { fat_sync(m.vol); m.dirty_since = 0; }
    }
}

int drives_count() { int n = 0; for (auto& m : mounts) if (m.vol) n++; return n; }
bool drives_info(int i, DriveInfo* out) {
    int n = 0;
    for (auto& m : mounts) {
        if (!m.vol) continue;
        if (n++ != i) continue;
        strlcpy(out->path, m.path, sizeof(out->path));
        strlcpy(out->label, m.label[0] ? m.label : m.dev->name, sizeof(out->label));
        strlcpy(out->device, m.dev->name, sizeof(out->device));
        strlcpy(out->type, fat_type_name(m.vol), sizeof(out->type));
        out->total_kb = fat_total_kb(m.vol);
        out->free_kb = fat_free_kb(m.vol);
        out->dirty = fat_dirty(m.vol);
        return true;
    }
    return false;
}

bool drive_eject(const char* what, char* msg, int n) {
    for (auto& m : mounts) {
        if (!m.vol) continue;
        if (strcmp(what, m.dev->name) && strcmp(what, m.path) && strcmp(what, m.label) && !(m.node && !strcmp(what, m.node->name))) continue;
        bool ok = fat_sync(m.vol);
        if (!ok) { snprintf(msg, n, "couldn't write the changes back to %s", m.label[0] ? m.label : m.dev->name); return false; }
        char name[24];
        strlcpy(name, m.label[0] ? m.label : m.dev->name, sizeof(name));
        do_unmount(&m, true);
        m.ejected = true;
        snprintf(msg, n, "%s can be removed now", name);
        ui_notify("Safe to remove", msg, "storage");
        return true;
    }
    snprintf(msg, n, "no mounted drive called %s", what);
    return false;
}

bool drive_node_is_mount(FsNode* node) { for (auto& m : mounts) if (m.node == node && m.vol) return true; return false; }
void drives_sync_all() { for (auto& m : mounts) if (m.vol) fat_sync(m.vol); }
