// silicon - processes, threads, scheduler and system calls
#include "proc.h"
#include "../fs/fs.h"
#include "../ui/wm.h"
#include "../text.h"
#include "../drv/drivers.h"
#include "../gfx/vfont.h"

void panic_frame(const char* what, IntFrame* f);

// ---------------------------------------------------------------- the user address space layout
// Everything a process owns lives between 1 GiB and 2 GiB; the kernel identity-maps the first
// 4 GiB without the user bit, so none of it is reachable from ring 3.
static const u64 USER_BASE   = 0x40000000ull;     // image (code, data, bss)
static const u64 USER_HEAP   = 0x48000000ull;
static const u64 USER_SURF   = 0x58000000ull;     // window pixels
static const u64 USER_STACK  = 0x60000000ull;     // thread stacks, 256 KiB apart
static const u64 USER_END    = 0x80000000ull;
// A second, much larger area from 4 GiB to 512 GiB (the rest of the first top-level slot, which the kernel
// doesn't use) holds what a program maps itself with mem_map: the Win32 runtime puts Windows programs at
// the address they were linked for (usually 5 GiB) and their heaps and stacks here.
static const u64 USER_HI     = 0x100000000ull;
static const u64 USER_HI_END = 0x8000000000ull;
static const u64 MMAP_BASE   = 0x7000000000ull;   // mem_map without an address (and window buffers) come from here:
                                                  // the top 64 GiB, so they never meet what a program maps at addresses it chose
static const u32 STACK_PAGES = 32;                // 128 KiB per thread
static const u32 KSTACK_SIZE = 32 * 1024;
static const int MAX_THREADS = 8;                 // threads on kernel-made stacks (the old thread call)
static const int MAX_THREADS_ALL = 64;            // and in total, counting those that bring their own stack
// page table bits
static const u64 PTE_P = 1, PTE_W = 2, PTE_U = 4, PTE_HELD = 0x200;   // HELD: mapped but no access allowed (not present)
static const u64 PTE_ADDR = 0x000FFFFFFFFFF000ull;
static u64 pte_nx = 0;                            // bit 63 when the cpu can mark pages non-executable
enum { PROT_R = 1, PROT_W = 2, PROT_X = 4 };
static const u32 MAX_IMAGE   = 8u << 20;
static const u32 MAX_HEAP    = 96u << 20;
static const u32 SLICE_MS    = 8;

// ---------------------------------------------------------------- physical pages for user memory
static u8*  arena = nullptr;
static u8*  page_used = nullptr;
static u32  arena_pages = 0;

static void pmm_init() {
    if (arena) return;
    u32 want = 160u << 20;                         // ask for what the heap can spare, down to 24 MiB
    u8* raw = nullptr;
    while (want >= (24u << 20) && !(raw = (u8*)kmalloc_try(want + 4096))) want -= 8u << 20;
    if (!raw) panic("proc: no memory for user programs");
    arena = (u8*)(((u64)raw + 4095) & ~4095ull);
    arena_pages = want / 4096;
    page_used = (u8*)kcalloc(arena_pages);
    klog("proc: %u MiB for user programs\n", want >> 20);
}

static u64 pmm_alloc(u32 n) {            // n contiguous zeroed pages; 0 when full
    u32 run = 0;
    for (u32 i = 0; i < arena_pages; i++) {
        run = page_used[i] ? 0 : run + 1;
        if (run == n) {
            u32 s = i + 1 - n;
            for (u32 k = 0; k < n; k++) page_used[s + k] = 1;
            u8* p = arena + (u64)s * 4096;
            memset(p, 0, (u64)n * 4096);
            return (u64)p;
        }
    }
    return 0;
}
static void pmm_free(u64 pa, u32 n) {
    u32 s = (u32)((pa - (u64)arena) / 4096);
    for (u32 k = 0; k < n; k++) page_used[s + k] = 0;
}

// ---------------------------------------------------------------- bookkeeping
static Thread main_thread;
static Thread* cur = &main_thread;
static Vec<Thread*> uthreads;
static Vec<Proc*> procs;
static int next_pid = 1, next_tid = 1;
static u64 kernel_cr3 = 0;
static u64 budget_end = 0, slice_end = 0;
static u32 g_budget = 4;
static Thread* last_run = nullptr;
static u64 slice_t0 = 0;                 // tsc when the running thread got the cpu
static u64 user_tsc_window = 0, window_t0 = 0, user_permille = 0;

static inline u64 rdtsc() { u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }

static inline u64 rdmsr(u32 msr) { u32 lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr)); return ((u64)hi << 32) | lo; }
static inline void wrmsr(u32 msr, u64 v) { __asm__ volatile("wrmsr" :: "c"(msr), "a"((u32)v), "d"((u32)(v >> 32))); }
static const u32 MSR_EFER = 0xC0000080, MSR_GS_BASE = 0xC0000101;

void proc_init() {
    kernel_cr3 = read_cr3();
    // non-executable pages: data a program maps can't be run as code
    u32 a = 0x80000001, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if (d & (1u << 20)) { wrmsr(MSR_EFER, rdmsr(MSR_EFER) | (1 << 11)); pte_nx = 1ull << 63; }
    main_thread.tid = 0;
    main_thread.state = T_RUN;
    cur = &main_thread;
}

// ---------------------------------------------------------------- address spaces
static bool va_user(u64 va) { return (va >= USER_BASE && va < USER_END) || (va >= USER_HI && va < USER_HI_END); }

// the page directory that covers va (each one spans 1 GiB); made on demand for the area above 4 GiB
static u64* pd_of(Proc* p, u64 va, bool create) {
    u64 pi = (va >> 30) & 511;
    if (!(p->pdpt[pi] & PTE_P)) {
        if (!create) return nullptr;
        u64 pg = pmm_alloc(1);
        if (!pg) return nullptr;
        p->pt_pages.push(pg);
        p->pdpt[pi] = pg | 7;
    }
    return (u64*)(p->pdpt[pi] & PTE_ADDR);
}

static u64* pte_of(Proc* p, u64 va, bool create) {
    if (!va_user(va)) return nullptr;
    u64* pd = pd_of(p, va, create);
    if (!pd) return nullptr;
    u64 pdi = (va >> 21) & 511;
    if (!(pd[pdi] & PTE_P)) {
        if (!create) return nullptr;
        u64 pg = pmm_alloc(1);
        if (!pg) return nullptr;
        p->pt_pages.push(pg);
        pd[pdi] = pg | 7;
    }
    return &((u64*)(pd[pdi] & PTE_ADDR))[(va >> 12) & 511];
}

static u64 prot_bits(int prot) {
    if (!(prot & 7)) return PTE_HELD;                              // no access: kept, but not present
    return PTE_P | PTE_U | ((prot & PROT_W) ? PTE_W : 0) | ((prot & PROT_X) ? 0 : pte_nx);
}

static bool map_page(Proc* p, u64 va, u64 pa, u64 bits = 7) {
    u64* pte = pte_of(p, va, true);
    if (!pte) return false;
    *pte = pa | bits;
    return true;
}

