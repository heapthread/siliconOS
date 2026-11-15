// win32run - the heap behind HeapAlloc, GlobalAlloc, malloc and operator new.
//
// Small blocks come from size classes: each class keeps a free list and refills it by cutting up a
// slab taken from the virtual memory manager. Big blocks get a region of their own, which goes back
// to the system when they are freed. Every block has a 16-byte header in front of it (so blocks are
// 16-byte aligned, as Windows promises), which is also how a stray pointer is told from a real block.
#include "rt.h"

namespace rt {

struct Hdr { u32 magic; u32 cls; u64 size; };      // size: what the caller asked for
static const u32 MAGIC_USED = 0x57483332, MAGIC_FREE = 0x66726565;
static const u32 CLS_BIG = 0xFFFF;

static const u32 class_size[] = {
    16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384,
    24576, 32768, 49152, 65536, 98304, 131072, 196608, 262144,
};
static const int NCLASS = (int)(sizeof(class_size) / sizeof(class_size[0]));
struct FreeBlk { FreeBlk* next; };
static FreeBlk* free_list[NCLASS];
static u8* slab_cur[2];                            // the slab being cut up: [0] small classes, [1] large ones
static u8* slab_end[2];
static Lock heap_lock;

static int class_of(usize n) {
    int lo = 0, hi = NCLASS - 1;
    while (lo < hi) { int mid = (lo + hi) / 2; if (class_size[mid] >= n) hi = mid; else lo = mid + 1; }
    return lo;
}

static void* alloc_small(int cls) {
    FreeBlk* b = free_list[cls];
    if (b) { free_list[cls] = b->next; return b; }
    usize need = class_size[cls] + sizeof(Hdr);
    int s = class_size[cls] > 4096 ? 1 : 0;
    if (!slab_cur[s] || slab_cur[s] + need > slab_end[s]) {
        // what is left of the old slab is still usable for smaller classes
        while (slab_cur[s] && slab_end[s] - slab_cur[s] >= (i64)(16 + sizeof(Hdr))) {
            usize left = (usize)(slab_end[s] - slab_cur[s]) - sizeof(Hdr);
            int c = class_of(left);
            if (class_size[c] > left) c--;
            if (c < 0) break;
            Hdr* h = (Hdr*)slab_cur[s];
            h->magic = MAGIC_FREE; h->cls = (u32)c; h->size = 0;
            FreeBlk* fb = (FreeBlk*)(h + 1);
            fb->next = free_list[c];
            free_list[c] = fb;
            slab_cur[s] += class_size[c] + sizeof(Hdr);
        }
        usize slab = s ? (2u << 20) : (256u << 10);
        u8* mem = (u8*)vm_alloc(nullptr, slab, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem) return nullptr;
        slab_cur[s] = mem;
        slab_end[s] = mem + slab;
    }
    Hdr* h = (Hdr*)slab_cur[s];
    slab_cur[s] += need;
    h->cls = (u32)cls;
    return h + 1;
}

void* h_alloc(usize n, bool zero) {
    if (n > (1ull << 40)) return nullptr;
    void* p;
    if (n <= class_size[NCLASS - 1]) {
        int cls = class_of(n ? n : 1);
        heap_lock.lock();
        p = alloc_small(cls);
        heap_lock.unlock();
        if (!p) return nullptr;
        Hdr* h = (Hdr*)p - 1;
        h->magic = MAGIC_USED; h->cls = (u32)cls; h->size = n;
        if (zero) memset(p, 0, n);
    } else {
        Hdr* h = (Hdr*)vm_alloc(nullptr, n + sizeof(Hdr), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!h) return nullptr;
        h->magic = MAGIC_USED; h->cls = CLS_BIG; h->size = n;
        p = h + 1;                                 // fresh pages are zero already
    }
    return p;
}

static Hdr* header(void* p) {
    if (!p || ((u64)p & 15) || (u64)p < 0x100000000ull) return nullptr;
    Hdr* h = (Hdr*)p - 1;
    return h->magic == MAGIC_USED ? h : nullptr;
}

void h_free(void* p) {
    if (!p) return;
    Hdr* h = header(p);
    if (!h) { trace("free of something that is not a heap block: %p", p); return; }
    if (h->cls == CLS_BIG) { h->magic = MAGIC_FREE; vm_free(h, 0, MEM_RELEASE); return; }
    h->magic = MAGIC_FREE;
    FreeBlk* b = (FreeBlk*)p;
    heap_lock.lock();
    b->next = free_list[h->cls];
    free_list[h->cls] = b;
    heap_lock.unlock();
}

usize h_size(void* p) {
    Hdr* h = header(p);
    return h ? h->size : (usize)-1;
}

void* h_realloc(void* p, usize n, bool zero) {
    if (!p) return h_alloc(n, zero);
    Hdr* h = header(p);
    if (!h) return nullptr;
    usize old = h->size;
    usize room = h->cls == CLS_BIG ? ((old + sizeof(Hdr) + 4095) & ~4095ull) - sizeof(Hdr) : class_size[h->cls];
    if (n <= room && (h->cls == CLS_BIG ? n > room / 2 : true)) {
        if (zero && n > old) memset((u8*)p + old, 0, n - old);
        h->size = n;
        return p;
    }
    void* q = h_alloc(n, false);
    if (!q) return nullptr;
    memcpy(q, p, old < n ? old : n);
    if (zero && n > old) memset((u8*)q + old, 0, n - old);
    h_free(p);
    return q;
}

// aligned blocks keep the real block's address just in front of what they hand out
void* h_aligned(usize n, usize align) {
    if (align < 16) align = 16;
    if (align & (align - 1)) return nullptr;
    void* raw = h_alloc(n + align + 8);
    if (!raw) return nullptr;
    u64 a = ((u64)raw + 8 + align - 1) & ~(align - 1);
    ((void**)a)[-1] = raw;
    return (void*)a;
}
void h_aligned_free(void* p) { if (p) h_free(((void**)p)[-1]); }

}  // namespace rt
