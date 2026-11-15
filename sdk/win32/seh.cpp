// win32run - Windows x64 structured exception handling.
//
// Exceptions on x64 Windows are table-driven: every function that isn't a leaf has an entry in its
// module's .pdata saying how to undo its prologue. To dispatch an exception the system walks the stack
// with those tables (RtlVirtualUnwind), asks each frame's language handler what to do, and when one
// takes the exception, unwinds again to it calling the cleanup handlers on the way (RtlUnwindEx).
// This is what makes __try/__except, C++ try/catch (g++'s SEH personality) and setjmp/longjmp work.
//
// Faults (bad pointers, division by zero...) come from silicon's fault handler, which the runtime
// installs for the process; they are turned into Windows exceptions and dispatched the same way.
#include "rt.h"

namespace rt {

enum { EXCEPTION_NONCONTINUABLE = 1, EXCEPTION_UNWINDING = 2, EXCEPTION_EXIT_UNWIND = 4, EXCEPTION_TARGET_UNWIND = 0x20,
       EXCEPTION_COLLIDED_UNWIND = 0x40 };
enum { ExceptionContinueExecution = 0, ExceptionContinueSearch = 1, ExceptionNestedException = 2, ExceptionCollidedUnwind = 3 };
enum { UNW_FLAG_EHANDLER = 1, UNW_FLAG_UHANDLER = 2, UNW_FLAG_CHAININFO = 4 };
static const u32 STATUS_UNWIND = 0xC0000027, STATUS_ACCESS_VIOLATION = 0xC0000005, STATUS_NONCONTINUABLE = 0xC0000025;

typedef u32 (WINAPI *LangHandler)(EXCEPTION_RECORD*, u64 frame, CONTEXT*, DISPATCHER_CONTEXT*);

// ---------------------------------------------------------------- the assembly helpers
// Each entry point captures the context of its caller (as the caller sees it right after the call
// returns) in a CONTEXT on its own stack, then calls into C with that as an extra argument.
#define CAPTURE_TO_RSP_40 \
    "  mov [rsp+0x40+0x78], rax\n  mov [rsp+0x40+0x80], rcx\n  mov [rsp+0x40+0x88], rdx\n  mov [rsp+0x40+0x90], rbx\n" \
    "  mov [rsp+0x40+0xa0], rbp\n  mov [rsp+0x40+0xa8], rsi\n  mov [rsp+0x40+0xb0], rdi\n  mov [rsp+0x40+0xb8], r8\n" \
    "  mov [rsp+0x40+0xc0], r9\n  mov [rsp+0x40+0xc8], r10\n  mov [rsp+0x40+0xd0], r11\n  mov [rsp+0x40+0xd8], r12\n" \
    "  mov [rsp+0x40+0xe0], r13\n  mov [rsp+0x40+0xe8], r14\n  mov [rsp+0x40+0xf0], r15\n" \
    "  lea rax, [rsp+0x540]\n  mov [rsp+0x40+0x98], rax\n  mov rax, [rsp+0x538]\n  mov [rsp+0x40+0xf8], rax\n" \
    "  pushfq\n  pop rax\n  mov [rsp+0x40+0x44], eax\n  stmxcsr [rsp+0x40+0x34]\n  fxsave [rsp+0x40+0x100]\n" \
    "  mov word ptr [rsp+0x40+0x38], cs\n  mov word ptr [rsp+0x40+0x42], ss\n  mov word ptr [rsp+0x40+0x3a], ds\n" \
    "  mov word ptr [rsp+0x40+0x3c], es\n  mov word ptr [rsp+0x40+0x3e], fs\n  mov word ptr [rsp+0x40+0x40], gs\n" \
    "  mov dword ptr [rsp+0x40+0x30], 0x10000F\n" \
    "  mov rax, [rsp+0x40+0x78]\n"

__asm__(".intel_syntax noprefix\n"
        ".text\n"
        // void RtlCaptureContext(CONTEXT*)
        ".globl rt_RtlCaptureContext\n"
        "rt_RtlCaptureContext:\n"
        "  mov [rcx+0x78], rax\n  mov [rcx+0x80], rcx\n  mov [rcx+0x88], rdx\n  mov [rcx+0x90], rbx\n"
        "  mov [rcx+0xa0], rbp\n  mov [rcx+0xa8], rsi\n  mov [rcx+0xb0], rdi\n  mov [rcx+0xb8], r8\n"
        "  mov [rcx+0xc0], r9\n  mov [rcx+0xc8], r10\n  mov [rcx+0xd0], r11\n  mov [rcx+0xd8], r12\n"
        "  mov [rcx+0xe0], r13\n  mov [rcx+0xe8], r14\n  mov [rcx+0xf0], r15\n"
        "  lea rax, [rsp+8]\n  mov [rcx+0x98], rax\n  mov rax, [rsp]\n  mov [rcx+0xf8], rax\n"
        "  pushfq\n  pop rax\n  mov [rcx+0x44], eax\n  stmxcsr [rcx+0x34]\n  fxsave [rcx+0x100]\n"
        "  mov word ptr [rcx+0x38], cs\n  mov word ptr [rcx+0x42], ss\n  mov word ptr [rcx+0x3a], ds\n"
        "  mov word ptr [rcx+0x3c], es\n  mov word ptr [rcx+0x3e], fs\n  mov word ptr [rcx+0x40], gs\n"
        "  mov dword ptr [rcx+0x30], 0x10000F\n"
        "  mov rax, [rcx+0x78]\n"
        "  ret\n"

        // void RaiseException(code, flags, nargs, args)
        ".globl rt_RaiseException\n"
        "rt_RaiseException:\n"
        "  sub rsp, 0x538\n"
        CAPTURE_TO_RSP_40
        "  lea rax, [rsp+0x40]\n  mov [rsp+0x20], rax\n"
        "  call rt_raise_c\n"
        "  add rsp, 0x538\n"
        "  ret\n"

        // void RtlRaiseException(EXCEPTION_RECORD*)
        ".globl rt_RtlRaiseException\n"
        "rt_RtlRaiseException:\n"
        "  sub rsp, 0x538\n"
        CAPTURE_TO_RSP_40
        "  lea rdx, [rsp+0x40]\n"
        "  call rt_raise_rec_c\n"
        "  add rsp, 0x538\n"
        "  ret\n"

        // void RtlUnwindEx(frame, target_ip, rec, retval, CONTEXT* scratch, history)
        ".globl rt_RtlUnwindEx\n"
        "rt_RtlUnwindEx:\n"
        "  sub rsp, 0x538\n"
        CAPTURE_TO_RSP_40
        "  lea rax, [rsp+0x40]\n  mov [rsp+0x20], rax\n"
        "  call rt_unwind_c\n"
        "  add rsp, 0x538\n"
        "  ret\n"

        // void rt_restore_context(CONTEXT*): carry on with exactly these registers
        ".globl rt_restore_context\n"
        "rt_restore_context:\n"
        "  fxrstor [rcx+0x100]\n"
        "  mov rax, [rcx+0x98]\n  sub rax, 24\n"
        "  mov rdx, [rcx+0xf8]\n  mov [rax+16], rdx\n"
        "  mov rdx, [rcx+0x80]\n  mov [rax+8], rdx\n"
        "  mov edx, [rcx+0x44]\n  and edx, 0x0CD5\n  or edx, 0x202\n  mov [rax], rdx\n"
        "  mov rdx, [rcx+0x88]\n  mov rbx, [rcx+0x90]\n  mov rbp, [rcx+0xa0]\n  mov rsi, [rcx+0xa8]\n"
        "  mov rdi, [rcx+0xb0]\n  mov r8, [rcx+0xb8]\n  mov r9, [rcx+0xc0]\n  mov r10, [rcx+0xc8]\n"
        "  mov r11, [rcx+0xd0]\n  mov r12, [rcx+0xd8]\n  mov r13, [rcx+0xe0]\n  mov r14, [rcx+0xe8]\n  mov r15, [rcx+0xf0]\n"
        "  mov rsp, rax\n"
        "  mov rax, [rcx+0x78]\n"
        "  popfq\n  pop rcx\n  ret\n"

        // void rt_fault_restore(CONTEXT*): the same, from inside silicon's fault handler
        ".globl rt_fault_restore\n"
        "rt_fault_restore:\n"
        "  sub rsp, 0xC8\n"
        "  mov rax, [rcx+0xf8]\n  mov [rsp+0x18], rax\n  mov rax, [rcx+0x98]\n  mov [rsp+0x20], rax\n"
        "  mov eax, [rcx+0x44]\n  mov [rsp+0x28], rax\n"
        "  mov rax, [rcx+0x78]\n  mov [rsp+0x30], rax\n  mov rax, [rcx+0x90]\n  mov [rsp+0x38], rax\n"
        "  mov rax, [rcx+0x80]\n  mov [rsp+0x40], rax\n  mov rax, [rcx+0x88]\n  mov [rsp+0x48], rax\n"
        "  mov rax, [rcx+0xa8]\n  mov [rsp+0x50], rax\n  mov rax, [rcx+0xb0]\n  mov [rsp+0x58], rax\n"
        "  mov rax, [rcx+0xa0]\n  mov [rsp+0x60], rax\n  mov rax, [rcx+0xb8]\n  mov [rsp+0x68], rax\n"
        "  mov rax, [rcx+0xc0]\n  mov [rsp+0x70], rax\n  mov rax, [rcx+0xc8]\n  mov [rsp+0x78], rax\n"
        "  mov rax, [rcx+0xd0]\n  mov [rsp+0x80], rax\n  mov rax, [rcx+0xd8]\n  mov [rsp+0x88], rax\n"
        "  mov rax, [rcx+0xe0]\n  mov [rsp+0x90], rax\n  mov rax, [rcx+0xe8]\n  mov [rsp+0x98], rax\n"
        "  mov rax, [rcx+0xf0]\n  mov [rsp+0xa0], rax\n"
        "  fxrstor [rcx+0x100]\n"
        "  mov rdi, rsp\n  mov eax, 49\n  int 0x80\n"
        "  add rsp, 0xC8\n"
        "  jmp rt_restore_context\n"

        // silicon calls this (rcx = Fault*) when a thread faults: keep the FPU/SSE state, then go to C
        ".globl rt_fault_entry\n"
        "rt_fault_entry:\n"
        "  sub rsp, 0x228\n"
        "  fxsave [rsp+0x20]\n"
        "  lea rdx, [rsp+0x20]\n"
        "  call rt_fault_c\n"
        "1: jmp 1b\n"

        // u64 rt_call_on_stack(fn, arg, stack_top): call fn(arg) on another stack
        ".globl rt_call_on_stack\n"
        "rt_call_on_stack:\n"
        "  push rbp\n  mov rbp, rsp\n"
        "  mov rax, rcx\n  mov rcx, rdx\n"
        "  and r8, -16\n  mov rsp, r8\n  sub rsp, 32\n"
        "  call rax\n"
        "  mov rsp, rbp\n  pop rbp\n  ret\n"
        ".att_syntax\n");

extern "C" void WINAPI rt_RtlRaiseException(EXCEPTION_RECORD* rec);
extern "C" void WINAPI rt_RtlUnwindEx(u64 frame, u64 target_ip, EXCEPTION_RECORD* rec, u64 retval, CONTEXT* scratch, void* history);
extern "C" [[noreturn]] void rt_restore_context(CONTEXT* c);
extern "C" [[noreturn]] void rt_fault_restore(CONTEXT* c);

[[noreturn]] void restore_context(CONTEXT* c) {
    Thr* t = cur_thr();
    if (t && t->in_fault) { t->in_fault = false; rt_fault_restore(c); }
    rt_restore_context(c);
}

// ---------------------------------------------------------------- finding a function's unwind entry
struct FnTable { RUNTIME_FUNCTION* fns; u32 count; u64 base, lo, hi; FnTable* next; };
static FnTable* dyn_tables = nullptr;
extern "C" RUNTIME_FUNCTION __rt_pdata_start[], __rt_pdata_end[];       // the runtime's own (build.py keeps them)

static RUNTIME_FUNCTION* search(RUNTIME_FUNCTION* fns, u32 count, u32 rva) {
    int lo = 0, hi = (int)count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (rva < fns[mid].BeginAddress) hi = mid - 1;
        else if (rva >= fns[mid].EndAddress) lo = mid + 1;
        else return &fns[mid];
    }
    return nullptr;
}

