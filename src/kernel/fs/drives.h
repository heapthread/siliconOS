// silicon - mounted removable drives
#pragma once
#include "fs.h"
#include "../drv/blk.h"

struct DriveInfo { char path[48], label[12], device[12], type[8]; u32 total_kb, free_kb; bool dirty; };

void drives_poll();                  // notice drives coming and going (call from the main loops)
void drives_tick();                  // write pending changes back after a short pause
int  drives_count();
bool drives_info(int i, DriveInfo* out);
bool drive_eject(const char* what, char* msg, int n);     // device name, label or /mnt path
bool drive_node_is_mount(FsNode* node);
void drives_sync_all();