// is [va, va+len) all mapped for this process (and writable, when the kernel is about to store there)?
static bool user_ok(Proc* p, u64 va, u64 len, bool write = false) {
    if (!len) return true;
    if (!va_user(va) || len > USER_HI_END || !va_user(va + len - 1)) return false;
    for (u64 a = va & ~4095ull; a < va + len; a += 4096) {
        u64* pte = pte_of(p, a, false);
        if (!pte || !(*pte & PTE_P) || (write && !(*pte & PTE_W))) return false;
    }
    return true;
}

static bool user_str(Proc* p, u64 va, char* out, int cap) {     // copy a NUL-terminated string in
    for (int i = 0; i < cap - 1; i++) {
        if (!user_ok(p, va + i, 1)) return false;
        char c = *(const char*)(va + i);
        out[i] = c;
        if (!c) return true;
    }
    out[cap - 1] = 0;
    return true;
}

static void flush_tlb(Proc* p) { if (read_cr3() == p->pml4) write_cr3(p->pml4); }

// is any page of [va, va + pages) taken already?
static bool range_used(Proc* p, u64 va, u64 pages) {
    for (u64 i = 0; i < pages; i++) {
        u64* pte = pte_of(p, va + i * 4096, false);
        if (pte && (*pte & (PTE_P | PTE_HELD))) return true;
    }
    return false;
}

// give pages back. Regions that are only partly covered are split
static void unmap_range(Proc* p, u64 va, u64 pages) {
    u64 end = va + pages * 4096;
    for (int i = p->regions.len - 1; i >= 0; i--) {
        Proc::Region r = p->regions[i];
        u64 rs = r.va, re = r.va + (u64)r.pages * 4096;
        if (re <= va || rs >= end) continue;
        u64 cs = max(rs, va), ce = min(re, end);
        for (u64 a = cs; a < ce; a += 4096) { u64* pte = pte_of(p, a, false); if (pte) *pte = 0; }
        pmm_free(r.pa + (cs - rs), (u32)((ce - cs) / 4096));
        p->mem_pages -= (u32)((ce - cs) / 4096);
        p->regions.remove(i);
        if (rs < cs) { Proc::Region a = {rs, r.pa, (u32)((cs - rs) / 4096)}; p->regions.push(a); }
        if (ce < re) { Proc::Region b = {ce, r.pa + (ce - rs), (u32)((re - ce) / 4096)}; p->regions.push(b); }
    }
    flush_tlb(p);
}

// zeroed memory at va. The physical pages come in one piece when possible, in smaller pieces when not
static bool add_region(Proc* p, u64 va, u32 pages, u64* pa_out = nullptr, u64 bits = 7) {
    u32 done = 0, chunk = pages;
    while (done < pages) {
        u32 n = min(chunk, pages - done);
        u64 pa = pmm_alloc(n);
        if (!pa) {
            if (chunk == 1 || pa_out) { unmap_range(p, va, done); return false; }     // pa_out: the caller needs one piece
            chunk = max<u32>(1, chunk / 2);
            continue;
        }
        Proc::Region r = {va + (u64)done * 4096, pa, n};
        p->regions.push(r);
        p->mem_pages += n;
        for (u32 i = 0; i < n; i++)
            if (!map_page(p, r.va + (u64)i * 4096, pa + (u64)i * 4096, bits)) { unmap_range(p, va, done + n); return false; }
        if (pa_out) *pa_out = pa;
        done += n;
    }
    return true;
}

static bool make_space(Proc* p) {
    u64 pml4 = pmm_alloc(1), pdpt = pmm_alloc(1), pd = pmm_alloc(1);
    if (!pml4 || !pdpt || !pd) return false;
    u64* kp4 = (u64*)(kernel_cr3 & ~0xFFFull);
    memcpy((void*)pml4, kp4, 4096);
    u64* kpdpt = (u64*)(kp4[0] & ~0xFFFull);
    memcpy((void*)pdpt, kpdpt, 4096);
    ((u64*)pdpt)[1] = pd | 7;                       // 1..2 GiB becomes this process's private space
    ((u64*)pml4)[0] = pdpt | 7;
    p->pml4 = pml4; p->pdpt = (u64*)pdpt; p->pd = (u64*)pd;
    return true;
}

// ---------------------------------------------------------------- threads
static Thread* thread_new(Proc* p, u64 entry, u64 sp, u64 arg) {
    Thread* t = new Thread();
    t->tid = next_tid++;
    t->proc = p;
    t->kstack = (u8*)kmalloc(KSTACK_SIZE + 16);
    t->kstack_top = ((u64)t->kstack + KSTACK_SIZE + 15) & ~15ull;
    IntFrame* f = (IntFrame*)(t->kstack_top - sizeof(IntFrame));
    memset(f, 0, sizeof(IntFrame));
    f->rip = entry;
    f->cs = 0x23;
    f->ss = 0x1B;
    f->rflags = 0x202;
    f->rsp = sp;
    f->rcx = arg;
    f->rdi = arg;
    u8* fx = (u8*)((((u64)f) - 528) & ~15ull);
    alignas(16) static u8 tmp[512];
    fx_default_state(tmp);
    memcpy(fx, tmp, 512);
    t->frame = f;
    t->state = T_RUN;
    t->stack_slot = -1;
    p->threads.push(t);
    uthreads.push(t);
    return t;
}

static bool alloc_stack(Proc* p, u64* top_out) {
    int slot = p->stack_slots_used;
    if (slot >= MAX_THREADS) return false;
    u64 va = USER_STACK + (u64)slot * 0x40000;
    if (!add_region(p, va, STACK_PAGES)) return false;
    p->stack_slots_used++;
    *top_out = va + (u64)STACK_PAGES * 4096 - 64;
    return true;
}

// ---------------------------------------------------------------- scheduler
static void wake_sleepers(u64 now) {
    for (int i = 0; i < uthreads.len; i++) {
        Thread* t = uthreads[i];
        if (t->state == T_SLEEP && now >= t->wake_ms) t->state = T_RUN;
        if (t->state == T_WAIT && t->wait_until && now >= t->wait_until) t->state = T_RUN;
    }
}

static Thread* pick_user(Thread* after) {
    int n = uthreads.len;
    if (!n) return nullptr;
    int s = after ? uthreads.find(after) : -1;
    for (int k = 1; k <= n; k++) {
        Thread* t = uthreads[((s < 0 ? -1 : s) + k + n) % n];
        if (t->state == T_RUN) return t;
    }
    return nullptr;
}

static IntFrame* switch_to(IntFrame* f, Thread* nx) {
    if (nx == cur) return f;
    u64 now = rdtsc();
    cur->frame = f;
    if (cur->proc) { u64 d = now - slice_t0; cur->cpu_tsc += d; cur->proc->cpu_tsc += d; user_tsc_window += d; }
    cur = nx;
    slice_t0 = now;
    slice_end = ticks() + SLICE_MS;
    if (nx->proc) {
        tss_set_rsp0(nx->kstack_top);
        if (read_cr3() != nx->proc->pml4) write_cr3(nx->proc->pml4);
        wrmsr(MSR_GS_BASE, nx->gs_base);
        last_run = nx;
    } else if (read_cr3() != kernel_cr3) write_cr3(kernel_cr3);
    return nx->frame;
}

