#include "common.h"
#include <windows.h>
#include <cstring>
#include <cstdio>

void test_files() {
    std::printf("[files]\n");
    CreateDirectoryA("C:\\home\\win32test", nullptr);
    const char* path = "C:\\home\\win32test\\data.bin";
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(h != INVALID_HANDLE_VALUE, "CreateFileA CREATE_ALWAYS");
    char data[3000];
    for (int i = 0; i < 3000; i++) data[i] = (char)(i * 7);
    DWORD n = 0;
    check(WriteFile(h, data, sizeof(data), &n, nullptr) && n == sizeof(data), "WriteFile 3000 bytes");
    check(GetFileSize(h, nullptr) == 3000, "GetFileSize");
    LARGE_INTEGER sz;
    check(GetFileSizeEx(h, &sz) && sz.QuadPart == 3000, "GetFileSizeEx");
    check(SetFilePointer(h, 1000, nullptr, FILE_BEGIN) == 1000, "SetFilePointer");
    char back[100];
    check(ReadFile(h, back, 100, &n, nullptr) && n == 100 && std::memcmp(back, data + 1000, 100) == 0, "ReadFile at an offset");
    LARGE_INTEGER d, np;
    d.QuadPart = -50;
    check(SetFilePointerEx(h, d, &np, FILE_END) && np.QuadPart == 2950, "SetFilePointerEx from the end");
    check(ReadFile(h, back, 100, &n, nullptr) && n == 50, "ReadFile stops at the end");
    check(ReadFile(h, back, 100, &n, nullptr) && n == 0, "ReadFile at the end reads 0");
    CloseHandle(h);
    DWORD attr = GetFileAttributesA(path);
    check(attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY), "GetFileAttributesA (file)");
    check(GetFileAttributesW(L"C:\\home\\win32test") & FILE_ATTRIBUTE_DIRECTORY, "GetFileAttributesW (directory)");
    check(GetFileAttributesA("C:\\home\\no\\such\\thing") == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_PATH_NOT_FOUND, "missing path -> ERROR_PATH_NOT_FOUND");
    check(CreateFileA("C:\\home\\win32test\\nope.txt", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND, "OPEN_EXISTING on a missing file -> ERROR_FILE_NOT_FOUND");
    // case-insensitive names, forward slashes and relative paths
    h = CreateFileA("c:/HOME/Win32Test/DATA.BIN", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(h != INVALID_HANDLE_VALUE, "paths are case-insensitive and take / too");
    CloseHandle(h);
    // stdio in text mode: \n becomes \r\n on disk and back
    FILE* f = std::fopen("C:\\home\\win32test\\text.txt", "w");
    std::fprintf(f, "line one\nline %d\n", 2);
    std::fclose(f);
    h = CreateFileA("C:\\home\\win32test\\text.txt", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    char raw[64] = {0};
    ReadFile(h, raw, sizeof(raw) - 1, &n, nullptr);
    CloseHandle(h);
    check(n == 18 && std::strcmp(raw, "line one\r\nline 2\r\n") == 0, "fopen text mode writes CR LF");
    f = std::fopen("C:\\home\\win32test\\text.txt", "r");
    char line[64];
    std::fgets(line, sizeof(line), f);
    check(std::strcmp(line, "line one\n") == 0, "fgets in text mode reads LF");
    int v = 0;
    check(std::fscanf(f, "line %d", &v) == 1 && v == 2, "fscanf");
    std::fclose(f);
    f = std::fopen("C:\\home\\win32test\\text.txt", "ab");
    std::fputs("appended", f);
    check(std::ftell(f) == 26, "fopen \"ab\" appends (ftell 26)");
    std::fclose(f);
    // finding files
    for (int i = 0; i < 3; i++) {
        char name[64];
        std::snprintf(name, sizeof(name), "C:\\home\\win32test\\find%d.log", i);
        HANDLE fh = CreateFileA(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        WriteFile(fh, "x", 1, &n, nullptr);
        CloseHandle(fh);
    }
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA("C:\\home\\win32test\\*.log", &fd);
    int found = 0;
    if (fh != INVALID_HANDLE_VALUE) { do found++; while (FindNextFileA(fh, &fd)); FindClose(fh); }
    check(found == 3 && GetLastError() == ERROR_NO_MORE_FILES, "FindFirstFileA/FindNextFileA (*.log) finds 3");
    WIN32_FIND_DATAW wfd;
    fh = FindFirstFileW(L"C:\\home\\win32test\\*", &wfd);
    int all = 0;
    bool saw_dot = false;
    if (fh != INVALID_HANDLE_VALUE) { do { all++; if (!wcscmp(wfd.cFileName, L".")) saw_dot = true; } while (FindNextFileW(fh, &wfd)); FindClose(fh); }
    check(all == 7 && saw_dot, "FindFirstFileW(*) lists . .. and 5 files");
    check(MoveFileA("C:\\home\\win32test\\find0.log", "C:\\home\\win32test\\moved.log") && GetFileAttributesA("C:\\home\\win32test\\moved.log") != INVALID_FILE_ATTRIBUTES, "MoveFileA");
    check(DeleteFileA("C:\\home\\win32test\\moved.log") && GetFileAttributesA("C:\\home\\win32test\\moved.log") == INVALID_FILE_ATTRIBUTES, "DeleteFileA");
    char cwd[MAX_PATH];
    SetCurrentDirectoryA("C:\\home\\win32test");
    GetCurrentDirectoryA(MAX_PATH, cwd);
    check(_stricmp(cwd, "C:\\home\\win32test") == 0, std::string("SetCurrentDirectory/GetCurrentDirectory = ") + cwd);
    check(GetFileAttributesA("data.bin") != INVALID_FILE_ATTRIBUTES, "relative paths use the current directory");
    // console handles
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    check(GetConsoleMode(out, &mode), "GetConsoleMode(stdout)");
    check(SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING), "SetConsoleMode");
    check(GetFileType(out) == FILE_TYPE_CHAR, "GetFileType(stdout) = FILE_TYPE_CHAR");
    const wchar_t* wmsg = L"  ok   WriteConsoleW: gr\u00fc\u00dfe, \u00e9t\u00e9\n";
    std::fflush(stdout);
    check(WriteConsoleW(out, wmsg, (DWORD)wcslen(wmsg), &n, nullptr) && n == wcslen(wmsg), "WriteConsoleW returns the characters written");
}
