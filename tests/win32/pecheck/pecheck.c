/* win32run phase 2 check: the loader and the Windows execution environment.
   Linked at 0x400000 (below 4 GiB, where silicon can't put it), so the loader has to apply relocations. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <winternl.h>

static int fails = 0;
#define CHECK(cond, what) do { if (cond) printf("  ok   %s\n", what); else { printf("  FAIL %s\n", what); fails++; } } while (0)

static const char rodata_string[] = "read-only data";
static int data_value = 42;

static LONG WINAPI on_fault(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        printf("  ok   writing to .rdata raised an access violation (address %p)\n", (void*)ep->ExceptionRecord->ExceptionInformation[1]);
        ep->ContextRecord->Rip += 3;                 /* skip the faulting "mov byte [rax], dl" style store (3 bytes, see below) */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv) {
    printf("pecheck: loader and TEB/PEB\n");
    HMODULE self = GetModuleHandleA(NULL);
    printf("  image base %p (linked for 0x400000)\n", (void*)self);
    CHECK((ULONG_PTR)self != 0x400000, "relocated away from its fixed base");
    CHECK(strcmp(rodata_string, "read-only data") == 0 && data_value == 42, "data reached through relocated addresses");

    void* teb_self = (void*)__readgsqword(0x30);
    PEB* peb = (PEB*)__readgsqword(0x60);
    CHECK(teb_self == (void*)NtCurrentTeb(), "gs:[0x30] is the TEB");
    CHECK(peb == NtCurrentTeb()->ProcessEnvironmentBlock, "gs:[0x60] is the PEB");
    CHECK(*(void**)((char*)peb + 0x10) == (void*)self, "PEB.ImageBaseAddress");
    CHECK(*(HANDLE*)((char*)peb + 0x30) == GetProcessHeap(), "PEB.ProcessHeap is GetProcessHeap()");
    RTL_USER_PROCESS_PARAMETERS* pp = peb->ProcessParameters;
    printf("  command line: %ls\n", pp->CommandLine.Buffer);
    CHECK(wcscmp(pp->CommandLine.Buffer, GetCommandLineW()) == 0, "ProcessParameters.CommandLine");
    CHECK(*(void**)((char*)pp + 0x80) != NULL, "ProcessParameters.Environment");
    CHECK(GetLastError() == *(DWORD*)((char*)teb_self + 0x68) , "TEB.LastErrorValue backs GetLastError");
    SetLastError(1234);
    CHECK(*(DWORD*)((char*)teb_self + 0x68) == 1234, "SetLastError writes the TEB");

    /* the loader's module list, as debuggers and some runtimes walk it */
    LIST_ENTRY* head = (LIST_ENTRY*)((char*)peb->Ldr + 0x10);
    int n = 0;
    printf("  InLoadOrderModuleList:");
    for (LIST_ENTRY* e = head->Flink; e != head && n < 32; e = e->Flink, n++) {
        UNICODE_STRING* base_name = (UNICODE_STRING*)((char*)e + 0x58);
        printf(" %ls", base_name->Buffer);
    }
    printf("\n");
    CHECK(n >= 2, "modules listed (the program first)");

    /* page protection follows the section flags */
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery((void*)main, &mbi, sizeof(mbi));
    CHECK(mbi.Protect == PAGE_EXECUTE_READ, ".text is R-X");
    VirtualQuery((void*)rodata_string, &mbi, sizeof(mbi));
    CHECK(mbi.Protect == PAGE_READONLY, ".rdata is R--");
    VirtualQuery((void*)&data_value, &mbi, sizeof(mbi));
    CHECK(mbi.Protect == PAGE_READWRITE, ".data is RW-");
    AddVectoredExceptionHandler(1, on_fault);
    volatile char* p = (volatile char*)rodata_string;
    __asm__ volatile("movb $0x58, (%0)\n\t" ::"r"(p) : "memory");   /* 3 bytes: c6 00 58 */

    /* a missing function, looked up at run time: reported, and the trap returns 0 */
    FARPROC f = GetProcAddress(GetModuleHandleA("kernel32.dll"), "ThisFunctionDoesNotExist");
    CHECK(f != NULL, "GetProcAddress of a missing kernel32 function gives a trap");
    if (f) { INT_PTR r = ((INT_PTR (WINAPI*)(void))f)(); CHECK(r == 0 && GetLastError() == ERROR_CALL_NOT_IMPLEMENTED, "the trap returns 0 / ERROR_CALL_NOT_IMPLEMENTED"); }
    /* an import that is linked but not implemented */
    LPVOID fiber = CreateFiber(0, NULL, NULL);
    CHECK(fiber == NULL, "an unimplemented import (CreateFiber) is trapped");

    printf("pecheck: %s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
    return fails;
}