RUNTIME_FUNCTION* lookup_function(u64 pc, u64* base) {
    Module* m = mod_by_addr((void*)pc);
    if (m) {
        *base = (u64)m->base;
        return m->pdata ? search(m->pdata, m->pdata_count, (u32)(pc - (u64)m->base)) : nullptr;
    }
    if (pc >= 0x40000000ull && pc < 0x48000000ull) {
        *base = 0;
        return search(__rt_pdata_start, (u32)(__rt_pdata_end - __rt_pdata_start), (u32)pc);
    }
    for (FnTable* t = dyn_tables; t; t = t->next)
        if (pc >= t->lo && pc < t->hi) { *base = t->base; return search(t->fns, t->count, (u32)(pc - t->base)); }
    *base = 0;
    return nullptr;
}

// ---------------------------------------------------------------- RtlVirtualUnwind
struct UnwindInfo { u8 ver_flags, prolog, count, frame; u16 codes[1]; };
enum { UWOP_PUSH_NONVOL = 0, UWOP_ALLOC_LARGE, UWOP_ALLOC_SMALL, UWOP_SET_FPREG, UWOP_SAVE_NONVOL, UWOP_SAVE_NONVOL_FAR,
       UWOP_EPILOG, UWOP_SPARE, UWOP_SAVE_XMM128, UWOP_SAVE_XMM128_FAR, UWOP_PUSH_MACHFRAME };