// the running user thread can't continue (blocked, ended): give the cpu to another one or to the kernel
static IntFrame* sched_block(IntFrame* f) {
    u64 now = ticks();
    wake_sleepers(now);
    if (now < budget_end) if (Thread* nx = pick_user(cur)) if (nx != cur || nx->state == T_RUN) return switch_to(f, nx);
    return switch_to(f, &main_thread);
}

IntFrame* sched_tick(IntFrame* f) {
    if (!cur->proc) return f;                       // the kernel thread runs until it hands over
    u64 now = ticks();
    wake_sleepers(now);
    if ((f->cs & 3) != 3) return f;
    if (now >= budget_end) return switch_to(f, &main_thread);
    if (now >= slice_end) {
        Thread* nx = pick_user(cur);
        if (nx && nx != cur) return switch_to(f, nx);
        slice_end = now + SLICE_MS;
    }
    return f;
}

IntFrame* sched_yield_main(IntFrame* f) {
    u64 now = ticks();
    wake_sleepers(now);
    Thread* nx = pick_user(last_run);
    if (!nx) return f;
    budget_end = now + g_budget;
    return switch_to(f, nx);
}

bool proc_busy() {
    u64 now = ticks();
    for (int i = 0; i < uthreads.len; i++) {
        Thread* t = uthreads[i];
        if (t->state == T_RUN || (t->state == T_SLEEP && now >= t->wake_ms)) return true;
        if (t->state == T_WAIT && t->wait_until && now >= t->wait_until) return true;
    }
    return false;
}

void proc_slice(u32 budget_ms) {
    if (!uthreads.len && !procs.len) return;
    proc_reap();
    u64 now = ticks();
    if (now - window_t0 >= 1000) {                  // user cpu share over the last second
        user_permille = min<u64>(1000, user_tsc_window / max<u64>(1, (u64)cpu_mhz() * (now - window_t0)));
        user_tsc_window = 0;
        window_t0 = now;
    }
    if (!proc_busy()) { wake_sleepers(now); if (!proc_busy()) return; }
    g_budget = budget_ms ? budget_ms : 1;
    __asm__ volatile("int $0x81" ::: "memory");
}

int proc_cpu_permille() { return (int)user_permille; }

// ---------------------------------------------------------------- processes
int   proc_count() { return procs.len; }
Proc* proc_at(int i) { return i >= 0 && i < procs.len ? procs[i] : nullptr; }
Proc* proc_by_pid(int pid) { for (int i = 0; i < procs.len; i++) if (procs[i]->pid == pid) return procs[i]; return nullptr; }

struct UappHeader { char magic[4]; u32 version, entry, image, bss, r0, r1, r2; };

Proc* proc_spawn_image(const u8* data, u32 size, const char* name, const char* path, const char* cmdline, const char* cwd,
                       bool with_console, char* err, int errlen) {
    pmm_init();
    if (!kernel_cr3) proc_init();
    if (size < sizeof(UappHeader) || memcmp(data, "UAPP", 4)) { snprintf(err, errlen, "not a silicon program (.uapp)"); return nullptr; }
    UappHeader h;
    memcpy(&h, data, sizeof(h));
    if (h.version != 1 || h.image > MAX_IMAGE || h.bss > MAX_IMAGE || h.entry >= h.image ||
        sizeof(h) + h.image > size) { snprintf(err, errlen, "damaged program file"); return nullptr; }
    Proc* p = new Proc();
    p->pid = next_pid++;
    strlcpy(p->name, name, sizeof(p->name));
    strlcpy(p->path, path, sizeof(p->path));
    p->cmdline = strdup(cmdline ? cmdline : "");
    p->cwd = strdup(cwd ? cwd : "/home");
    p->mmap_next = MMAP_BASE;
    p->start_ms = ticks();
    u64 stack_top = 0, pa = 0;
    u32 ipages = (h.image + h.bss + 4095) / 4096;
    if (!make_space(p) || !add_region(p, USER_BASE, ipages, &pa) || !alloc_stack(p, &stack_top)) {
        snprintf(err, errlen, "out of memory");
        p->dead = true;
        procs.push(p);
        proc_reap();
        return nullptr;
    }
    memcpy((void*)pa, data + sizeof(h), h.image);
    if (with_console) {
        p->con_out = (u8*)kmalloc(CON_OUT_SIZE);
        p->con_in = (u8*)kmalloc(CON_IN_SIZE);
        p->con_hold = true;
    }
    thread_new(p, USER_BASE + h.entry, stack_top, 0);
    procs.push(p);
    klog("proc: started %s (pid %d, %u KiB)\n", p->name, p->pid, (u32)(p->mem_pages * 4));
    return p;
}

Proc* proc_spawn(const char* path, char* err, int errlen, const char* cmdline, const char* cwd) {
    FsNode* n = fs_lookup(path);
    if (!n || n->dir) { snprintf(err, errlen, "no such program"); return nullptr; }
    char name[48];
    const char* base = strrchr(path, '/');
    strlcpy(name, base ? base + 1 : path, sizeof(name));
    char* dot = strrchr(name, '.');
    if (dot) *dot = 0;
    return proc_spawn_image(n->data, n->size, name, path, cmdline, cwd, false, err, errlen);
}

// ---------------------------------------------------------------- a process's console
int proc_con_take(Proc* p, char* buf, int cap) {
    int n = 0;
    if (!p->con_out) return 0;
    while (n < cap && p->co_head != p->co_tail) {
        buf[n++] = (char)p->con_out[p->co_head];
        p->co_head = (p->co_head + 1) % CON_OUT_SIZE;
    }
    return n;
}

void proc_con_input(Proc* p, const char* s, int n) {
    if (!p->con_in) return;
    if (!s) { p->con_eof = true; return; }
    for (int i = 0; i < n; i++) {
        u32 nt = (p->ci_tail + 1) % CON_IN_SIZE;
        if (nt == p->ci_head) break;
        p->con_in[p->ci_tail] = (u8)s[i];
        p->ci_tail = nt;
    }
}

void proc_kill(Proc* p, const char* why) {
    if (p->dead) return;
    p->dead = true;
    strlcpy(p->why, why, sizeof(p->why));
    for (int i = 0; i < p->threads.len; i++) p->threads[i]->state = T_DEAD;
    klog("proc: %s (pid %d) ended: %s\n", p->name, p->pid, why);
}

