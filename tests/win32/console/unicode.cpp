#include "common.h"
#include <windows.h>
#include <cstring>
#include <cwchar>
#include <cstdlib>
#include <string>

void test_unicode() {
    std::printf("[UTF-16 and code pages]\n");
    const char* utf8 = "gr\xC3\xBC\xC3\x9F" "e \xE2\x82\xAC \xF0\x9F\x98\x80";     // gruesse, euro sign, an emoji
    int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    check(need == 11, "MultiByteToWideChar size (a surrogate pair counts as 2)");
    wchar_t w[32];
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 32);
    check(w[2] == 0xFC && w[3] == 0xDF && w[6] == 0x20AC && w[8] == 0xD83D && w[9] == 0xDE00, "UTF-8 -> UTF-16 values");
    char back[64];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, back, sizeof(back), nullptr, nullptr);
    check(n == (int)std::strlen(utf8) + 1 && std::strcmp(back, utf8) == 0, "UTF-16 -> UTF-8 round trip");
    check(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, "\xC3\x28", -1, w, 32) == 0 && GetLastError() == ERROR_NO_UNICODE_TRANSLATION, "MB_ERR_INVALID_CHARS rejects bad UTF-8");
    check(MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 3) == 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "too small a buffer -> ERROR_INSUFFICIENT_BUFFER");
    wchar_t l1[4];
    MultiByteToWideChar(1252, 0, "\x80\xE9", 2, l1, 4);
    check(l1[0] == 0x20AC && l1[1] == 0xE9, "code page 1252 (0x80 is the euro sign)");
    char a[8];
    BOOL used = FALSE;
    WideCharToMultiByte(1252, 0, L"あ", 1, a, 8, nullptr, &used);
    check(a[0] == '?' && used, "unmappable characters use the default character");
    check(GetACP() == CP_UTF8, "GetACP() = 65001 (UTF-8)");
    check(CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, L"Hello", -1, L"hELLO", -1) == CSTR_EQUAL, "CompareStringW NORM_IGNORECASE");
    check(lstrcmpiW(L"apple", L"Banana") < 0 && lstrlenW(L"four") == 4, "lstrcmpiW / lstrlenW");
    wchar_t up[16] = L"straße été";
    CharUpperW(up);
    check(up[0] == 'S' && up[7] == 0xC9, "CharUpperW handles Latin-1");
    // the C runtime's side
    wchar_t wc[16];
    size_t k = std::mbstowcs(wc, "abc\xC3\xA9", 16);
    check(k == 4 && wc[3] == 0xE9, "mbstowcs (UTF-8)");
    char mb[16];
    k = std::wcstombs(mb, L"éx", 16);
    check(k == 3 && (unsigned char)mb[0] == 0xC3, "wcstombs (UTF-8)");
    wchar_t buf[64];
    swprintf(buf, 64, L"%ls=%d %.2f", L"pi", 3, 3.14159);
    check(wcscmp(buf, L"pi=3 3.14") == 0, "swprintf");
    std::wstring ws = L"wide string ";
    ws += std::to_wstring(123);
    check(ws == L"wide string 123", "std::wstring");
}
