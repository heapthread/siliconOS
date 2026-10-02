// silicon - kernel heap: address-ordered block list, next-fit, coalescing
#include "../core.h"

struct Block {
    u64    magic;
    size_t size;        // payload bytes
    Block* prev;
    Block* next;
    u64    free;
    u64    pad;         // keep header 48 bytes -> payload 16-aligned
};

static const u64 MAGIC = 0x51C0B10C51C0B10Cull;
static Block* head = nullptr;
static Block* rover = nullptr;
static size_t used_bytes = 0;
static size_t total_bytes = 0;

void heap_init(u64 start, u64 end) {
    start = (start + 15) & ~15ull;
    head = (Block*)start;
    head->magic = MAGIC;
    head->size = end - start - sizeof(Block);
    head->prev = head->next = nullptr;
    head->free = 1;
    rover = head;
    total_bytes = head->size;
    used_bytes = 0;
}

static void split(Block* b, size_t n) {
    if (b->size < n + sizeof(Block) + 64) return;
    Block* nb = (Block*)((u8*)(b + 1) + n);
    nb->magic = MAGIC;
    nb->size = b->size - n - sizeof(Block);
    nb->free = 1;
    nb->prev = b;
    nb->next = b->next;
    if (b->next) b->next->prev = nb;
    b->next = nb;
    b->size = n;
}

// like kmalloc, but returns nullptr instead of panicking (for big optional buffers: pictures, ...)
void* kmalloc_try(size_t n) {
    if (!n) n = 1;
    n = (n + 15) & ~(size_t)15;
    u64 f = irq_save();
    Block* start = rover ? rover : head;
    Block* b = start;
    do {
        if (b->free && b->size >= n) {
            split(b, n);
            b->free = 0;
            used_bytes += b->size;
            rover = b->next ? b->next : head;
            irq_restore(f);
            return b + 1;
        }
        b = b->next ? b->next : head;
    } while (b != start);
    irq_restore(f);
    return nullptr;
}

void* kmalloc(size_t n) {
    void* p = kmalloc_try(n);
    if (!p) panic("kmalloc: out of memory (%u bytes, %u KiB in use)", (u32)n, (u32)(heap_used() >> 10));
    return p;
}

void* kcalloc_try(size_t n) {
    void* p = kmalloc_try(n);
    if (p) memset(p, 0, n);
    return p;
}

void* kcalloc(size_t n) {
    void* p = kmalloc(n);
    memset(p, 0, n);
    return p;
}

void kfree(void* p) {
    if (!p) return;
    u64 f = irq_save();
    Block* b = (Block*)p - 1;
    if (b->magic != MAGIC || b->free) panic("kfree: bad pointer %p", p);
    b->free = 1;
    used_bytes -= b->size;
    if (b->next && b->next->free) {
        Block* n = b->next;
        if (rover == n) rover = b;
        b->size += sizeof(Block) + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
        n->magic = 0;
    }
    if (b->prev && b->prev->free) {
        Block* pb = b->prev;
        if (rover == b) rover = pb;
        pb->size += sizeof(Block) + b->size;
        pb->next = b->next;
        if (b->next) b->next->prev = pb;
        b->magic = 0;
    }
    irq_restore(f);
}

void* krealloc(void* p, size_t n) {
    if (!p) return kmalloc(n);
    Block* b = (Block*)p - 1;
    if (b->size >= n) return p;
    void* np = kmalloc(n);
    memcpy(np, p, b->size);
    kfree(p);
    return np;
}

size_t heap_used() { return used_bytes; }
size_t heap_total() { return total_bytes; }

void* operator new(size_t n) { return kcalloc(n); }
void* operator new[](size_t n) { return kcalloc(n); }
void operator delete(void* p) noexcept { kfree(p); }
void operator delete[](void* p) noexcept { kfree(p); }
void operator delete(void* p, size_t) noexcept { kfree(p); }
void operator delete[](void* p, size_t) noexcept { kfree(p); }