// free the memory of finished processes. Only called from the kernel thread, so nothing of theirs is running
void proc_reap() {
    for (int i = procs.len - 1; i >= 0; i--) {
        Proc* p = procs[i];
        if (!p->dead) {
            // threads that ended in a program that is still running
            for (int k = p->threads.len - 1; k >= 0; k--) {
                Thread* t = p->threads[k];
                if (t->state != T_DEAD || t == cur) continue;
                int u = uthreads.find(t);
                if (u >= 0) uthreads.remove(u);
                if (last_run == t) last_run = nullptr;
                kfree(t->kstack);
                delete t;
                p->threads.remove(k);
            }
            continue;
        }
        for (int w = 0; w < MAX_UWIN; w++) if (p->win[w].app) { userapp_detach(p->win[w].app); p->win[w].app = nullptr; }
        if (p->con_hold) continue;                   // the shell hasn't shown everything it printed yet
        for (int k = 0; k < p->threads.len; k++) {
            Thread* t = p->threads[k];
            int u = uthreads.find(t);
            if (u >= 0) uthreads.remove(u);
            if (last_run == t) last_run = nullptr;
            kfree(t->kstack);
            delete t;
        }
        p->threads.free();
        for (int k = 0; k < p->regions.len; k++) pmm_free(p->regions[k].pa, p->regions[k].pages);
        for (int k = 0; k < p->pt_pages.len; k++) pmm_free(p->pt_pages[k], 1);
        if (p->pml4) { pmm_free(p->pml4, 1); pmm_free((u64)p->pdpt, 1); pmm_free((u64)p->pd, 1); }
        p->regions.free();
        p->pt_pages.free();
        if (p->cmdline) kfree(p->cmdline);
        if (p->cwd) kfree(p->cwd);
        if (p->con_out) kfree(p->con_out);
        if (p->con_in) kfree(p->con_in);
        procs.remove(i);
        delete p;
    }
}

IntFrame* proc_fault(IntFrame* f, const char* what) {
    Proc* p = cur->proc;
    if (!p) panic_frame(what, f);
    u64 cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    // a program that asked for it hears about its own faults first (the Win32 runtime reports where it
    // happened, and turns them into Windows exceptions). A fault inside that handler ends the process.
    if (p->fault_handler && !cur->in_fault && !p->dead) {
        u64 sp = ((f->rsp - 256 - sizeof(UFault)) & ~15ull);
        if (user_ok(p, sp - 64, sizeof(UFault) + 64, true) && user_ok(p, p->fault_handler, 1)) {
            UFault* u = (UFault*)sp;
            u->int_no = f->int_no; u->err = f->err; u->cr2 = cr2; u->rip = f->rip; u->rsp = f->rsp; u->rflags = f->rflags;
            u->rax = f->rax; u->rbx = f->rbx; u->rcx = f->rcx; u->rdx = f->rdx; u->rsi = f->rsi; u->rdi = f->rdi; u->rbp = f->rbp;
            u->r8 = f->r8; u->r9 = f->r9; u->r10 = f->r10; u->r11 = f->r11; u->r12 = f->r12; u->r13 = f->r13; u->r14 = f->r14; u->r15 = f->r15;
            // a machine frame (rip, cs, rflags, rsp, ss) where a return address would go: the handler's unwind
            // data says so, which lets the faulting thread's stack be walked through the handler
            u64* mf = (u64*)(sp - 40);
            mf[0] = f->rip; mf[1] = 0x23; mf[2] = f->rflags; mf[3] = f->rsp; mf[4] = 0x1B;
            static u32 reported = 0;
            if (reported++ < 32) klog("proc: %s (pid %d) %s at %lx (address %lx, gs %lx): passed to its fault handler\n", p->name, p->pid, what, f->rip, cr2, cur->gs_base);
            cur->in_fault = true;
            f->rcx = sp;
            f->rdi = sp;
            f->rip = p->fault_handler;
            f->rsp = sp - 40;
            f->rflags &= ~0x400ull;
            return f;
        }
    }
    char why[64];
    if (f->int_no == 14) snprintf(why, sizeof(why), "%s at %lx (address %lx)", what, f->rip, cr2);
    else snprintf(why, sizeof(why), "%s at %lx", what, f->rip);
    proc_kill(p, why);
    return sched_block(f);
}

// ---------------------------------------------------------------- system calls
enum {
    SYS_EXIT = 0, SYS_YIELD = 1, SYS_SLEEP = 2, SYS_TICKS = 3, SYS_LOG = 4, SYS_GETPID = 5, SYS_RAND = 6,
    SYS_WIN_OPEN = 10, SYS_WIN_PRESENT = 11, SYS_WIN_TITLE = 12, SYS_WIN_EVENT = 13, SYS_WIN_SIZE = 14,
    SYS_GFX = 20,
    SYS_FILE_READ = 30, SYS_FILE_WRITE = 31, SYS_FILE_SIZE = 32, SYS_DIR_ENTRY = 33,
    SYS_MEM_GROW = 40, SYS_THREAD = 41, SYS_THREAD_EXIT = 42,
    SYS_CLIP_SET = 50, SYS_CLIP_GET = 51, SYS_NOTIFY = 60,
    // added for the Win32 runtime (sdk/win32), usable by any program
    SYS_TICKS_US = 7, SYS_TIME = 8, SYS_PROC_INFO = 9, SYS_WIN_POST = 15, SYS_TEXT = 21,
    SYS_FILE_STAT = 34, SYS_FILE_DELETE = 35, SYS_MKDIR = 36, SYS_FILE_RENAME = 37,
    SYS_THREAD_AT = 43, SYS_SET_GS = 44, SYS_MEM_MAP = 45, SYS_MEM_UNMAP = 46, SYS_MEM_PROTECT = 47,
    SYS_FAULT_HANDLER = 48, SYS_FAULT_RETURN = 49,
    SYS_CON_WRITE = 70, SYS_CON_READ = 71,
    SYS_WIN_CREATE = 16, SYS_WIN_CTL = 17, SYS_WIN_EVENT2 = 18,
};
enum { GFX_FILL = 1, GFX_RRECT = 2, GFX_TEXT = 3, GFX_TEXT_W = 4, GFX_ICON = 5, GFX_LINE = 6, GFX_DISC = 7, GFX_CLEAR = 8, GFX_RECT = 9 };

// what a program may read and write: its own corner of the disk, not the system's secrets
static const int EVQ = 64;
static bool has_dotdot(const char* s) { for (; s[0] && s[1]; s++) if (s[0] == '.' && s[1] == '.') return true; return false; }
static bool path_ok(const char* path, bool write) {
    if (path[0] != '/' || has_dotdot(path)) return false;
    if (str_starts(path, "/home/.config/mail") || str_starts(path, "/system")) return false;
    if (write) return (str_starts(path, "/home/") && !str_starts(path, "/home/.config/")) || str_starts(path, "/mnt/");
    return true;
}

static bool surf_canvas(Proc* p, Canvas& c) {
    UWin& w = p->win[0];
    if (!w.surf || w.sw <= 0 || w.sh <= 0) return false;
    c.init(w.surf, w.sw, w.sh, w.sw);
    return true;
}