static u64* reg_ptr(CONTEXT* c, int r) { return &c->Rax + r; }      // Rax Rcx Rdx Rbx Rsp Rbp Rsi Rdi R8..R15: Windows' order

static int opcode_slots(u16 code) {
    switch ((code >> 8) & 15) {
    case UWOP_ALLOC_LARGE: return ((code >> 12) & 15) ? 3 : 2;
    case UWOP_SAVE_NONVOL: case UWOP_SAVE_XMM128: case UWOP_EPILOG: return 2;
    case UWOP_SAVE_NONVOL_FAR: case UWOP_SAVE_XMM128_FAR: return 3;
    }
    return 1;
}

// is pc in an epilogue (add/lea rsp, pops, ret)? Then the prologue codes don't apply
static bool in_epilog(const u8* pc, u64 base, RUNTIME_FUNCTION* fn) {
    if ((pc[0] & 0xF8) == 0x48) {
        if (pc[1] == 0x81 && pc[0] == 0x48 && pc[2] == 0xC4) pc += 7;
        else if (pc[1] == 0x83 && pc[0] == 0x48 && pc[2] == 0xC4) pc += 4;
        else if (pc[1] == 0x8D) {
            if (pc[0] & 6) return false;
            if (((pc[2] >> 3) & 7) != 4 || (pc[2] & 7) == 4) return false;
            if ((pc[2] >> 6) == 1) pc += 4;
            else if ((pc[2] >> 6) == 2) pc += 7;
            else return false;
        }
    }
    for (int guard = 0; guard < 32; guard++) {
        const u8* p = pc;
        if ((*p & 0xF0) == 0x40) p++;
        if (*p >= 0x58 && *p <= 0x5F) { pc = p + 1; continue; }
        if (*p == 0xC3 || *p == 0xC2) return true;
        if (*p == 0xF3 && p[1] == 0xC3) return true;
        if (*p == 0xE9 || *p == 0xEB) {                             // a tail call out of the function
            i64 rel = *p == 0xE9 ? (i64)*(i32*)(p + 1) : (i64)(i8)p[1];
            u64 target = (u64)(p + (*p == 0xE9 ? 5 : 2)) + (u64)rel - base;
            return target < fn->BeginAddress || target >= fn->EndAddress;
        }
        if (*p == 0xFF && p[1] == 0x25) return true;                // jmp [rip+x]
        return false;
    }
    return false;
}

