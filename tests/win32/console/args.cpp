#include "common.h"
#include <windows.h>
#include <cstring>
#include <cstdlib>

void test_args(int argc, char** argv) {
    std::printf("[arguments and environment]\n");
    check(argc == 4, "argc is 4 (program + 3 arguments)");
    if (argc == 4) {
        check(std::strcmp(argv[1], "alpha") == 0, "argv[1] = alpha");
        check(std::strcmp(argv[2], "two words") == 0, "argv[2] keeps a quoted space");
        check(std::strcmp(argv[3], "back\\slash") == 0, "argv[3] keeps a backslash");
    }
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    check(wargv && wargc == argc, "CommandLineToArgvW agrees with argv");
    if (wargv) LocalFree(wargv);
    char buf[256];
    DWORD n = GetEnvironmentVariableA("OS", buf, sizeof(buf));
    check(n == 10 && std::strcmp(buf, "Windows_NT") == 0, "GetEnvironmentVariableA(OS) = Windows_NT");
    check(SetEnvironmentVariableA("WIN32RUN_TEST", "42") && GetEnvironmentVariableA("WIN32RUN_TEST", buf, sizeof(buf)) == 2, "SetEnvironmentVariableA then read back");
    check(GetEnvironmentVariableA("NO_SUCH_VARIABLE_X", buf, sizeof(buf)) == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND, "missing variable -> ERROR_ENVVAR_NOT_FOUND");
    const char* os = std::getenv("OS");
    check(os && std::strcmp(os, "Windows_NT") == 0, "getenv(OS)");
    WCHAR wbuf[64];
    check(GetEnvironmentVariableW(L"OS", wbuf, 64) == 10 && wcscmp(wbuf, L"Windows_NT") == 0, "GetEnvironmentVariableW(OS)");
    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    check(len > 4 && _stricmp(exe + len - 4, ".exe") == 0, std::string("GetModuleFileNameA = ") + exe);
    check(GetModuleHandleA(nullptr) == GetModuleHandleW(nullptr), "GetModuleHandle(NULL)");
    HMODULE k32 = LoadLibraryA("kernel32.dll");
    check(k32 && GetProcAddress(k32, "GetTickCount64") != nullptr, "LoadLibrary(kernel32) + GetProcAddress");
    check(LoadLibraryA("no_such_library_xyz.dll") == nullptr && GetLastError() == ERROR_MOD_NOT_FOUND, "LoadLibrary of a missing DLL fails cleanly");
}