static i64 sys_gfx(Proc* p, u64 op, u64 argp, u64 strp) {
    int a[8];
    if (!user_ok(p, argp, sizeof(a))) return -1;
    memcpy(a, (void*)argp, sizeof(a));
    Canvas c;
    if (!surf_canvas(p, c)) return -1;
    char buf[260], sil[300];
    sil[0] = 0;
    if (op == GFX_TEXT || op == GFX_TEXT_W || op == GFX_ICON) {
        if (!user_str(p, strp, buf, sizeof(buf))) return -1;
        utf8_to_silicon(buf, (u32)strlen(buf), sil, sizeof(sil));
    }
    switch (op) {
    case GFX_CLEAR: c.clear((u32)a[0] | 0xFF000000); break;
    case GFX_FILL: c.fill(a[0], a[1], a[2], a[3], (u32)a[4] | 0xFF000000); break;
    case GFX_RECT: c.rect(a[0], a[1], a[2], a[3], (u32)a[4] | 0xFF000000); break;
    case GFX_RRECT: c.rrect(a[0], a[1], a[2], a[3], a[4], (u32)a[5] | 0xFF000000); break;
    case GFX_LINE: c.line(a[0], a[1], a[2], a[3], (u32)a[4] | 0xFF000000); break;
    case GFX_DISC: c.disc(a[0], a[1], a[2], (u32)a[3] | 0xFF000000); break;
    case GFX_TEXT: {
        int scale = max(1, (a[3] >> 8) & 7);
        return c.text(a[0], a[1], sil, (u32)a[2] | 0xFF000000, scale, (a[3] & 1) != 0);
    }
    case GFX_TEXT_W: return text_width(sil, max(1, (a[3] >> 8) & 7), (a[3] & 1) != 0);
    case GFX_ICON: {
        Icon* ic = icon(buf);
        const Image& im = a[2] >= 48 ? ic->big : a[2] >= 24 ? ic->mid : ic->small;
        c.blit(im, a[0], a[1]);
        break;
    }
    default: return -1;
    }
    p->win[0].dirty = true;
    return 0;
}

static i64 sys_win_open(Proc* p, int w, int h, u64 title_va, int flags) {
    if (p->win[0].used || w < 80 || h < 60 || w > 4096 || h > 4096) return 0;
    char title[48];
    if (!user_str(p, title_va, title, sizeof(title))) strlcpy(title, p->name, sizeof(title));
    u32 maxpx = max<u32>((u32)userapp_screen_pixels(), (u32)(w * h));
    maxpx = min<u32>(maxpx, 3840u * 2160u);
    u32 pages = (maxpx * 4 + 4095) / 4096;
    u64 pa = 0;
    if (!add_region(p, USER_SURF, pages, &pa)) return 0;
    UWin& u = p->win[0];
    memset(&u, 0, sizeof(u));
    u.used = true;
    u.surf = (u32*)pa;
    u.va = USER_SURF;
    u.pages = pages;
    u.sw = w; u.sh = h;
    u.resizable = (flags & 1) != 0;
    u.x = u.y = -1;
    strlcpy(u.title, title, sizeof(u.title));
    memset(u.surf, 0xFF, (u64)w * h * 4);
    u.app = userapp_create(p, 0);
    return (i64)USER_SURF;
}

static i64 sys_file_read(Proc* p, u64 path_va, u64 buf_va, i64 cap) {
    char path[160];
    if (!user_str(p, path_va, path, sizeof(path)) || !path_ok(path, false)) return -1;
    if (cap < 0 || !user_ok(p, buf_va, (u64)cap, true)) return -1;
    FsNode* n = fs_lookup(path);
    if (!n || n->dir) return -1;
    u32 k = min<u32>(n->size, (u32)cap);
    memcpy((void*)buf_va, n->data, k);
    return k;
}

static i64 sys_file_write(Proc* p, u64 path_va, u64 buf_va, i64 len) {
    char path[160];
    if (!user_str(p, path_va, path, sizeof(path)) || !path_ok(path, true)) return -1;
    if (len < 0 || len > (8 << 20) || !user_ok(p, buf_va, (u64)len)) return -1;
    char dir[160];
    strlcpy(dir, path, sizeof(dir));
    char* sl = strrchr(dir, '/');
    if (sl && sl != dir) { *sl = 0; fs_mkdirs(dir); }
    FsNode* n = fs_create_path(path, false);
    if (!n || n->dir) return -1;
    return fs_write(n, (void*)buf_va, (u32)len) ? 0 : -1;
}

static IntFrame* sys_exit_now(IntFrame* f, Proc* p, int code) {
    p->exit_code = code;
    proc_kill(p, code ? "exited with an error" : "finished");
    return sched_block(f);
}

// ---------------------------------------------------------------- the newer calls: memory, threads, console, files
struct UStat { u32 size, mtime, ctime, flags; };          // flags: 1 folder, 2 read-only

// the next free stretch of pages at or after p->mmap_next
static u64 auto_place(Proc* p, u64 pages) {
    u64 va = p->mmap_next;
    while (va + pages * 4096 <= USER_HI_END) {
        if (!range_used(p, va, pages)) return va;
        va += 0x100000;
    }
    return 0;
}

static i64 sys_mem_map(Proc* p, u64 addr, u64 len, int prot) {
    u64 pages = (len + 4095) / 4096;
    if (!pages || pages > arena_pages) return 0;
    if (!addr) {
        addr = auto_place(p, pages);
        if (!addr) return 0;
        p->mmap_next = addr;
    } else if ((addr & 4095) || addr < USER_HI || addr + pages * 4096 > USER_HI_END || range_used(p, addr, pages)) return 0;
    if (!add_region(p, addr, (u32)pages, nullptr, prot_bits(prot))) return 0;
    if (addr == p->mmap_next) p->mmap_next += (pages + 16) * 4096;      // a gap after each block catches overruns
    flush_tlb(p);
    return (i64)addr;
}

static i64 sys_mem_protect(Proc* p, u64 addr, u64 len, int prot) {
    u64 pages = (len + (addr & 4095) + 4095) / 4096;
    addr &= ~4095ull;
    if (addr < USER_HI || pages > arena_pages || addr + pages * 4096 > USER_HI_END) return -1;
    for (u64 i = 0; i < pages; i++) {
        u64* pte = pte_of(p, addr + i * 4096, false);
        if (!pte || !(*pte & (PTE_P | PTE_HELD))) return -1;
    }
    u64 bits = prot_bits(prot);
    for (u64 i = 0; i < pages; i++) {
        u64* pte = pte_of(p, addr + i * 4096, false);
        *pte = (*pte & PTE_ADDR) | bits;
    }
    flush_tlb(p);
    return 0;
}

static bool user_path(Proc* p, u64 va, char* path, int cap, bool write) {
    return user_str(p, va, path, cap) && path_ok(path, write);
}

static i64 sys_text(Proc* p, u64 argp, u64 strp, u64 len);
static i64 sys_win_create(Proc* p, int w, int h, u64 title_va, int flags, u64 out_va);
static i64 sys_win_ctl(Proc* p, u64 wid, u64 op, u64 a, u64 b, u64 c);

