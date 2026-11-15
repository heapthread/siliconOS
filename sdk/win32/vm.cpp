// win32run - virtual memory with VirtualAlloc's rules.
//
// silicon gives a program the addresses from 4 GiB to 512 GiB to map as it likes (mem_map, mem_unmap,
// mem_protect, a page at a time). Windows programs expect more structure: regions that are reserved
// first and committed piece by piece, 64 KiB allocation granularity, a protection per page that can be
// read back (VirtualQuery) and "release the whole region I once allocated". This file keeps that book:
// a sorted list of regions, each with one byte of state per page.
#include "rt.h"

namespace rt {

static const u64 VM_LO = 0x100000000ull;           // what silicon lets a program map
static const u64 VM_HI = 0x7000000000ull;          // silicon keeps the top 64 GiB for what it places itself (window buffers)
static const u64 VM_DYNAMIC = 0x200000000ull;      // where allocations without an address start
static const u64 GRAN = 0x10000;                   // allocations start on 64 KiB boundaries
static const u64 PAGE = 4096;

struct Region {
    u64 base, size;                                // size in bytes, whole pages
    u32 kind;                                      // MEM_PRIVATE, MEM_IMAGE, MEM_MAPPED
    u32 alloc_protect;
    u8* pages;                                     // per page: 0 = reserved only, else the protection (as a code)
};
static Region* regions = nullptr;
static int nregions = 0, cap_regions = 0;
static Lock vm_lock;
static u64 committed_pages = 0;
static u64 next_hint = VM_DYNAMIC;

// protection <-> the byte kept per page
static u8 prot_code(u32 p) {
    switch (p & 0xFF) {
    case PAGE_NOACCESS: return 1;
    case PAGE_READONLY: return 2;
    case PAGE_READWRITE: return 3;
    case PAGE_WRITECOPY: return 4;
    case PAGE_EXECUTE: return 5;
    case PAGE_EXECUTE_READ: return 6;
    case PAGE_EXECUTE_READWRITE: return 7;
    case PAGE_EXECUTE_WRITECOPY: return 8;
    }
    return 0;
}
static u32 code_prot(u8 c) {
    static const u32 t[9] = {0, PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE, PAGE_WRITECOPY, PAGE_EXECUTE, PAGE_EXECUTE_READ,
                             PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_WRITECOPY};
    return c < 9 ? t[c] : 0;
}
static int code_kernel(u8 c) {                     // what silicon's mem_protect takes
    static const int t[9] = {0, 0, si::PROT_R, si::PROT_R | si::PROT_W, si::PROT_R | si::PROT_W, si::PROT_R | si::PROT_X,
                             si::PROT_R | si::PROT_X, si::PROT_R | si::PROT_W | si::PROT_X, si::PROT_R | si::PROT_W | si::PROT_X};
    return c < 9 ? t[c] : 0;
}

static int find_region(u64 addr) {                 // the region holding addr, or -1
    int lo = 0, hi = nregions - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (addr < regions[mid].base) hi = mid - 1;
        else if (addr >= regions[mid].base + regions[mid].size) lo = mid + 1;
        else return mid;
    }
    return -1;
}

static bool range_free(u64 base, u64 size) {
    for (int i = 0; i < nregions; i++) {
        if (regions[i].base >= base + size) break;
        if (regions[i].base + regions[i].size > base) return false;
    }
    return true;
}

static u64 find_space(u64 size) {
    for (int pass = 0; pass < 2; pass++) {
        u64 at = pass == 0 ? next_hint : VM_DYNAMIC;
        for (int i = 0; i < nregions; i++) {
            Region& r = regions[i];
            if (r.base + r.size <= at) continue;
            if (r.base >= at + size) break;
            at = (r.base + r.size + GRAN - 1) & ~(GRAN - 1);
        }
        if (at + size <= VM_HI) { next_hint = (at + size + GRAN - 1) & ~(GRAN - 1); return at; }
    }
    return 0;
}

static Region* insert_region(u64 base, u64 size, u32 kind, u32 protect) {
    if (nregions == cap_regions) {
        int nc = cap_regions ? cap_regions * 2 : 256;
        Region* nr = (Region*)ialloc(sizeof(Region) * (usize)nc);
        if (!nr) return nullptr;
        if (regions) { memcpy(nr, regions, sizeof(Region) * (usize)nregions); ifree(regions); }
        regions = nr;
        cap_regions = nc;
    }
    u8* pages = (u8*)izalloc(size / PAGE);
    if (!pages) return nullptr;
    int at = 0;
    while (at < nregions && regions[at].base < base) at++;
    memmove(regions + at + 1, regions + at, sizeof(Region) * (usize)(nregions - at));
    nregions++;
    regions[at] = {base, size, kind, protect, pages};
    return &regions[at];
}

