// silicon - processes and threads: ring 3 programs with their own address space, a preemptive
// scheduler and the system call interface (see sdk/user/silicon_user.h for the user side).
//
// The kernel itself is one cooperative thread. At a safe point in its main loop (proc_slice) it
// hands the cpu to the user threads; the timer interrupt takes it back after a budget, and moves
// between user threads when a time slice runs out. A user program can't touch kernel memory or
// another program's memory: a stray access is a page fault and ends only that process.
#pragma once
#include "../core.h"
#include "../arch/arch.h"

enum { T_RUN, T_SLEEP, T_WAIT, T_DEAD };

struct Proc;
struct Thread {
    int      tid;
    Proc*    proc;                 // nullptr for the kernel's main thread
    IntFrame* frame;               // saved context while it isn't running
    u8*      kstack;
    u64      kstack_top;
    int      state;
    u64      wake_ms;
    u64      cpu_tsc;
    int      stack_slot;
};

// events a process reads with win_event (keep in sync with sdk/user/silicon_user.h)
enum { EV_NONE, EV_KEY, EV_MOUSE, EV_CLOSE, EV_RESIZE };
struct UEvent { u32 type; i32 a, b, c, d; };

class UserApp;
struct Proc {
    int      pid;
    char     name[48];
    char     path[96];
    bool     dead;
    int      exit_code;
    char     why[64];              // why it ended
    u64      pml4;                 // physical address of its top-level page table
    u64*     pdpt;
    u64*     pd;
    Vec<u64> pt_pages;
    struct Region { u64 va, pa; u32 pages; };
    Vec<Region> regions;
    u32      heap_len;
    Vec<Thread*> threads;
    int      stack_slots_used;
    UserApp* app;
    u32*     surf;                 // the window's pixels (shared with the kernel's window)
    int      sw, sh;
    u32      surf_pages;
    bool     dirty;
    UEvent   evq[64];
    int      ev_head, ev_tail;
    u64      start_ms;
    u64      cpu_tsc;
    u32      mem_pages;
};

// ---- called from the kernel's main loop
void proc_init();
Proc* proc_spawn(const char* path, char* err, int errlen);   // from a .uapp in SiliconFS
void  proc_slice(u32 budget_ms);       // let user threads run for a while (the kernel thread is parked here)
bool  proc_busy();                     // a user thread wants the cpu: don't halt
void  proc_kill(Proc* p, const char* why);
void  proc_reap();                     // free what dead processes left behind
int   proc_count();
Proc* proc_at(int i);
Proc* proc_by_pid(int pid);
void  proc_push_event(Proc* p, const UEvent& e);
int   proc_cpu_permille();             // share of the last second spent in user code

// ---- implemented in ui/apps/userapp.cpp
UserApp* userapp_create(Proc* p, int w, int h, const char* title, int flags);
void     userapp_set_title(UserApp* a, const char* title);
void     userapp_close(UserApp* a);
void     userapp_detach(UserApp* a);    // the process is gone
int      userapp_screen_pixels();