static IntFrame* sys_more(IntFrame* f, Proc* p, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    i64 ret = 0;
    (void)a5;
    switch (f->rax) {
    case SYS_TICKS_US: ret = (i64)ticks_us(); break;
    case SYS_TIME: {                                  // the clock as year, month, day, hour, minute, second, weekday
        if (!user_ok(p, a1, sizeof(DateTime), true)) { ret = -1; break; }
        DateTime d;
        rtc_read(d);
        memcpy((void*)a1, &d, sizeof(d));
        ret = (i64)rtc_stamp();
        break;
    }
    case SYS_PROC_INFO: {                             // 0: the command line, 1: the starting folder, 2: the program's path
        const char* s = a1 == 0 ? p->cmdline : a1 == 1 ? p->cwd : a1 == 2 ? p->path : nullptr;
        if (!s) { ret = -1; break; }
        u64 n = strlen(s);
        if (a3 && user_ok(p, a2, a3, true)) strlcpy((char*)a2, s, (size_t)a3);
        ret = (i64)n;
        break;
    }
    case SYS_WIN_POST: {                              // wake this program's own event loop (from another of its threads)
        UEvent e = {EV_USER, (i32)a1, (i32)a2, (i32)a3, (i32)a4};
        proc_push_event(p, e);
        break;
    }
    case SYS_TEXT: ret = sys_text(p, a1, a2, a3); break;
    case SYS_WIN_CREATE: ret = sys_win_create(p, (int)a1, (int)a2, a3, (int)a4, a5); break;
    case SYS_WIN_CTL: ret = sys_win_ctl(p, a1, a2, a3, a4, a5); break;
    case SYS_WIN_EVENT2: {                            // like SYS_WIN_EVENT, with the window each event is for
        if (!user_ok(p, a1, sizeof(UEvent), true)) { ret = -1; break; }
        if (p->ev_head != p->ev_tail) {
            memcpy((void*)a1, &p->evq[p->ev_head], sizeof(UEvent));
            p->ev_head = (p->ev_head + 1) % EVQ;
            cur->wait_until = 0;
            ret = 1;
        } else if (a2) {
            if (a3) {
                u64 now = ticks();
                if (!cur->wait_until) cur->wait_until = now + a3;
                if (now >= cur->wait_until) { cur->wait_until = 0; ret = 0; break; }
            }
            cur->state = T_WAIT;
            f->rip -= 2;
            f->rax = SYS_WIN_EVENT2;
            return sched_block(f);
        }
        break;
    }
    case SYS_FILE_STAT: {
        char path[200];
        if (!user_path(p, a1, path, sizeof(path), false) || !user_ok(p, a2, sizeof(UStat), true)) { ret = -1; break; }
        FsNode* n = fs_lookup(path);
        if (!n) { ret = -1; break; }
        UStat st = {n->dir ? 0 : n->size, n->mtime, n->ctime, (u32)((n->dir ? 1 : 0) | (path_ok(path, true) ? 0 : 2))};
        memcpy((void*)a2, &st, sizeof(st));
        break;
    }
    case SYS_FILE_DELETE: {                           // a file, or an empty folder
        char path[200];
        if (!user_path(p, a1, path, sizeof(path), true)) { ret = -1; break; }
        FsNode* n = fs_lookup(path);
        if (!n || (n->dir && n->children.len) || !fs_can_modify(n) || n->mount) { ret = -1; break; }
        fs_remove(n);
        break;
    }
    case SYS_MKDIR: {
        char path[200];
        if (!user_path(p, a1, path, sizeof(path), true)) { ret = -1; break; }
        if (fs_lookup(path)) { ret = -2; break; }
        ret = fs_mkdirs(path) ? 0 : -1;
        break;
    }
    case SYS_FILE_RENAME: {
        char from[200], to[200];
        if (!user_path(p, a1, from, sizeof(from), true) || !user_path(p, a2, to, sizeof(to), true)) { ret = -1; break; }
        FsNode* n = fs_lookup(from);
        char* sl = strrchr(to, '/');
        if (!n || !sl || fs_lookup(to) || !fs_can_modify(n)) { ret = -1; break; }
        *sl = 0;
        FsNode* dir = fs_lookup(sl == to ? "/" : to);
        ret = dir && dir->dir && fs_move(n, dir, sl + 1) ? 0 : -1;
        break;
    }
    case SYS_THREAD_AT: {                             // a thread on a stack the program made itself: entry, argument, stack top
        int live = 0;
        for (int i = 0; i < p->threads.len; i++) if (p->threads[i]->state != T_DEAD) live++;
        if (live >= MAX_THREADS_ALL || !user_ok(p, a1, 1) || !user_ok(p, a3 - 64, 64, true)) { ret = -1; break; }
        Thread* t = thread_new(p, a1, (a3 & ~15ull) - 40, a2);
        ret = t->tid;
        break;
    }
    case SYS_SET_GS:                                  // this thread's gs base (0, or an address in its own memory)
        if (a1 && !va_user(a1)) { ret = -1; break; }
        cur->gs_base = a1;
        wrmsr(MSR_GS_BASE, a1);
        break;
    case SYS_MEM_MAP: ret = sys_mem_map(p, a1, a2, (int)a3); break;
    case SYS_MEM_UNMAP: {
        u64 pages = (a2 + 4095) / 4096;
        if ((a1 & 4095) || a1 < USER_HI || pages > arena_pages || a1 + pages * 4096 > USER_HI_END) { ret = -1; break; }
        unmap_range(p, a1, pages);
        break;
    }
    case SYS_MEM_PROTECT: ret = sys_mem_protect(p, a1, a2, (int)a3); break;
    case SYS_FAULT_HANDLER:
        if (a1 && !user_ok(p, a1, 1)) { ret = -1; break; }
        p->fault_handler = a1;
        break;
    case SYS_FAULT_RETURN: {                          // leave the fault handler and carry on with these registers
        if (!cur->in_fault || !user_ok(p, a1, sizeof(UFault))) { ret = -1; break; }
        UFault u;
        memcpy(&u, (void*)a1, sizeof(u));
        if (!va_user(u.rip) || !va_user(u.rsp)) { ret = -1; break; }
        f->rip = u.rip; f->rsp = u.rsp;
        f->rflags = (u.rflags & 0xCD5) | 0x202;      // only the arithmetic flags are the program's to set
        f->rax = u.rax; f->rbx = u.rbx; f->rcx = u.rcx; f->rdx = u.rdx; f->rsi = u.rsi; f->rdi = u.rdi; f->rbp = u.rbp;
        f->r8 = u.r8; f->r9 = u.r9; f->r10 = u.r10; f->r11 = u.r11; f->r12 = u.r12; f->r13 = u.r13; f->r14 = u.r14; f->r15 = u.r15;
        cur->in_fault = false;
        return f;
    }
    case SYS_CON_WRITE: {                             // returns how much was taken (less when the shell is behind)
        if (a2 > (1u << 20) || !user_ok(p, a1, a2)) { ret = -1; break; }
        const u8* s = (const u8*)a1;
        u32 n = (u32)a2;
        if (p->con_out) {
            u32 used = (p->co_tail + CON_OUT_SIZE - p->co_head) % CON_OUT_SIZE;
            n = min<u32>(n, CON_OUT_SIZE - 1 - used);
            for (u32 i = 0; i < n; i++) { p->con_out[p->co_tail] = s[i]; p->co_tail = (p->co_tail + 1) % CON_OUT_SIZE; }
        }
        for (u32 i = 0; i < n; i++) { if (s[i] == '\n') serial_putc('\r'); serial_putc((char)s[i]); }
        ret = n;
        break;
    }
    case SYS_CON_READ: {                              // typed input: what is there (0 = nothing yet), -1 at the end
        if (!user_ok(p, a1, a2, true)) { ret = -1; break; }
        if (!p->con_in) { ret = -1; break; }
        u32 n = 0;
        while (n < a2 && p->ci_head != p->ci_tail) {
            ((u8*)a1)[n++] = p->con_in[p->ci_head];
            p->ci_head = (p->ci_head + 1) % CON_IN_SIZE;
        }
        ret = n ? (i64)n : p->con_eof ? -1 : 0;
        break;
    }
    default: ret = -1; break;
    }
    f->rax = (u64)ret;
    return f;
}

