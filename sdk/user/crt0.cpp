// silicon SDK startup for user programs: the entry point, static constructors, a small heap and the
// functions the compiler wants even in freestanding mode.
#include "silicon_user.h"

extern "C" {
typedef void (*CtorFn)(void);
extern CtorFn __ctors_start[];
extern CtorFn __ctors_end[];

int uapp_entry() {
    for (CtorFn* f = __ctors_start; f < __ctors_end; f++) if (*f) (*f)();
    return app_main();
}

void* memcpy(void* d, const void* s, unsigned long long n) {
    unsigned char* a = (unsigned char*)d;
    const unsigned char* b = (const unsigned char*)s;
    while (n--) *a++ = *b++;
    return d;
}
void* memmove(void* d, const void* s, unsigned long long n) {
    unsigned char* a = (unsigned char*)d;
    const unsigned char* b = (const unsigned char*)s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void* memset(void* d, int c, unsigned long long n) {
    unsigned char* a = (unsigned char*)d;
    while (n--) *a++ = (unsigned char)c;
    return d;
}
int memcmp(const void* x, const void* y, unsigned long long n) {
    const unsigned char* a = (const unsigned char*)x;
    const unsigned char* b = (const unsigned char*)y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
unsigned long long strlen(const char* s) { unsigned long long n = 0; while (s[n]) n++; return n; }
}

// the entry point: align the stack, run the program, exit with its result
__asm__(".text\n"
        ".globl _start\n"
        "_start:\n"
        "  and $-16, %rsp\n"
        "  sub $32, %rsp\n"
        "  call uapp_entry\n"
        "  mov %eax, %edi\n"
        "  xor %eax, %eax\n"
        "  int $0x80\n"
        "1: jmp 1b\n");

// ---------------------------------------------------------------- a small first-fit heap
namespace si {
struct Block { unsigned long long size; Block* next; bool free; };
static Block* head = nullptr;
static volatile int heap_lock = 0;

static void lock() { while (__sync_lock_test_and_set(&heap_lock, 1)) yield(); }
static void unlock() { __sync_lock_release(&heap_lock); }

void* malloc(unsigned long long n) {
    n = (n + 15) & ~15ull;
    lock();
    for (Block* b = head; b; b = b->next)
        if (b->free && b->size >= n) {
            if (b->size >= n + sizeof(Block) + 32) {
                Block* rest = (Block*)((char*)(b + 1) + n);
                rest->size = b->size - n - sizeof(Block);
                rest->free = true;
                rest->next = b->next;
                b->next = rest;
                b->size = n;
            }
            b->free = false;
            unlock();
            return b + 1;
        }
    unsigned long long chunk = (n + sizeof(Block) + 0xFFFF) & ~0xFFFFull;
    char* mem = (char*)mem_grow(chunk);
    if (!mem) { unlock(); return nullptr; }
    Block* b = (Block*)mem;
    b->size = chunk - sizeof(Block);
    b->free = true;
    b->next = nullptr;
    Block** tail = &head;
    while (*tail) tail = &(*tail)->next;
    *tail = b;
    unlock();
    return malloc(n);
}

void free(void* p) {
    if (!p) return;
    lock();
    Block* b = (Block*)p - 1;
    b->free = true;
    // merge with the next block when it directly follows
    while (b->next && b->next->free && (char*)(b + 1) + b->size == (char*)b->next) {
        b->size += sizeof(Block) + b->next->size;
        b->next = b->next->next;
    }
    unlock();
}

struct Start { void (*fn)(void*); void* arg; };
static void thread_main(void* p) {
    Start s = *(Start*)p;
    free(p);
    s.fn(s.arg);
    sys(S_THREAD_EXIT);
}
int thread(void (*fn)(void*), void* arg) {
    Start* s = (Start*)malloc(sizeof(Start));
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    return (int)sys(S_THREAD, (word)&thread_main, (word)s);
}
}

void* operator new(unsigned long long n) { return si::malloc(n); }
void* operator new[](unsigned long long n) { return si::malloc(n); }
void operator delete(void* p) noexcept { si::free(p); }
void operator delete[](void* p) noexcept { si::free(p); }
void operator delete(void* p, unsigned long long) noexcept { si::free(p); }
void operator delete[](void* p, unsigned long long) noexcept { si::free(p); }