// commit [first, first + count) pages of r with protection code c: map what isn't there, re-protect what is
static bool commit_pages(Region* r, u64 first, u64 count, u8 c) {
    u64 i = first, end = first + count;
    while (i < end) {
        bool have = r->pages[i] != 0;
        u64 j = i;
        while (j < end && (r->pages[j] != 0) == have) j++;
        void* va = (void*)(r->base + i * PAGE);
        if (have) si::mem_protect(va, (j - i) * PAGE, code_kernel(c));
        else {
            if (!si::mem_map(va, (j - i) * PAGE, code_kernel(c))) {
                // out of memory part-way: give back what this call mapped
                for (u64 k = first; k < i; k++) {}
                return false;
            }
            committed_pages += j - i;
        }
        for (u64 k = i; k < j; k++) r->pages[k] = c;
        i = j;
    }
    return true;
}

static void decommit_pages(Region* r, u64 first, u64 count) {
    u64 i = first, end = first + count;
    while (i < end) {
        if (!r->pages[i]) { i++; continue; }
        u64 j = i;
        while (j < end && r->pages[j]) j++;
        si::mem_unmap((void*)(r->base + i * PAGE), (j - i) * PAGE);
        committed_pages -= j - i;
        for (u64 k = i; k < j; k++) r->pages[k] = 0;
        i = j;
    }
}

