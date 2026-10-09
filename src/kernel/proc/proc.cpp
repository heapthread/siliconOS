// silicon - processes, threads, scheduler and system calls
#include "proc.h"
#include "../fs/fs.h"
#include "../ui/wm.h"
#include "../text.h"

void panic_frame(const char* what, IntFrame* f);

// ---------------------------------------------------------------- the user address space layout
// Everything a process owns lives between 1 GiB and 2 GiB; the kernel identity-maps the first
// 4 GiB without the user bit, so none of it is reachable from ring 3.
static const u64 USER_BASE   = 0x40000000ull;     // image (code, data, bss)
static const u64 USER_HEAP   = 0x48000000ull;
static const u64 USER_SURF   = 0x58000000ull;     // window pixels
static const u64 USER_STACK  = 0x60000000ull;     // thread stacks, 256 KiB apart
static const u64 USER_END    = 0x80000000ull;
static const u32 STACK_PAGES = 32;                // 128 KiB per thread
static const u32 KSTACK_SIZE = 32 * 1024;
static const int MAX_THREADS = 8;
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

void proc_init() {
    kernel_cr3 = read_cr3();
    main_thread.tid = 0;
    main_thread.state = T_RUN;
    cur = &main_thread;
}

// ---------------------------------------------------------------- address spaces
static bool map_page(Proc* p, u64 va, u64 pa) {
    u64 pdi = (va >> 21) & 511, pti = (va >> 12) & 511;
    if (!(p->pd[pdi] & 1)) {
        u64 pg = pmm_alloc(1);
        if (!pg) return false;
        p->pt_pages.push(pg);
        p->pd[pdi] = pg | 7;
    }
    u64* pt = (u64*)(p->pd[pdi] & ~0xFFFull);
    pt[pti] = pa | 7;
    return true;
}

static u64 user_phys(Proc* p, u64 va) {
    if (va < USER_BASE || va >= USER_END) return 0;
    u64 pdi = (va >> 21) & 511, pti = (va >> 12) & 511;
    if (!(p->pd[pdi] & 1)) return 0;
    u64* pt = (u64*)(p->pd[pdi] & ~0xFFFull);
    return (pt[pti] & 1) ? (pt[pti] & ~0xFFFull) | (va & 0xFFF) : 0;
}