static void run_epilog(const u8* pc, CONTEXT* c) {
    if ((pc[0] & 0xF8) == 0x48) {
        if (pc[1] == 0x81 && pc[2] == 0xC4) { c->Rsp += (u64)(i64)*(i32*)(pc + 3); pc += 7; }
        else if (pc[1] == 0x83 && pc[2] == 0xC4) { c->Rsp += (u64)(i64)(i8)pc[3]; pc += 4; }
        else if (pc[1] == 0x8D) {
            int base = (pc[2] & 7) | ((pc[0] & 1) << 3);
            if ((pc[2] >> 6) == 1) { c->Rsp = *reg_ptr(c, base) + (u64)(i64)(i8)pc[3]; pc += 4; }
            else { c->Rsp = *reg_ptr(c, base) + (u64)(i64)*(i32*)(pc + 3); pc += 7; }
        }
    }
    for (int guard = 0; guard < 32; guard++) {
        int rex = 0;
        if ((*pc & 0xF0) == 0x40) rex = *pc++ & 15;
        if (*pc >= 0x58 && *pc <= 0x5F) {
            *reg_ptr(c, (*pc & 7) | ((rex & 1) << 3)) = *(u64*)c->Rsp;
            c->Rsp += 8;
            pc++;
            continue;
        }
        break;
    }
    c->Rip = *(u64*)c->Rsp;                                         // ret, or the tail call's eventual return
    c->Rsp += 8;
}

extern "C" void* WINAPI rt_RtlVirtualUnwind(u32 type, u64 base, u64 pc, RUNTIME_FUNCTION* fn, CONTEXT* c, void** data,
                                            u64* frame_out, void* ctx_ptrs) {
    (void)ctx_ptrs;
    u64 frame = c->Rsp;
    *frame_out = frame;
    bool mach = false;
    UnwindInfo* info = nullptr;
    u32 offset = 0;
    while (fn->UnwindData & 1) fn = (RUNTIME_FUNCTION*)(base + (fn->UnwindData & ~1u));
    RUNTIME_FUNCTION* first = fn;
    for (int chain = 0; chain < 32; chain++) {
        info = (UnwindInfo*)(base + fn->UnwindData);
        offset = (u32)(pc - base - fn->BeginAddress);
        int freg = info->frame & 15, foff = info->frame >> 4;
        if (fn == first && offset >= info->prolog && in_epilog((const u8*)pc, base, fn)) {
            run_epilog((const u8*)pc, c);
            return nullptr;
        }
        if (freg) {
            if (offset >= info->prolog) frame = *reg_ptr(c, freg) - (u64)foff * 16;
            else
                for (int i = 0; i < info->count; i += opcode_slots(info->codes[i]))
                    if (((info->codes[i] >> 8) & 15) == UWOP_SET_FPREG && offset >= (info->codes[i] & 0xFF)) {
                        frame = *reg_ptr(c, freg) - (u64)foff * 16;
                        break;
                    }
        }
        for (int i = 0; i < info->count; i += opcode_slots(info->codes[i])) {
            u16 code = info->codes[i];
            if (offset < (u32)(code & 0xFF)) continue;               // not done yet at this point of the prologue
            int op = (code >> 8) & 15, oi = code >> 12;
            switch (op) {
            case UWOP_PUSH_NONVOL: *reg_ptr(c, oi) = *(u64*)c->Rsp; c->Rsp += 8; break;
            case UWOP_ALLOC_LARGE: c->Rsp += oi ? *(u32*)&info->codes[i + 1] : (u64)info->codes[i + 1] * 8; break;
            case UWOP_ALLOC_SMALL: c->Rsp += (u64)oi * 8 + 8; break;
            case UWOP_SET_FPREG: c->Rsp = frame; *frame_out = frame; break;
            case UWOP_SAVE_NONVOL: *reg_ptr(c, oi) = *(u64*)(frame + (u64)info->codes[i + 1] * 8); break;
            case UWOP_SAVE_NONVOL_FAR: *reg_ptr(c, oi) = *(u64*)(frame + *(u32*)&info->codes[i + 1]); break;
            case UWOP_SAVE_XMM128: memcpy(&c->Xmm[oi], (void*)(frame + (u64)info->codes[i + 1] * 16), 16); break;
            case UWOP_SAVE_XMM128_FAR: memcpy(&c->Xmm[oi], (void*)(frame + *(u32*)&info->codes[i + 1]), 16); break;
            case UWOP_PUSH_MACHFRAME:
                if (oi) c->Rsp += 8;
                c->Rip = *(u64*)c->Rsp;
                c->Rsp = *(u64*)(c->Rsp + 24);
                mach = true;
                break;
            default: break;
            }
        }
        if (!((info->ver_flags >> 3) & UNW_FLAG_CHAININFO)) break;
        fn = (RUNTIME_FUNCTION*)&info->codes[(info->count + 1) & ~1];
    }
    if (!mach) { c->Rip = *(u64*)c->Rsp; c->Rsp += 8; }
    info = (UnwindInfo*)(base + first->UnwindData);
    offset = (u32)(pc - base - first->BeginAddress);
    if (!((info->ver_flags >> 3) & type)) return nullptr;
    if (offset < info->prolog) return nullptr;
    u32* h = (u32*)&info->codes[(info->count + 1) & ~1];
    *data = h + 1;
    return (void*)(base + *h);
}

