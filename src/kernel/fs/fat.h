// silicon - FAT16 / FAT32 volumes on block devices (USB drives): the whole tree is imported into
// SiliconFS under /mnt/<label>, and changes are written back to the drive (fat_sync) while it is
// mounted. Long file names are supported; FAT12 is not.
#pragma once
#include "fs.h"
#include "../drv/blk.h"

struct FatVol;

enum { FAT_SYNCED = 1, FAT_NEW = 2, FAT_MODIFIED = 3, FAT_OVERSIZE = 4 };   // FsNode::fat_state

FatVol* fat_mount(BlkDev* dev, FsNode* mount_point, char* label_out, char* err, int errlen);   // imports the volume under mount_point
bool    fat_sync(FatVol* v);                  // write every pending change to the drive
void    fat_unmount(FatVol* v, bool write_back);
bool    fat_dirty(FatVol* v);                 // changes waiting to be written
bool    fat_readonly(FatVol* v);
u32     fat_total_kb(FatVol* v);
u32     fat_free_kb(FatVol* v);
const char* fat_type_name(FatVol* v);         // "FAT16" / "FAT32"

// hooks called by fs.cpp when a node inside a mounted volume is touched
void    fat_note_modified(FsNode* n);
void    fat_note_removed(FsNode* n);          // before the node is freed: remembers what to delete on the drive
FatVol* fat_vol_by_id(int id);                // FsNode::fat_vol
