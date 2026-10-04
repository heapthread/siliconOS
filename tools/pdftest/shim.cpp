// host build shim: the kernel services the PDF code uses, on top of the C library
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
extern "C" { void* malloc(size_t); void* calloc(size_t, size_t); void* realloc(void*, size_t); void free(void*); void exit(int); double strtod(const char*, char**); }
#include "../../src/kernel/core.h"

void* kmalloc(size_t n) { void* p = malloc(n ? n : 1); if (!p) { fprintf(stderr, "OOM\n"); exit(2); } return p; }
void* kcalloc(size_t n) { void* p = calloc(1, n ? n : 1); if (!p) { fprintf(stderr, "OOM\n"); exit(2); } return p; }
void* kmalloc_try(size_t n) { return malloc(n ? n : 1); }
void* kcalloc_try(size_t n) { return calloc(1, n ? n : 1); }
void* krealloc(void* p, size_t n) { return realloc(p, n); }
void kfree(void* p) { free(p); }
void* operator new(size_t n) { return kmalloc(n); }
void* operator new[](size_t n) { return kmalloc(n); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
void klog(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }
void panic(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); exit(3); }
u64 ticks() { return (u64)clock() * 1000 / CLOCKS_PER_SEC; }
void sleep_ms(u32) {}
void serial_putc(char c) { fputc(c, stderr); }
