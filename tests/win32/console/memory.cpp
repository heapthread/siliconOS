#include "common.h"
#include <windows.h>
#include <cstdlib>
#include <cstring>

void test_memory() {
    std::printf("[memory]\n");
    // VirtualAlloc: reserve, commit part, use, protect, free
    char* r = (char*)VirtualAlloc(nullptr, 1 << 20, MEM_RESERVE, PAGE_NOACCESS);
    check(r != nullptr && ((ULONG_PTR)r & 0xFFFF) == 0, "VirtualAlloc reserve (64 KiB aligned)");
    char* c = (char*)VirtualAlloc(r + 8192, 8192, MEM_COMMIT, PAGE_READWRITE);
    check(c == r + 8192, "VirtualAlloc commit inside the reservation");
    std::memset(c, 0x5A, 8192);
    check(c[0] == 0x5A && c[8191] == 0x5A, "committed pages are writable");
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(r, &mbi, sizeof(mbi));
    check(mbi.State == MEM_RESERVE && mbi.AllocationBase == r, "VirtualQuery: reserved part");
    VirtualQuery(c, &mbi, sizeof(mbi));
    check(mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize == 8192, "VirtualQuery: committed part");
    DWORD old = 0;
    check(VirtualProtect(c, 4096, PAGE_READONLY, &old) && old == PAGE_READWRITE, "VirtualProtect -> PAGE_READONLY");
    VirtualQuery(c, &mbi, sizeof(mbi));
    check(mbi.Protect == PAGE_READONLY, "VirtualQuery sees the new protection");
    check(VirtualFree(c, 4096, MEM_DECOMMIT), "VirtualFree MEM_DECOMMIT");
    check(VirtualFree(r, 0, MEM_RELEASE), "VirtualFree MEM_RELEASE");
    // executable memory
    unsigned char* code = (unsigned char*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned char ret42[] = {0xB8, 42, 0, 0, 0, 0xC3};            // mov eax, 42; ret
    std::memcpy(code, ret42, sizeof(ret42));
    check(((int (*)())code)() == 42, "code generated at run time runs");
    VirtualFree(code, 0, MEM_RELEASE);

    // heaps
    HANDLE ph = GetProcessHeap();
    int* a = (int*)HeapAlloc(ph, HEAP_ZERO_MEMORY, 1000 * sizeof(int));
    bool zero = true;
    for (int i = 0; i < 1000; i++) zero &= a[i] == 0;
    check(a && zero, "HeapAlloc HEAP_ZERO_MEMORY");
    check(HeapSize(ph, 0, a) == 1000 * sizeof(int), "HeapSize");
    for (int i = 0; i < 1000; i++) a[i] = i;
    a = (int*)HeapReAlloc(ph, 0, a, 100000 * sizeof(int));
    bool kept = a != nullptr;
    for (int i = 0; kept && i < 1000; i++) kept &= a[i] == i;
    check(kept, "HeapReAlloc keeps the contents");
    check(HeapFree(ph, 0, a), "HeapFree");
    HANDLE h = HeapCreate(0, 0, 0);
    void* blocks[500];
    for (int i = 0; i < 500; i++) { blocks[i] = HeapAlloc(h, 0, (i * 37) % 3000 + 1); std::memset(blocks[i], i & 0xFF, (i * 37) % 3000 + 1); }
    bool intact = true;
    for (int i = 0; i < 500; i++) intact &= ((unsigned char*)blocks[i])[(i * 37) % 3000] == (i & 0xFF);
    for (int i = 0; i < 500; i += 2) HeapFree(h, 0, blocks[i]);
    for (int i = 0; i < 500; i += 2) blocks[i] = HeapAlloc(h, 0, 64);
    for (int i = 0; i < 500; i++) HeapFree(h, 0, blocks[i]);
    check(intact && HeapDestroy(h), "HeapCreate + 500 blocks of mixed sizes + HeapDestroy");
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 256);
    char* gp = (char*)GlobalLock(g);
    check(gp && gp[255] == 0, "GlobalAlloc/GlobalLock");
    GlobalUnlock(g);
    GlobalFree(g);
    HLOCAL l = LocalAlloc(LPTR, 64);
    check(l && LocalFree(l) == nullptr, "LocalAlloc/LocalFree");
    // the C runtime's heap
    void* m = std::malloc(123);
    m = std::realloc(m, 4567);
    check(m && _msize(m) == 4567, "malloc/realloc/_msize");
    std::free(m);
    void* al = _aligned_malloc(1000, 256);
    check(al && ((ULONG_PTR)al & 255) == 0, "_aligned_malloc(.., 256)");
    _aligned_free(al);
    char* big = (char*)std::calloc(16, 1 << 20);
    check(big && big[16 * 1048576 - 1] == 0, "calloc 16 MiB");
    std::free(big);
}