// one step out of the frame c describes: through its unwind data, or as a leaf (just the return address)
static void* step(CONTEXT* c, u32 type, void** data, u64* frame, RUNTIME_FUNCTION** fn_out, u64* base_out) {
    u64 base = 0;
    RUNTIME_FUNCTION* fn = lookup_function(c->Rip, &base);
    *fn_out = fn;
    *base_out = base;
    if (!fn) {
        *frame = c->Rsp;
        c->Rip = *(u64*)c->Rsp;
        c->Rsp += 8;
        return nullptr;
    }
    return rt_RtlVirtualUnwind(type, base, c->Rip, fn, c, data, frame, nullptr);
}

static bool on_stack(u64 sp) {
    TEB* t = teb();
    return sp >= (u64)t->StackLimit && sp < (u64)t->StackBase && !(sp & 7);
}

// ---------------------------------------------------------------- vectored handlers and the last-chance filter
typedef i32 (WINAPI *VectoredHandler)(EXCEPTION_POINTERS*);
typedef i32 (WINAPI *TopFilter)(EXCEPTION_POINTERS*);
struct Veh { VectoredHandler fn; Veh* next; };
static Veh* veh_list = nullptr;
static Lock veh_lock;
TopFilter top_filter = nullptr;

static bool call_vectored(EXCEPTION_RECORD* rec, CONTEXT* ctx) {
    EXCEPTION_POINTERS ep = {rec, ctx};
    for (Veh* v = veh_list; v; v = v->next)
        if (v->fn(&ep) == -1) return true;                          // EXCEPTION_CONTINUE_EXECUTION
    return false;
}

// ---------------------------------------------------------------- dispatching
bool dispatch_exception(EXCEPTION_RECORD* rec, CONTEXT* orig) {
    Thr* th = cur_thr();
    if (call_vectored(rec, orig)) return true;
    if (th->dispatch_depth >= 8) { msg("exceptions nested too deeply"); unhandled_exception(rec, orig); }
    th->dispatch_ctx[th->dispatch_depth++] = orig;
    CONTEXT c = *orig;
    bool done = false;
    for (int frames = 0; frames < 4096 && on_stack(c.Rsp) && c.Rip; frames++) {
        CONTEXT before = c;
        void* data = nullptr;
        u64 frame = 0, base = 0;
        RUNTIME_FUNCTION* fn;
        LangHandler h = (LangHandler)step(&c, UNW_FLAG_EHANDLER, &data, &frame, &fn, &base);
        if (h) {
            DISPATCHER_CONTEXT dc;
            memset(&dc, 0, sizeof(dc));
            dc.ControlPc = before.Rip;
            dc.ImageBase = base;
            dc.FunctionEntry = fn;
            dc.EstablisherFrame = frame;
            dc.ContextRecord = &before;
            dc.LanguageHandler = (void*)h;
            dc.HandlerData = data;
            u32 r = h(rec, frame, orig, &dc);
            if (r == ExceptionContinueExecution) {
                if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) {
                    EXCEPTION_RECORD nr;
                    memset(&nr, 0, sizeof(nr));
                    nr.ExceptionCode = STATUS_NONCONTINUABLE;
                    nr.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
                    nr.ExceptionRecord = rec;
                    th->dispatch_depth--;
                    unhandled_exception(&nr, orig);
                }
                done = true;
                break;
            }
        }
        if (c.Rsp <= before.Rsp && c.Rip == before.Rip) break;      // no progress: give up
    }
    th->dispatch_depth--;
    if (done) return true;
    // nobody took it: the program's last-chance filter (the C runtime installs one), then the default
    if (top_filter) {
        EXCEPTION_POINTERS ep = {rec, orig};
        i32 r = top_filter(&ep);
        if (r == -1) return true;                                   // EXCEPTION_CONTINUE_EXECUTION
        if (r == 1) { files_flush_all(); si::exit((int)rec->ExceptionCode); }
    }
    unhandled_exception(rec, orig);
}

static const char* exc_name(u32 code) {
    switch (code) {
    case 0xC0000005: return "access violation";
    case 0xC0000094: return "integer division by zero";
    case 0xC0000095: return "integer overflow";
    case 0xC000001D: return "illegal instruction";
    case 0xC0000096: return "privileged instruction";
    case 0xC00000FD: return "stack overflow";
    case 0x80000003: return "breakpoint";
    case 0x80000004: return "single step";
    case 0xC000008E: return "floating-point division by zero";
    case 0xC0000090: return "floating-point invalid operation";
    case 0xC00002B4: case 0xC00002B5: return "SIMD floating-point exception";
    case 0xC0000409: return "stack buffer overrun (fail fast)";
    case 0x40000015: return "abort";
    case 0xE06D7363: return "Visual C++ exception";
    case 0x20474343: return "uncaught C++ exception (g++)";
    case 0xC0000025: return "noncontinuable exception";
    }
    return "exception";
}

void backtrace(CONTEXT* start, int max) {
    CONTEXT c = *start;
    char b[96];
    for (int i = 0; i < max && on_stack(c.Rsp) && c.Rip; i++) {
        msg("    #%d %s", i, describe_addr((void*)c.Rip, b, sizeof(b)));
        void* data;
        u64 frame, base;
        RUNTIME_FUNCTION* fn;
        u64 sp = c.Rsp, ip = c.Rip;
        if (!vm_readable((void*)c.Rsp, 8)) break;
        step(&c, 0, &data, &frame, &fn, &base);
        if (c.Rsp <= sp && c.Rip == ip) break;
    }
}

