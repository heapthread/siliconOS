// silicon - SiliconFS: an in-memory tree serialized to its own disk region
#pragma once
#include "../core.h"

static const u32 FS_LBA = 32768;           // keep in sync with build.py
static const int FS_NAME_MAX = 48;
static const int FS_ORIGIN_MAX = 96;

struct FsNode {
    char    name[FS_NAME_MAX];
    bool    dir;
    FsNode* parent;
    Vec<FsNode*> children;
    u8*     data;
    u32     size, cap;
    u32     ctime, mtime;
    char    origin[FS_ORIGIN_MAX];       // where a trashed node came from
};

void    fs_init();
bool    fs_persistent();                 // backed by a disk
FsNode* fs_root();
FsNode* fs_home();
FsNode* fs_trash_dir();
FsNode* fs_lookup(const char* path, FsNode* cwd = nullptr);
FsNode* fs_child(FsNode* dir, const char* name);
FsNode* fs_create(FsNode* dir, const char* name, bool is_dir);   // returns existing if present & same kind
FsNode* fs_create_path(const char* path, bool is_dir, FsNode* cwd = nullptr);
FsNode* fs_mkdirs(const char* path);
bool    fs_write(FsNode* f, const void* data, u32 len);
bool    fs_append(FsNode* f, const void* data, u32 len);
bool    fs_write_text(const char* path, const char* text);
const char* fs_read_text(FsNode* f);     // NUL-terminated view (data has a spare byte)
void    fs_remove(FsNode* n);            // permanent, recursive
bool    fs_move(FsNode* n, FsNode* new_parent, const char* new_name);
FsNode* fs_copy(FsNode* n, FsNode* new_parent, const char* new_name);
bool    fs_is_ancestor(FsNode* a, FsNode* n);
void    fs_path(FsNode* n, char* buf, int len);
bool    fs_valid_name(const char* name);
void    fs_unique_name(FsNode* dir, const char* base, char* out, int len);
u32     fs_tree_size(FsNode* n);
int     fs_count(FsNode* n);
void    fs_sort(FsNode* dir);            // folders first, then alphabetical

// trash
FsNode* fs_trash(FsNode* n);
FsNode* fs_restore(FsNode* n);
void    fs_empty_trash();

// persistence
void fs_mark_dirty();
u32  fs_generation();                    // bumps on every change (for live views)
void fs_tick();                          // sync if dirty for a little while
bool fs_sync();
u32  fs_disk_used();
u32  fs_disk_capacity();
void fs_format();