void* vm_alloc(void* addr_, usize size, u32 type, u32 protect, u32 kind) {
    u64 addr = (u64)addr_;
    u8 c = prot_code(protect);
    if (!size || !c || !(type & (MEM_COMMIT | MEM_RESERVE))) { set_error(ERROR_INVALID_PARAMETER); return nullptr; }
    Guard g(vm_lock);
    int ri = addr ? find_region(addr) : -1;
    if (ri >= 0) {
        // inside something that exists already: committing more of it (reserving it again is harmless)
        Region* r = &regions[ri];
        u64 start = addr & ~(PAGE - 1), end = (addr + size + PAGE - 1) & ~(PAGE - 1);
        if (end > r->base + r->size) { set_error(ERROR_INVALID_ADDRESS); return nullptr; }
        if (type & MEM_COMMIT) {
            if (!commit_pages(r, (start - r->base) / PAGE, (end - start) / PAGE, c)) { set_error(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
        }
        return (void*)start;
    }
    u64 base, bytes;
    if (addr) {
        base = addr & ~(GRAN - 1);
        bytes = ((addr + size + PAGE - 1) & ~(PAGE - 1)) - base;
        if (base < VM_LO || base + bytes > VM_HI || !range_free(base, bytes)) { set_error(ERROR_INVALID_ADDRESS); return nullptr; }
    } else {
        bytes = (size + PAGE - 1) & ~(PAGE - 1);
        if (bytes > VM_HI - VM_DYNAMIC) { set_error(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
        base = find_space(bytes);
        if (!base) { set_error(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    }
    Region* r = insert_region(base, bytes, kind, protect);
    if (!r) { set_error(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    if (type & MEM_COMMIT) {
        u64 start = addr ? (addr & ~(PAGE - 1)) : base;
        if (!commit_pages(r, (start - base) / PAGE, (base + bytes - start) / PAGE, c)) {
            decommit_pages(r, 0, bytes / PAGE);
            ifree(r->pages);
            int at = (int)(r - regions);
            memmove(regions + at, regions + at + 1, sizeof(Region) * (usize)(nregions - at - 1));
            nregions--;
            set_error(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        return (void*)start;
    }
    return (void*)(addr ? (addr & ~(PAGE - 1)) : base);
}

bool vm_free(void* addr_, usize size, u32 type) {
    u64 addr = (u64)addr_;
    Guard g(vm_lock);
    int ri = find_region(addr);
    if (ri < 0) { set_error(ERROR_INVALID_ADDRESS); return false; }
    Region* r = &regions[ri];
    if (type & MEM_RELEASE) {
        if (addr != r->base || size) { set_error(ERROR_INVALID_PARAMETER); return false; }
        decommit_pages(r, 0, r->size / PAGE);
        ifree(r->pages);
        if (r->base < next_hint && r->base >= VM_DYNAMIC) next_hint = r->base;
        memmove(regions + ri, regions + ri + 1, sizeof(Region) * (usize)(nregions - ri - 1));
        nregions--;
        return true;
    }
    if (type & MEM_DECOMMIT) {
        u64 start = addr & ~(PAGE - 1);
        u64 end = size ? (addr + size + PAGE - 1) & ~(PAGE - 1) : r->base + r->size;
        if (!size && addr != r->base) { set_error(ERROR_INVALID_PARAMETER); return false; }
        if (end > r->base + r->size) { set_error(ERROR_INVALID_ADDRESS); return false; }
        decommit_pages(r, (start - r->base) / PAGE, (end - start) / PAGE);
        return true;
    }
    set_error(ERROR_INVALID_PARAMETER);
    return false;
}

bool vm_protect(void* addr_, usize size, u32 protect, u32* old) {
    u64 addr = (u64)addr_;
    u8 c = prot_code(protect);
    if (!c || !size) { set_error(ERROR_INVALID_PARAMETER); return false; }
    Guard g(vm_lock);
    int ri = find_region(addr);
    if (ri < 0) { set_error(ERROR_INVALID_ADDRESS); return false; }
    Region* r = &regions[ri];
    u64 start = addr & ~(PAGE - 1), end = (addr + size + PAGE - 1) & ~(PAGE - 1);
    if (end > r->base + r->size) { set_error(ERROR_INVALID_ADDRESS); return false; }
    u64 first = (start - r->base) / PAGE, count = (end - start) / PAGE;
    for (u64 i = first; i < first + count; i++) if (!r->pages[i]) { set_error(ERROR_INVALID_ADDRESS); return false; }
    if (old) *old = code_prot(r->pages[first]);
    si::mem_protect((void*)start, count * PAGE, code_kernel(c));
    for (u64 i = first; i < first + count; i++) r->pages[i] = c;
    return true;
}

usize vm_query(const void* addr_, MEMORY_BASIC_INFORMATION* mbi) {
    u64 addr = (u64)addr_ & ~(PAGE - 1);
    memset(mbi, 0, sizeof(*mbi));
    Guard g(vm_lock);
    int ri = find_region(addr);
    if (ri < 0) {
        // free space up to the next region. Below 4 GiB is the runtime's own; reported as inaccessible
        u64 next = VM_HI;
        for (int i = 0; i < nregions; i++) if (regions[i].base > addr) { next = regions[i].base; break; }
        if (addr >= VM_HI) return 0;
        mbi->BaseAddress = (void*)addr;
        mbi->RegionSize = next - addr;
        mbi->State = MEM_FREE;
        mbi->Protect = PAGE_NOACCESS;
        return sizeof(*mbi);
    }
    Region* r = &regions[ri];
    u64 pi = (addr - r->base) / PAGE, n = r->size / PAGE;
    u8 c = r->pages[pi];
    u64 first = pi, last = pi;
    while (first > 0 && r->pages[first - 1] == c) first--;
    while (last + 1 < n && r->pages[last + 1] == c) last++;
    (void)first;
    mbi->BaseAddress = (void*)addr;
    mbi->AllocationBase = (void*)r->base;
    mbi->AllocationProtect = r->alloc_protect;
    mbi->RegionSize = (last + 1 - pi) * PAGE;
    mbi->State = c ? MEM_COMMIT : MEM_RESERVE;
    mbi->Protect = code_prot(c);
    mbi->Type = r->kind;
    return sizeof(*mbi);
}

static bool check_access(const void* addr_, usize size, bool write) {
    u64 addr = (u64)addr_;
    if (!size) return true;
    if (addr >= 0x40000000ull && addr + size <= 0x80000000ull) return true;      // the runtime's own image, heap and stacks
    Guard g(vm_lock);
    u64 a = addr & ~(PAGE - 1), end = addr + size;
    while (a < end) {
        int ri = find_region(a);
        if (ri < 0) return false;
        Region* r = &regions[ri];
        u8 c = r->pages[(a - r->base) / PAGE];
        if (c < 2) return false;
        if (write && !(c == 3 || c == 4 || c == 7 || c == 8)) return false;
        a += PAGE;
    }
    return true;
}
bool vm_readable(const void* addr, usize size) { return check_access(addr, size, false); }
bool vm_writable(const void* addr, usize size) { return check_access(addr, size, true); }
u64 vm_committed() { return committed_pages * PAGE; }

}  // namespace rt