void unhandled_exception(EXCEPTION_RECORD* rec, CONTEXT* ctx) {
    char where[96];
    describe_addr(rec->ExceptionAddress, where, sizeof(where));
    if (rec->ExceptionCode == STATUS_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const char* how = rec->ExceptionInformation[0] == 1 ? "writing" : rec->ExceptionInformation[0] == 8 ? "executing" : "reading";
        msg("unhandled exception 0x%08X (access violation %s address 0x%llx) at %s", rec->ExceptionCode, how, rec->ExceptionInformation[1], where);
    } else msg("unhandled exception 0x%08X (%s) at %s", rec->ExceptionCode, exc_name(rec->ExceptionCode), where);
    msg("  rax %016llx rbx %016llx rcx %016llx rdx %016llx", ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
    msg("  rsi %016llx rdi %016llx rbp %016llx rsp %016llx", ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
    msg("  r8  %016llx r9  %016llx r12 %016llx r13 %016llx", ctx->R8, ctx->R9, ctx->R12, ctx->R13);
    msg("  call stack:");
    backtrace(ctx, 16);
    files_flush_all();
    si::exit((int)rec->ExceptionCode);
}

// ---------------------------------------------------------------- raising
extern "C" void rt_raise_c(u32 code, u32 flags, u32 nargs, const u64* args, CONTEXT* ctx) {
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof(rec));
    rec.ExceptionCode = code;
    rec.ExceptionFlags = flags & EXCEPTION_NONCONTINUABLE;
    rec.ExceptionAddress = (void*)ctx->Rip;
    if (nargs > 15) nargs = 15;
    if (args) { rec.NumberParameters = nargs; for (u32 i = 0; i < nargs; i++) rec.ExceptionInformation[i] = args[i]; }
    if (dispatch_exception(&rec, ctx)) {
        // continuing after RaiseException returns to its caller, with whatever registers the handler set
        ctx->Rax = 0;
        restore_context(ctx);
    }
}

extern "C" void rt_raise_rec_c(EXCEPTION_RECORD* rec, CONTEXT* ctx) {
    if (!rec->ExceptionAddress) rec->ExceptionAddress = (void*)ctx->Rip;
    if (dispatch_exception(rec, ctx)) restore_context(ctx);
}

// ---------------------------------------------------------------- unwinding
extern "C" void rt_unwind_c(u64 target_frame, u64 target_ip, EXCEPTION_RECORD* rec, u64 retval, CONTEXT* caller) {
    Thr* th = cur_thr();
    // the scratch CONTEXT and history table are the caller's 5th and 6th arguments
    CONTEXT c;
    // While an exception is being dispatched, the handler that asked for this unwind is running on top of
    // the dispatcher; the frames to unwind are the ones below the point where the exception happened.
    if (th->dispatch_depth > 0 && caller->Rsp < th->dispatch_ctx[th->dispatch_depth - 1]->Rsp) {
        c = *th->dispatch_ctx[th->dispatch_depth - 1];
        th->dispatch_depth--;
    } else c = *caller;
    EXCEPTION_RECORD local;
    if (!rec) {
        memset(&local, 0, sizeof(local));
        local.ExceptionCode = STATUS_UNWIND;
        local.ExceptionAddress = (void*)c.Rip;
        rec = &local;
    }
    rec->ExceptionFlags |= EXCEPTION_UNWINDING;
    if (!target_frame) rec->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;
    for (int frames = 0; frames < 4096; frames++) {
        if (!on_stack(c.Rsp) || !c.Rip) {
            if (!target_frame) { files_flush_all(); si::exit(0); }
            msg("unwinding ran off the stack looking for frame 0x%llx", target_frame);
            unhandled_exception(rec, &c);
        }
        CONTEXT next = c;
        void* data = nullptr;
        u64 frame = 0, base = 0;
        RUNTIME_FUNCTION* fn;
        LangHandler h = (LangHandler)step(&next, UNW_FLAG_UHANDLER, &data, &frame, &fn, &base);
        if (frame == target_frame) rec->ExceptionFlags |= EXCEPTION_TARGET_UNWIND;
        if (h) {
            DISPATCHER_CONTEXT dc;
            memset(&dc, 0, sizeof(dc));
            dc.ControlPc = c.Rip;
            dc.ImageBase = base;
            dc.FunctionEntry = fn;
            dc.EstablisherFrame = frame;
            dc.TargetIp = target_ip;
            dc.ContextRecord = &c;
            dc.LanguageHandler = (void*)h;
            dc.HandlerData = data;
            h(rec, frame, &c, &dc);
            rec->ExceptionFlags &= ~EXCEPTION_COLLIDED_UNWIND;
        }
        if (frame == target_frame) break;
        if (next.Rsp <= c.Rsp && next.Rip == c.Rip) { msg("unwinding made no progress"); unhandled_exception(rec, &c); }
        c = next;
    }
    c.Rax = retval;
    c.Rip = target_ip;
    restore_context(&c);
}

extern "C" void WINAPI rt_RtlUnwind(u64 frame, u64 ip, EXCEPTION_RECORD* rec, u64 retval) {
    rt_RtlUnwindEx(frame, ip, rec, retval, nullptr, nullptr);
}

// ---------------------------------------------------------------- faults
static const u32 FAULT_CODE[32] = {
    0xC0000094, 0x80000004, 0, 0x80000003, 0xC0000095, 0xC000008C, 0xC000001D, 0, 0, 0, 0, 0, 0xC00000FD, 0xC0000005, 0xC0000005, 0,
    0xC0000090, 0x80000002, 0, 0xC00002B5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

extern "C" void rt_fault_c(si::Fault* f, u8* fx) {
    Thr* th = cur_thr();
    th->in_fault = true;
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = 0x10000F;
    ctx.Rax = f->rax; ctx.Rbx = f->rbx; ctx.Rcx = f->rcx; ctx.Rdx = f->rdx; ctx.Rsi = f->rsi; ctx.Rdi = f->rdi;
    ctx.Rbp = f->rbp; ctx.Rsp = f->rsp; ctx.R8 = f->r8; ctx.R9 = f->r9; ctx.R10 = f->r10; ctx.R11 = f->r11;
    ctx.R12 = f->r12; ctx.R13 = f->r13; ctx.R14 = f->r14; ctx.R15 = f->r15; ctx.Rip = f->rip; ctx.EFlags = (u32)f->rflags;
    ctx.SegCs = 0x23; ctx.SegSs = ctx.SegDs = ctx.SegEs = ctx.SegFs = ctx.SegGs = 0x1B;
    memcpy(ctx.FltSave, fx, 512);
    ctx.MxCsr = *(u32*)(fx + 24);
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof(rec));
    u32 n = (u32)f->int_no;
    rec.ExceptionCode = n < 32 && FAULT_CODE[n] ? FAULT_CODE[n] : 0xC0000005;
    rec.ExceptionAddress = (void*)f->rip;
    if (n == 14) {                                                  // page fault: what kind, and where
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = (f->err & 16) ? 8 : (f->err & 2) ? 1 : 0;
        rec.ExceptionInformation[1] = f->cr2;
        // ran off the bottom of the stack?
        TEB* t = teb();
        if (f->cr2 < (u64)t->StackLimit && f->cr2 + 0x10000 >= (u64)t->StackLimit) { rec.ExceptionCode = 0xC00000FD; rec.NumberParameters = 0; }
    } else if (n == 13) {
        const u8* ip = (const u8*)f->rip;
        if (vm_readable(ip, 2) && (ip[0] == 0xF4 || ip[0] == 0xFA || ip[0] == 0xFB || (ip[0] == 0x0F && (ip[1] == 0x20 || ip[1] == 0x22 || ip[1] == 0x30 || ip[1] == 0x32))))
            rec.ExceptionCode = 0xC0000096;
        else { rec.NumberParameters = 2; rec.ExceptionInformation[0] = 0; rec.ExceptionInformation[1] = ~0ull; }
    } else if (n == 3) {
        ctx.Rip--;                                                  // Windows reports the int3 itself
        rec.ExceptionAddress = (void*)ctx.Rip;
    } else if (n == 16 || n == 19) {
        u32 mx = ctx.MxCsr;
        if (n == 19 && (mx & 4)) rec.ExceptionCode = 0xC000008E;
        else if (n == 19 && (mx & 1)) rec.ExceptionCode = 0xC0000090;
    }
    if (opt_trace) {
        char b[96];
        trace("fault %u (code %08X) at %s", n, rec.ExceptionCode, describe_addr((void*)f->rip, b, sizeof(b)));
    }
    if (dispatch_exception(&rec, &ctx)) restore_context(&ctx);
    unhandled_exception(&rec, &ctx);
}

extern "C" void rt_fault_entry(si::Fault* f);
void install_fault_handler() { si::fault_handler(rt_fault_entry); }

// ---------------------------------------------------------------- the API around it
extern "C" RUNTIME_FUNCTION* WINAPI rt_RtlLookupFunctionEntry(u64 pc, u64* base, void* history) {
    (void)history;
    u64 b = 0;
    RUNTIME_FUNCTION* f = lookup_function(pc, &b);
    if (base) *base = b;
    return f;
}

static u8 WINAPI RtlAddFunctionTable(RUNTIME_FUNCTION* fns, u32 count, u64 base) {
    FnTable* t = (FnTable*)izalloc(sizeof(FnTable));
    if (!t) return 0;
    t->fns = fns; t->count = count; t->base = base;
    t->lo = base + (count ? fns[0].BeginAddress : 0);
    t->hi = base + (count ? fns[count - 1].EndAddress : 0);
    t->next = dyn_tables;
    dyn_tables = t;
    return 1;
}
static u8 WINAPI RtlDeleteFunctionTable(RUNTIME_FUNCTION* fns) {
    for (FnTable** p = &dyn_tables; *p; p = &(*p)->next)
        if ((*p)->fns == fns) { FnTable* t = *p; *p = t->next; ifree(t); return 1; }
    return 0;
}
static u8 WINAPI RtlInstallFunctionTableCallback(u64 id, u64 base, u32 len, void* cb, void* ctx, const WCHAR* dll) {
    (void)id; (void)base; (void)len; (void)cb; (void)ctx; (void)dll;
    return 1;
}

static void* WINAPI AddVectoredExceptionHandler(u32 first, VectoredHandler fn) {
    Veh* v = (Veh*)izalloc(sizeof(Veh));
    if (!v) return nullptr;
    v->fn = fn;
    Guard g(veh_lock);
    if (first || !veh_list) { v->next = veh_list; veh_list = v; }
    else { Veh* t = veh_list; while (t->next) t = t->next; t->next = v; }
    return v;
}
static u32 WINAPI RemoveVectoredExceptionHandler(void* h) {
    Guard g(veh_lock);
    for (Veh** p = &veh_list; *p; p = &(*p)->next) if (*p == h) { *p = (*p)->next; ifree(h); return 1; }
    return 0;
}
static void* WINAPI AddVectoredContinueHandler(u32 first, VectoredHandler fn) { (void)first; (void)fn; static int dummy; return &dummy; }
static u32 WINAPI RemoveVectoredContinueHandler(void* h) { (void)h; return 1; }

static TopFilter WINAPI SetUnhandledExceptionFilter(TopFilter f) { TopFilter old = top_filter; top_filter = f; return old; }
static i32 WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
    unhandled_exception(ep->ExceptionRecord, ep->ContextRecord);
}
static void WINAPI RtlRestoreContext(CONTEXT* c, EXCEPTION_RECORD* rec) { (void)rec; restore_context(c); }
static NTSTATUS WINAPI NtContinue(CONTEXT* c, u8 alert) { (void)alert; restore_context(c); }
static void WINAPI RaiseFailFastException(EXCEPTION_RECORD* rec, CONTEXT* ctx, u32 flags) {
    (void)flags;
    msg("fail fast exception");
    if (rec && ctx) unhandled_exception(rec, ctx);
    quit(0xC0000409);
}
static void WINAPI rt_fastfail(u32 code) { msg("__fastfail(%u)", code); quit(0xC0000409); }

