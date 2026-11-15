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
    u64      gs_base;              // what gs points at in ring 3 (the Win32 runtime keeps its TEB there)
    u64      wait_until;           // a blocking win_event with a timeout ends at this time (0 = none)
    bool     in_fault;             // running the process's own fault handler
};

// events a process reads with win_event (keep in sync with sdk/user/silicon_user.h)
enum { EV_NONE, EV_KEY, EV_MOUSE, EV_CLOSE, EV_RESIZE, EV_USER };
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
    u64      mmap_next;            // where the next mem_map without an address goes
    u64      fault_handler;        // user function that gets a UFault when a thread faults (0 = the process just ends)
    char*    cmdline;              // what it was started with ("" when nothing)
    char*    cwd;                  // the folder it was started from
    // a console: the shell that started it shows con_out and feeds con_in (see term/proc_cmds.cpp)
    bool     con_hold;             // the shell still wants what is left in con_out: don't free the process yet
    bool     con_eof;              // no more input will come
    u8*      con_out;
    u32      co_head, co_tail;
    u8*      con_in;
    u32      ci_head, ci_tail;
};
static const u32 CON_OUT_SIZE = 64 * 1024, CON_IN_SIZE = 2048;

// what a process's fault handler is given (keep in sync with sdk/user/silicon_user.h)
struct UFault { u64 int_no, err, cr2, rip, rsp, rflags, rax, rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10, r11, r12, r13, r14, r15; };

// ---- called from the kernel's main loop
void proc_init();
Proc* proc_spawn(const char* path, char* err, int errlen, const char* cmdline = nullptr, const char* cwd = nullptr);   // from a .uapp in SiliconFS
// from a .uapp image in memory; with_console gives it console buffers the caller must serve (and sets con_hold)
Proc* proc_spawn_image(const u8* data, u32 size, const char* name, const char* path, const char* cmdline, const char* cwd,
                       bool with_console, char* err, int errlen);
int   proc_con_take(Proc* p, char* buf, int cap);            // output the process wrote since the last call
void  proc_con_input(Proc* p, const char* s, int n);         // typed input for it
// run a Windows program (.exe) with the Win32 runtime (sdk/win32): nullptr and a message in err when it can't start
// cmdline: [options] <program.exe> [arguments], as typed after "win32run"
Proc* win32_spawn(const char* cmdline, const char* cwd, bool with_console, char* err, int errlen);
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