// is [va, va+len) all mapped for this process?
static bool user_ok(Proc* p, u64 va, u64 len) {
    if (!len) return true;
    if (va < USER_BASE || va >= USER_END || len > USER_END - va) return false;
    for (u64 a = va & ~4095ull; a < va + len; a += 4096) if (!user_phys(p, a)) return false;
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

static bool add_region(Proc* p, u64 va, u32 pages, u64* pa_out = nullptr) {
    u64 pa = pmm_alloc(pages);
    if (!pa) return false;
    for (u32 i = 0; i < pages; i++)
        if (!map_page(p, va + (u64)i * 4096, pa + (u64)i * 4096)) return false;
    Proc::Region r = {va, pa, pages};
    p->regions.push(r);
    p->mem_pages += pages;
    if (pa_out) *pa_out = pa;
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

Proc* proc_spawn(const char* path, char* err, int errlen) {
    pmm_init();
    if (!kernel_cr3) proc_init();
    FsNode* n = fs_lookup(path);
    if (!n || n->dir) { snprintf(err, errlen, "no such program"); return nullptr; }
    if (n->size < sizeof(UappHeader) || memcmp(n->data, "UAPP", 4)) { snprintf(err, errlen, "not a silicon program (.uapp)"); return nullptr; }
    UappHeader h;
    memcpy(&h, n->data, sizeof(h));
    if (h.version != 1 || h.image > MAX_IMAGE || h.bss > MAX_IMAGE || h.entry >= h.image ||
        sizeof(h) + h.image > n->size) { snprintf(err, errlen, "damaged program file"); return nullptr; }
    Proc* p = new Proc();
    p->pid = next_pid++;
    const char* base = strrchr(path, '/');
    strlcpy(p->name, base ? base + 1 : path, sizeof(p->name));
    char* dot = strrchr(p->name, '.');
    if (dot) *dot = 0;
    strlcpy(p->path, path, sizeof(p->path));
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
    memcpy((void*)pa, n->data + sizeof(h), h.image);
    thread_new(p, USER_BASE + h.entry, stack_top, 0);
    procs.push(p);
    klog("proc: started %s (pid %d, %u KiB)\n", p->name, p->pid, (u32)(p->mem_pages * 4));
    return p;
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
        if (!p->dead) continue;
        if (p->app) { userapp_detach(p->app); p->app = nullptr; }
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
        procs.remove(i);
        delete p;
    }
}

IntFrame* proc_fault(IntFrame* f, const char* what) {
    Proc* p = cur->proc;
    if (!p) panic_frame(what, f);
    char why[64];
    snprintf(why, sizeof(why), "%s at %lx", what, f->rip);
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
};
enum { GFX_FILL = 1, GFX_RRECT = 2, GFX_TEXT = 3, GFX_TEXT_W = 4, GFX_ICON = 5, GFX_LINE = 6, GFX_DISC = 7, GFX_CLEAR = 8, GFX_RECT = 9 };

// what a program may read and write: its own corner of the disk, not the system's secrets
static bool has_dotdot(const char* s) { for (; s[0] && s[1]; s++) if (s[0] == '.' && s[1] == '.') return true; return false; }
static bool path_ok(const char* path, bool write) {
    if (path[0] != '/' || has_dotdot(path)) return false;
    if (str_starts(path, "/home/.config/mail") || str_starts(path, "/system")) return false;
    if (write) return str_starts(path, "/home/") && !str_starts(path, "/home/.config/");
    return true;
}

static bool surf_canvas(Proc* p, Canvas& c) {
    if (!p->surf || p->sw <= 0 || p->sh <= 0) return false;
    c.init(p->surf, p->sw, p->sh, p->sw);
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
    p->dirty = true;
    return 0;
}

static i64 sys_win_open(Proc* p, int w, int h, u64 title_va, int flags) {
    if (p->app || w < 80 || h < 60 || w > 4096 || h > 4096) return 0;
    char title[48];
    if (!user_str(p, title_va, title, sizeof(title))) strlcpy(title, p->name, sizeof(title));
    u32 maxpx = max<u32>((u32)userapp_screen_pixels(), (u32)(w * h));
    maxpx = min<u32>(maxpx, 3840u * 2160u);
    u32 pages = (maxpx * 4 + 4095) / 4096;
    u64 pa = 0;
    if (!add_region(p, USER_SURF, pages, &pa)) return 0;
    p->surf = (u32*)pa;
    p->surf_pages = pages;
    p->sw = w; p->sh = h;
    memset(p->surf, 0xFF, (u64)w * h * 4);
    p->app = userapp_create(p, w, h, title, flags);
    return (i64)USER_SURF;
}

static i64 sys_file_read(Proc* p, u64 path_va, u64 buf_va, i64 cap) {
    char path[160];
    if (!user_str(p, path_va, path, sizeof(path)) || !path_ok(path, false)) return -1;
    if (cap < 0 || !user_ok(p, buf_va, (u64)cap)) return -1;
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
    case SYS_WIN_PRESENT: p->dirty = true; break;
    case SYS_WIN_TITLE: { char b[48]; if (p->app && user_str(p, a1, b, sizeof(b))) userapp_set_title(p->app, b); break; }
    case SYS_WIN_SIZE: ret = p->surf ? (i64)(p->sw | (p->sh << 16)) : 0; break;
    case SYS_WIN_EVENT: {
        if (!user_ok(p, a1, sizeof(UEvent))) { ret = -1; break; }
        if (p->ev_head != p->ev_tail) {
            memcpy((void*)a1, &p->evq[p->ev_head], sizeof(UEvent));
            p->ev_head = (p->ev_head + 1) % 64;
            ret = 1;
        } else if (a2) {                              // block until something arrives
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
        if (!user_str(p, a1, path, sizeof(path)) || !path_ok(path, false) || !user_ok(p, a3, (u64)a4)) { ret = -1; break; }
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
        if (!user_ok(p, a1, (u64)a2)) { ret = -1; break; }
        strlcpy((char*)a1, clip_has() ? clip_get() : "", (size_t)a2);
        ret = (i64)strlen((char*)a1);
        break;
    }
    case SYS_NOTIFY: {
        char t[40], b[96];
        if (user_str(p, a1, t, sizeof(t)) && user_str(p, a2, b, sizeof(b))) ui_notify(t, b, "module");
        break;
    }
    default: ret = -1; break;
    }
    f->rax = (u64)ret;
    return f;
}

void proc_push_event(Proc* p, const UEvent& e) {
    int nt = (p->ev_tail + 1) % 64;
    if (nt == p->ev_head) return;                     // queue full: drop
    p->evq[p->ev_tail] = e;
    p->ev_tail = nt;
    for (int i = 0; i < p->threads.len; i++)
        if (p->threads[i]->state == T_WAIT) p->threads[i]->state = T_RUN;
}
