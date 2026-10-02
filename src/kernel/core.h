// silicon - core kernel definitions shared by every module
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

#define SILICON_VERSION "0.1"
#define ARRAY_LEN(a) (int)(sizeof(a) / sizeof((a)[0]))

// ---------------------------------------------------------------- port io
static inline void outb(u16 p, u8 v)  { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline u8  inb(u16 p) { u8 v;  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline u16 inw(u16 p) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline u32 inl(u16 p) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void io_wait() { outb(0x80, 0); }
static inline void cli() { __asm__ volatile("cli"); }
static inline void sti() { __asm__ volatile("sti"); }
static inline void hlt() { __asm__ volatile("hlt"); }
static inline u64 irq_save() { u64 f; __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory"); return f; }
static inline void irq_restore(u64 f) { if (f & 0x200) sti(); }

// ---------------------------------------------------------------- lib
extern "C" {
void* memcpy(void* d, const void* s, size_t n);
void* memmove(void* d, const void* s, size_t n);
void* memset(void* d, int c, size_t n);
int   memcmp(const void* a, const void* b, size_t n);
}
void   memset32(u32* d, u32 v, size_t count);
size_t strlen(const char* s);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
char*  strcpy(char* d, const char* s);
void   strlcpy(char* d, const char* s, size_t n);
void   strlcat(char* d, const char* s, size_t n);
char*  strchr(const char* s, int c);
char*  strrchr(const char* s, int c);
bool   str_starts(const char* s, const char* prefix);
bool   str_ieq_prefix(const char* s, const char* prefix);   // case-insensitive prefix
bool   str_icontains(const char* hay, const char* needle);
char*  strdup(const char* s);
int    atoi(const char* s);
char   to_lower(char c);
char   to_upper(char c);
int    vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);
int    snprintf(char* buf, size_t n, const char* fmt, ...);
template <typename T> static inline T min(T a, T b) { return a < b ? a : b; }
template <typename T> static inline T max(T a, T b) { return a > b ? a : b; }
template <typename T> static inline T clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
template <typename T> static inline T iabs(T v) { return v < 0 ? -v : v; }
template <typename T> static inline void swap(T& a, T& b) { T t = a; a = b; b = t; }

// pseudo random
u32  rand();
void srand(u32 seed);

// ---------------------------------------------------------------- heap
void   heap_init(u64 start, u64 end);
void*  kmalloc(size_t n);
void*  kcalloc(size_t n);
void*  kmalloc_try(size_t n);          // nullptr when the heap is full
void*  kcalloc_try(size_t n);
void*  krealloc(void* p, size_t n);
void   kfree(void* p);
size_t heap_used();
size_t heap_total();
void*  operator new(size_t n);
void*  operator new[](size_t n);
void   operator delete(void* p) noexcept;
void   operator delete[](void* p) noexcept;
void   operator delete(void* p, size_t) noexcept;
void   operator delete[](void* p, size_t) noexcept;
inline void* operator new(size_t, void* p) noexcept { return p; }

// ---------------------------------------------------------------- logging
void serial_init();
void serial_putc(char c);
void klog(const char* fmt, ...);
[[noreturn]] void panic(const char* fmt, ...);

// ---------------------------------------------------------------- time
u64  ticks();                 // milliseconds since boot
void sleep_ms(u32 ms);

// ---------------------------------------------------------------- growable array
template <typename T>
struct Vec {
    T*  data = nullptr;
    int len = 0;
    int cap = 0;
    void reserve(int n) {
        if (n <= cap) return;
        int nc = cap ? cap : 8;
        while (nc < n) nc *= 2;
        T* nd = (T*)kmalloc(sizeof(T) * nc);
        if (data) { memcpy((void*)nd, (void*)data, sizeof(T) * len); kfree(data); }
        data = nd; cap = nc;
    }
    void push(const T& v) { reserve(len + 1); data[len++] = v; }
    void insert(int i, const T& v) {
        reserve(len + 1);
        memmove((void*)(data + i + 1), (void*)(data + i), sizeof(T) * (len - i));
        data[i] = v; len++;
    }
    void remove(int i) {
        memmove((void*)(data + i), (void*)(data + i + 1), sizeof(T) * (len - i - 1));
        len--;
    }
    int  find(const T& v) const { for (int i = 0; i < len; i++) if (data[i] == v) return i; return -1; }
    void clear() { len = 0; }
    void free() { if (data) kfree(data); data = nullptr; len = cap = 0; }
    T& operator[](int i) { return data[i]; }
    const T& operator[](int i) const { return data[i]; }
    T& back() { return data[len - 1]; }
    T* begin() { return data; }
    T* end() { return data + len; }
    const T* begin() const { return data; }
    const T* end() const { return data + len; }
};
