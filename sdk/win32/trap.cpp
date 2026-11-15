// win32run - the missing-function trap.
//
// Every import the runtime can't satisfy is bound to a small stub of its own. Calling it prints
//     [WIN32 UNIMPLEMENTED] dll!function (called from prog.exe+0x1234)
// to the program's console and the serial log, then either returns 0 with ERROR_CALL_NOT_IMPLEMENTED
// (the usual failure value of a Windows function, which most programs handle) or, with --strict, ends
// the program. Each missing function is reported loudly the first time and counted afterwards.
#include "rt.h"

namespace rt {

struct Trap {
    char dll[48];
    char func[80];
    u32  calls;
    Trap* next;
};
static Trap* traps = nullptr;
static Lock trap_lock;
static int trap_calls = 0;
static u8* code_page = nullptr;
static usize code_used = 0, code_cap = 0;

int traps_hit() { return trap_calls; }

static u64 WINAPI trap_hit(Trap* t) {
    void* from = __builtin_return_address(0);
    __sync_fetch_and_add(&t->calls, 1);
    __sync_fetch_and_add(&trap_calls, 1);
    if (t->calls == 1 || opt_strict) {
        char b[96];
        // the shape the task asked for, on its own line, so it is easy to grep from the serial log
        char line[300];
        int n = snfmt(line, sizeof(line), "[WIN32 UNIMPLEMENTED] %s!%s (called from %s)\n", t->dll, t->func, describe_addr(from, b, sizeof(b)));
        con_out(line, (usize)n);
    }
    if (opt_strict) {
        msg("stopping: --strict was given");
        quit(3);
    }
    set_error(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

void* make_trap(const char* dll, const char* func) {
    Guard g(trap_lock);
    for (Trap* t = traps; t; t = t->next)                            // one stub per function is enough
        if (!scmp(t->func, func) && !scmp(t->dll, dll)) return (void*)((u8**)(t + 1))[0];
    Trap* t = (Trap*)izalloc(sizeof(Trap) + sizeof(void*));
    if (!t) return nullptr;
    slcpy(t->dll, dll, sizeof(t->dll));
    slcpy(t->func, func, sizeof(t->func));
    if (!code_page || code_used + 32 > code_cap) {
        code_cap = 0x10000;
        code_page = (u8*)vm_alloc(nullptr, code_cap, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        code_used = 0;
        if (!code_page) return nullptr;
    }
    u8* c = code_page + code_used;
    code_used += 32;
    // mov rcx, t ; mov rax, trap_hit ; jmp rax
    c[0] = 0x48; c[1] = 0xB9; *(u64*)(c + 2) = (u64)t;
    c[10] = 0x48; c[11] = 0xB8; *(u64*)(c + 12) = (u64)&trap_hit;
    c[20] = 0xFF; c[21] = 0xE0;
    for (int i = 22; i < 32; i++) c[i] = 0xCC;
    ((u8**)(t + 1))[0] = c;
    t->next = traps;
    traps = t;
    return c;
}

}  // namespace rt