// the language handler of C's __try / __except / __finally (also what MinGW's SEH macros use)
struct ScopeTable { u32 Count; struct { u32 Begin, End, Handler, Jump; } r[1]; };
typedef i32 (WINAPI *Filter)(EXCEPTION_POINTERS*, u64 frame);
typedef void (WINAPI *Termination)(u8 abnormal, u64 frame);

extern "C" u32 WINAPI rt_C_specific_handler(EXCEPTION_RECORD* rec, u64 frame, CONTEXT* ctx, DISPATCHER_CONTEXT* dc) {
    ScopeTable* t = (ScopeTable*)dc->HandlerData;
    u64 base = dc->ImageBase;
    u32 pc = (u32)(dc->ControlPc - base);
    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        u32 target = (u32)(dc->TargetIp - base);
        for (u32 i = dc->ScopeIndex; i < t->Count; i++) {
            if (pc < t->r[i].Begin || pc >= t->r[i].End) continue;
            if ((rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) && target >= t->r[i].Begin && target < t->r[i].End) break;
            if (t->r[i].Jump) continue;                              // an __except, not a __finally
            dc->ScopeIndex = i + 1;
            ((Termination)(base + t->r[i].Handler))(1, frame);
        }
        return ExceptionContinueSearch;
    }
    for (u32 i = dc->ScopeIndex; i < t->Count; i++) {
        if (pc < t->r[i].Begin || pc >= t->r[i].End || !t->r[i].Jump) continue;
        i32 r;
        if (t->r[i].Handler == 1) r = 1;
        else {
            EXCEPTION_POINTERS ep = {rec, ctx};
            r = ((Filter)(base + t->r[i].Handler))(&ep, frame);
        }
        if (r < 0) return ExceptionContinueExecution;
        if (r == 0) continue;
        rt_RtlUnwindEx(frame, base + t->r[i].Jump, rec, rec->ExceptionCode, dc->ContextRecord, dc->HistoryTable);
    }
    return ExceptionContinueSearch;
}

