// silicon SDK startup code: the module entry point, static constructors and the few functions
// the compiler needs even in freestanding mode.
#include "silicon.h"

extern "C" {
const SiApi* si = nullptr;

typedef void (*CtorFn)(void);
extern CtorFn __ctors_start[];
extern CtorFn __ctors_end[];

int silicon_main(const SiApi* api, SiApp* app) {
    si = api;
    for (CtorFn* f = __ctors_start; f < __ctors_end; f++) if (*f) (*f)();
    return silicon_app(app);
}

void* memcpy(void* d, const void* s, unsigned long n) {
    unsigned char* a = (unsigned char*)d;
    const unsigned char* b = (const unsigned char*)s;
    while (n--) *a++ = *b++;
    return d;
}
void* memmove(void* d, const void* s, unsigned long n) {
    unsigned char* a = (unsigned char*)d;
    const unsigned char* b = (const unsigned char*)s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void* memset(void* d, int c, unsigned long n) {
    unsigned char* a = (unsigned char*)d;
    while (n--) *a++ = (unsigned char)c;
    return d;
}
int memcmp(const void* x, const void* y, unsigned long n) {
    const unsigned char* a = (const unsigned char*)x;
    const unsigned char* b = (const unsigned char*)y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
unsigned long strlen(const char* s) { unsigned long n = 0; while (s[n]) n++; return n; }
}