// text with the system's TrueType fonts into a scratch buffer of the program's (white on black, so the
// pixel values are coverage); the Win32 runtime composites it with its own colours and clipping
struct UTextArgs { u64 pixels; i32 w, h; i32 x, baseline; i32 px100; i32 flags; i32 out_w, out_asc, out_desc, out_lh; };
static i64 sys_text(Proc* p, u64 argp, u64 strp, u64 len) {
    UTextArgs t;
    if (!user_ok(p, argp, sizeof(t), true)) return -1;
    memcpy(&t, (void*)argp, sizeof(t));
    if (len > 4096 || !user_ok(p, strp, len)) return -1;
    float px = t.px100 / 100.0f;
    if (px < 1 || px > 400) return -1;
    VFont* f = vfont_get((t.flags & 1) != 0, (t.flags & 2) != 0);
    if (!f) return -1;
    const char* s = (const char*)strp;
    float pen = (float)t.x;
    bool draw = !(t.flags & 4) && t.pixels;
    Canvas c;
    if (draw) {
        if (t.w <= 0 || t.h <= 0 || t.w > 8192 || t.h > 8192 || !user_ok(p, t.pixels, (u64)t.w * t.h * 4, true)) return -1;
        c.init((u32*)t.pixels, t.w, t.h, t.w);
    }
    for (u64 i = 0; i < len;) {
        u32 cp = (u8)s[i];
        int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        if (more) { cp &= 0x3F >> more; for (int k = 1; k <= more && i + k < len; k++) cp = (cp << 6) | ((u8)s[i + k] & 0x3F); }
        i += more + 1;
        int gid = vfont_gid(f, cp);
        if (!gid && cp >= 0x20) gid = vfont_gid(f, '?');
        if (draw) vfont_draw_gid(c, f, gid, px, pen, (float)t.baseline, 0xFFFFFFFF);
        pen += vfont_advance(f, gid, px);
    }
    t.out_w = (int)(pen - t.x + 0.5f);
    t.out_asc = (int)(vfont_ascent(px) + 0.5f);
    t.out_lh = (int)(vfont_line_height(px) + 0.5f);
    t.out_desc = t.out_lh - t.out_asc;
    memcpy((void*)argp, &t, sizeof(t));
    return t.out_w;
}

// ---- more windows per process (the Win32 runtime makes one per top-level HWND)
static i64 sys_win_create(Proc* p, int w, int h, u64 title_va, int flags, u64 out_va) {
    if (w < 1 || h < 1 || w > 4096 || h > 4096 || !user_ok(p, out_va, 16, true)) return -1;
    int id = -1;
    for (int i = 1; i < MAX_UWIN; i++) if (!p->win[i].used) { id = i; break; }
    if (id < 0) return -1;
    UWin& u = p->win[id];
    memset(&u, 0, sizeof(u));
    if (!user_str(p, title_va, u.title, sizeof(u.title))) strlcpy(u.title, p->name, sizeof(u.title));
    // room for the window to grow to the whole screen without remapping
    u32 maxpx = max<u32>((u32)userapp_screen_pixels(), (u32)(w * h));
    maxpx = min<u32>(maxpx, 3840u * 2160u);
    u32 pages = (maxpx * 4 + 4095) / 4096;
    u64 va = auto_place(p, pages), pa = 0;
    if (!va || !add_region(p, va, pages, &pa)) return -1;
    p->mmap_next = va + ((u64)pages + 16) * 4096;
    u.used = true;
    u.surf = (u32*)pa;
    u.va = va;
    u.pages = pages;
    u.sw = w; u.sh = h;
    u.resizable = (flags & 1) != 0;
    u.x = u.y = -1;
    memset(u.surf, 0xFF, (u64)w * h * 4);
    ((u64*)out_va)[0] = va;
    ((u64*)out_va)[1] = (u64)pages * 1024;                    // capacity in pixels
    return id;
}

enum { WC_DESTROY, WC_SHOW, WC_HIDE, WC_TITLE, WC_PRESENT, WC_GET_SIZE, WC_SET_SIZE, WC_MOVE, WC_GET_POS, WC_FOCUS,
       WC_MINIMIZE, WC_MAXIMIZE, WC_IS_FOCUSED, WC_CURSOR_POS, WC_SCREEN_SIZE, WC_SET_CURSOR, WC_IS_VISIBLE };
static i64 sys_win_ctl(Proc* p, u64 wid, u64 op, u64 a, u64 b, u64 c) {
    (void)c;
    if (op == WC_CURSOR_POS) { extern int ui_mouse_x, ui_mouse_y; return (i64)(u32)ui_mouse_x | ((i64)(u32)ui_mouse_y << 32); }
    if (op == WC_SCREEN_SIZE) { extern int SW, SH; return (i64)(SW | (SH << 16)); }
    if (wid >= (u64)MAX_UWIN || !p->win[wid].used) return -1;
    UWin& u = p->win[wid];
    switch (op) {
    case WC_DESTROY:
        if (u.app) { userapp_detach(u.app); userapp_close(u.app); u.app = nullptr; }
        if (wid) { u64 pages = u.pages; u64 va = u.va; unmap_range(p, va, pages); }
        u.used = false;
        return 0;
    case WC_SHOW:
        if (!u.app) u.app = userapp_create(p, (int)wid);
        return 0;
    case WC_HIDE:
        if (u.app) { userapp_detach(u.app); userapp_close(u.app); u.app = nullptr; }
        return 0;
    case WC_TITLE: {
        char t[64];
        if (!user_str(p, a, t, sizeof(t))) return -1;
        strlcpy(u.title, t, sizeof(u.title));
        if (u.app) userapp_set_title(u.app, t);
        return 0;
    }
    case WC_PRESENT: u.dirty = true; return 0;
    case WC_GET_SIZE: return (i64)(u.sw | (u.sh << 16));
    case WC_SET_SIZE: {
        int w = (int)a, h = (int)b;
        if (w < 1 || h < 1 || (u64)w * h > (u64)u.pages * 1024) return -1;
        u.sw = w; u.sh = h;
        if (u.app) userapp_set_geometry(u.app, -1, -1, w, h);
        return 0;
    }
    case WC_MOVE:
        u.x = (int)a; u.y = (int)b;
        if (u.app) userapp_set_geometry(u.app, (int)a, (int)b, -1, -1);
        return 0;
    case WC_GET_POS: {
        int x = u.x, y = u.y, w, h;
        if (u.app) userapp_geometry(u.app, &x, &y, &w, &h);
        return (i64)(u32)x | ((i64)(u32)y << 32);
    }
    case WC_FOCUS: if (u.app) userapp_focus(u.app); return 0;
    case WC_MINIMIZE: if (u.app) userapp_minimize(u.app, 0); return 0;
    case WC_MAXIMIZE: if (u.app) userapp_minimize(u.app, 1); return 0;
    case WC_IS_FOCUSED: return u.app && userapp_focused(u.app) ? 1 : 0;
    case WC_SET_CURSOR: u.cursor = (int)a; return 0;
    case WC_IS_VISIBLE: return u.app ? 1 : 0;
    }
    return -1;
}