static const Export seh_k32[] = {
    EA("RtlCaptureContext", rt_RtlCaptureContext), EA("RaiseException", rt_RaiseException),
    EA("RtlUnwindEx", rt_RtlUnwindEx), EA("RtlUnwind", rt_RtlUnwind), EA("RtlVirtualUnwind", rt_RtlVirtualUnwind),
    EA("RtlLookupFunctionEntry", rt_RtlLookupFunctionEntry), E(RtlAddFunctionTable), E(RtlDeleteFunctionTable),
    E(RtlInstallFunctionTableCallback), E(AddVectoredExceptionHandler), E(RemoveVectoredExceptionHandler),
    E(AddVectoredContinueHandler), E(RemoveVectoredContinueHandler), E(SetUnhandledExceptionFilter),
    E(UnhandledExceptionFilter), E(RtlRestoreContext), E(RaiseFailFastException),
};
EXPORT_TABLE("kernel32.dll", seh_k32);

static const Export seh_nt[] = {
    EA("RtlCaptureContext", rt_RtlCaptureContext), EA("RtlRaiseException", rt_RtlRaiseException),
    EA("RtlUnwindEx", rt_RtlUnwindEx), EA("RtlUnwind", rt_RtlUnwind), EA("RtlVirtualUnwind", rt_RtlVirtualUnwind),
    EA("RtlLookupFunctionEntry", rt_RtlLookupFunctionEntry), E(RtlAddFunctionTable), E(RtlDeleteFunctionTable),
    E(RtlInstallFunctionTableCallback), EA("RtlAddVectoredExceptionHandler", AddVectoredExceptionHandler),
    EA("RtlRemoveVectoredExceptionHandler", RemoveVectoredExceptionHandler), E(RtlRestoreContext), E(NtContinue),
    EA("__C_specific_handler", rt_C_specific_handler),
};
EXPORT_TABLE("ntdll.dll", seh_nt);

static const Export seh_crt[] = {
    EA("__C_specific_handler", rt_C_specific_handler), EA("__fastfail", rt_fastfail),
};
EXPORT_TABLE("ucrtbase.dll", seh_crt);

}  // namespace rt