IntFrame* sys_dispatch(IntFrame* f) {
    Proc* p = cur->proc;
    if (!p) { f->rax = (u64)-1; return f; }
    u64 a1 = f->rdi, a2 = f->rsi, a3 = f->rdx, a4 = f->r10, a5 = f->r8;
    i64 ret = 0;
    switch (f->rax) {
    case SYS_EXIT: return sys_exit_now(f, p, (int)a1);
    case SYS_YIELD: { f->rax = 0; Thread* nx = pick_user(cur); if (nx && nx != cur) return switch_to(f, nx); return f; }
    case SYS_SLEEP:
        f->rax = 0;
        cur->state = T_SLEEP;
        cur->wake_ms = ticks() + a1;
        return sched_block(f);
    case SYS_TICKS: ret = (i64)ticks(); break;
    case SYS_RAND: ret = rand(); break;
    case SYS_GETPID: ret = p->pid; break;
    case SYS_LOG: { char b[160]; if (user_str(p, a1, b, sizeof(b))) klog("%s: %s\n", p->name, b); break; }
    case SYS_WIN_OPEN: ret = sys_win_open(p, (int)a1, (int)a2, a3, (int)a4); break;
    case SYS_WIN_PRESENT: p->win[0].dirty = true; break;
    case SYS_WIN_TITLE: { char b[48]; if (p->win[0].app && user_str(p, a1, b, sizeof(b))) userapp_set_title(p->win[0].app, b); break; }
    case SYS_WIN_SIZE: ret = p->win[0].surf ? (i64)(p->win[0].sw | (p->win[0].sh << 16)) : 0; break;
    case SYS_WIN_EVENT: {
        if (!user_ok(p, a1, sizeof(UEvent), true)) { ret = -1; break; }
        if (p->ev_head != p->ev_tail) {
            memcpy((void*)a1, &p->evq[p->ev_head], 20);
            p->ev_head = (p->ev_head + 1) % EVQ;
            cur->wait_until = 0;
            ret = 1;
        } else if (a2) {                              // block until something arrives (a3: for at most this many ms)
            if (a3) {
                u64 now = ticks();
                if (!cur->wait_until) cur->wait_until = now + a3;
                if (now >= cur->wait_until) { cur->wait_until = 0; ret = 0; break; }
            }
            f->rax = 0;
            cur->state = T_WAIT;
            // the event is delivered by re-running the call when woken
            f->rip -= 2;                              // back to the int 0x80 instruction
            f->rax = SYS_WIN_EVENT;
            return sched_block(f);
        }
        break;
    }
    case SYS_GFX: ret = sys_gfx(p, a1, a2, a3); break;
    case SYS_FILE_READ: ret = sys_file_read(p, a1, a2, (i64)a3); break;
    case SYS_FILE_WRITE: ret = sys_file_write(p, a1, a2, (i64)a3); break;
    case SYS_FILE_SIZE: {
        char path[160];
        if (!user_str(p, a1, path, sizeof(path)) || !path_ok(path, false)) { ret = -1; break; }
        FsNode* n = fs_lookup(path);
        ret = !n ? -1 : n->dir ? -2 : (i64)n->size;
        break;
    }
    case SYS_DIR_ENTRY: {
        char path[160];
        if (!user_str(p, a1, path, sizeof(path)) || !path_ok(path, false) || !user_ok(p, a3, (u64)a4, true)) { ret = -1; break; }
        FsNode* d = fs_lookup(path);
        if (!d || !d->dir || (i64)a2 >= d->children.len || (i64)a2 < 0) { ret = 0; break; }
        FsNode* c = d->children[(int)a2];
        strlcpy((char*)a3, c->name, (size_t)a4);
        ret = c->dir ? 2 : 1;
        break;
    }
    case SYS_MEM_GROW: {
        u32 bytes = (u32)((a1 + 4095) & ~4095ull);
        if (!bytes || p->heap_len + bytes > MAX_HEAP) { ret = 0; break; }
        u64 va = USER_HEAP + p->heap_len;
        if (!add_region(p, va, bytes / 4096)) { ret = 0; break; }
        p->heap_len += bytes;
        ret = (i64)va;
        break;
    }
    case SYS_THREAD: {
        u64 top;
        if (!user_ok(p, a1, 1) || !alloc_stack(p, &top)) { ret = -1; break; }
        Thread* t = thread_new(p, a1, top, a2);
        ret = t->tid;
        break;
    }
    case SYS_THREAD_EXIT:
        cur->state = T_DEAD;
        return sched_block(f);
    case SYS_CLIP_SET: { char b[400]; if (user_str(p, a1, b, sizeof(b))) clip_set(b); break; }
    case SYS_CLIP_GET: {
        if (!user_ok(p, a1, (u64)a2, true)) { ret = -1; break; }
        strlcpy((char*)a1, clip_has() ? clip_get() : "", (size_t)a2);
        ret = (i64)strlen((char*)a1);
        break;
    }
    case SYS_NOTIFY: {
        char t[40], b[96];
        if (user_str(p, a1, t, sizeof(t)) && user_str(p, a2, b, sizeof(b))) ui_notify(t, b, "module");
        break;
    }
    default: return sys_more(f, p, a1, a2, a3, a4, a5);
    }
    f->rax = (u64)ret;
    return f;
}

void proc_push_event(Proc* p, const UEvent& e) {
    int nt = (p->ev_tail + 1) % EVQ;
    if (nt == p->ev_head) return;                     // queue full: drop
    // a queue that is filling up keeps only the newest mouse position
    if (e.type == EV_MOUSE && (e.d & 0xFF) == 0 && p->ev_head != p->ev_tail) {
        UEvent& last = p->evq[(p->ev_tail + EVQ - 1) % EVQ];
        int used = (p->ev_tail + EVQ - p->ev_head) % EVQ;
        if (last.type == EV_MOUSE && (last.d & 0xFF) == 0 && last.win == e.win && used > 8) { last = e; return; }
    }
    p->evq[p->ev_tail] = e;
    p->ev_tail = nt;
    for (int i = 0; i < p->threads.len; i++)
        if (p->threads[i]->state == T_WAIT) p->threads[i]->state = T_RUN;
}
