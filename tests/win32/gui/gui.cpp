// win32run phase 4 test: a real Win32 GUI program. Double-buffered GDI drawing (a DIB section in a memory
// DC, blitted to the window), a timer animation, keyboard input and mouse clicks. It also checks a set of
// GDI results by reading pixels back, and reports what happens on standard output (silicon's serial log).
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

static HDC g_back = nullptr;
static HBITMAP g_bmp = nullptr, g_old_bmp = nullptr;
static unsigned* g_bits = nullptr;
static int g_w = 0, g_h = 0;
static int g_frame = 0;
static double g_bx = 80, g_by = 90, g_vx = 3.1, g_vy = 2.3;
static int g_mx = -1, g_my = -1;
static int g_clicks = 0, g_click_x = -1, g_click_y = -1;
static char g_typed[64] = "";
static int g_last_vk = 0;
static COLORREF g_ball = RGB(230, 80, 60);
static HFONT g_font_big, g_font_small, g_font_mono;
struct Burst { int x, y, age; COLORREF c; };
static Burst g_bursts[16];
static DWORD g_start;

static void log(const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    printf("[gui] %s\n", b);
    fflush(stdout);
}

static void make_back_buffer(HWND h, int w, int hh) {
    if (w <= 0 || hh <= 0) return;
    HDC wdc = GetDC(h);
    if (!g_back) g_back = CreateCompatibleDC(wdc);
    if (g_bmp) { SelectObject(g_back, g_old_bmp); DeleteObject(g_bmp); }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -hh;                     // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, (void**)&g_bits, nullptr, 0);
    g_old_bmp = (HBITMAP)SelectObject(g_back, g_bmp);
    g_w = w; g_h = hh;
    ReleaseDC(h, wdc);
}

static void render(HWND h) {
    HDC dc = g_back;
    // sky gradient written straight into the DIB section's pixels
    for (int y = 0; y < g_h; y++) {
        unsigned r = 20 + y * 40 / (g_h ? g_h : 1), g = 30 + y * 60 / (g_h ? g_h : 1), b = 70 + y * 100 / (g_h ? g_h : 1);
        unsigned c = (r << 16) | (g << 8) | b;
        unsigned* row = g_bits + (size_t)y * g_w;
        for (int x = 0; x < g_w; x++) row[x] = c;
    }
    GdiFlush();
    // ground
    RECT ground = {0, g_h - 60, g_w, g_h};
    HBRUSH gb = CreateSolidBrush(RGB(40, 120, 70));
    FillRect(dc, &ground, gb);
    DeleteObject(gb);
    // a rotating spoke wheel
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 220, 120));
    HGDIOBJ op = SelectObject(dc, pen);
    int cx = g_w - 110, cy = 110;
    double a = g_frame * 0.05;
    for (int i = 0; i < 12; i++) {
        double t = a + i * 3.14159265 / 6;
        MoveToEx(dc, cx, cy, nullptr);
        LineTo(dc, cx + (int)(70 * cos(t)), cy + (int)(70 * sin(t)));
    }
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - 72, cy - 72, cx + 72, cy + 72);
    SelectObject(dc, op);
    DeleteObject(pen);
    // a house: rectangle and polygon
    HBRUSH wall = CreateSolidBrush(RGB(200, 170, 120));
    HGDIOBJ ob = SelectObject(dc, wall);
    Rectangle(dc, 40, g_h - 140, 160, g_h - 60);
    SelectObject(dc, ob);
    DeleteObject(wall);
    HBRUSH roof = CreateSolidBrush(RGB(150, 50, 40));
    ob = SelectObject(dc, roof);
    POINT tri[3] = {{30, g_h - 140}, {100, g_h - 190}, {170, g_h - 140}};
    Polygon(dc, tri, 3);
    SelectObject(dc, ob);
    DeleteObject(roof);
    // the bouncing ball
    HBRUSH ball = CreateSolidBrush(g_ball);
    ob = SelectObject(dc, ball);
    HGDIOBJ np = SelectObject(dc, GetStockObject(BLACK_PEN));
    Ellipse(dc, (int)g_bx - 20, (int)g_by - 20, (int)g_bx + 20, (int)g_by + 20);
    SelectObject(dc, np);
    SelectObject(dc, ob);
    DeleteObject(ball);
    // click bursts
    for (auto& b : g_bursts) {
        if (b.age <= 0) continue;
        int r = (30 - b.age) * 2;
        HPEN bp = CreatePen(PS_SOLID, 2, b.c);
        op = SelectObject(dc, bp);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, b.x - r, b.y - r, b.x + r, b.y + r);
        SelectObject(dc, op);
        DeleteObject(bp);
    }
    // text
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font_big);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, 16, 12, L"win32run GDI demo", 17);
    SelectObject(dc, g_font_small);
    char line[160];
    DWORD secs = (GetTickCount() - g_start) / 1000;
    snprintf(line, sizeof(line), "frame %d   %lus   mouse %d,%d   clicks %d", g_frame, secs, g_mx, g_my, g_clicks);
    TextOutA(dc, 16, 44, line, (int)strlen(line));
    snprintf(line, sizeof(line), "last key: 0x%02X   typed: %s", g_last_vk, g_typed);
    TextOutA(dc, 16, 62, line, (int)strlen(line));
    RECT box = {g_w / 2 - 150, g_h - 52, g_w / 2 + 150, g_h - 8};
    SelectObject(dc, g_font_mono);
    SetTextColor(dc, RGB(250, 250, 200));
    DrawTextA(dc, "click to make bursts, type to fill the line, right-click to recolour, Esc quits", -1, &box, DT_CENTER | DT_WORDBREAK);
    // present
    HDC wdc = GetDC(h);
    BitBlt(wdc, 0, 0, g_w, g_h, dc, 0, 0, SRCCOPY);
    ReleaseDC(h, wdc);
}

// GDI results checked by reading pixels back from a memory DC
static void self_test() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) { total++; if (ok) pass++; else log("self-test FAIL: %s", what); };
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = 64; bi.bmiHeader.biHeight = 64; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    unsigned* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT all = {0, 0, 64, 64};
    FillRect(dc, &all, (HBRUSH)GetStockObject(WHITE_BRUSH));
    check(GetPixel(dc, 10, 10) == RGB(255, 255, 255), "FillRect white");
    check(bits[0] == 0xFFFFFF, "DIB bits reflect GDI drawing");
    SetPixel(dc, 5, 6, RGB(10, 20, 30));
    check(GetPixel(dc, 5, 6) == RGB(10, 20, 30), "SetPixel/GetPixel");
    check(bits[(63 - 6) * 64 + 5] == 0x0A141E, "bottom-up DIB layout");
    HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
    SelectObject(dc, red);
    SelectObject(dc, GetStockObject(NULL_PEN));
    Rectangle(dc, 20, 20, 40, 40);
    check(GetPixel(dc, 30, 30) == RGB(255, 0, 0) && GetPixel(dc, 19, 30) == RGB(255, 255, 255), "Rectangle fill");
    SelectObject(dc, GetStockObject(BLACK_PEN));
    MoveToEx(dc, 0, 50, nullptr);
    LineTo(dc, 63, 50);
    check(GetPixel(dc, 32, 50) == 0 && GetPixel(dc, 63, 50) == RGB(255, 255, 255), "LineTo excludes the last point");
    FillRect(dc, &all, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SelectObject(dc, red);
    SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, 0, 0, 64, 64);
    check(GetPixel(dc, 32, 32) == RGB(255, 0, 0) && GetPixel(dc, 1, 1) == RGB(255, 255, 255), "Ellipse");
    HDC dc2 = CreateCompatibleDC(dc);
    HBITMAP bmp2 = CreateCompatibleBitmap(dc, 16, 16);
    HGDIOBJ old2 = SelectObject(dc2, bmp2);
    RECT r16 = {0, 0, 16, 16};
    HBRUSH blue = CreateSolidBrush(RGB(0, 0, 255));
    FillRect(dc2, &r16, blue);
    BitBlt(dc, 0, 0, 16, 16, dc2, 0, 0, SRCCOPY);
    check(GetPixel(dc, 8, 8) == RGB(0, 0, 255), "BitBlt SRCCOPY");
    StretchBlt(dc, 0, 0, 64, 64, dc2, 0, 0, 16, 16, SRCCOPY);
    check(GetPixel(dc, 60, 60) == RGB(0, 0, 255), "StretchBlt");
    BitBlt(dc, 0, 0, 16, 16, nullptr, 0, 0, WHITENESS);
    check(GetPixel(dc, 4, 4) == RGB(255, 255, 255), "BitBlt WHITENESS");
    BitBlt(dc, 0, 0, 16, 16, dc2, 0, 0, SRCINVERT);
    check(GetPixel(dc, 4, 4) == RGB(255, 255, 0), "BitBlt SRCINVERT");
    // a 8-bit DIB through StretchDIBits
    struct { BITMAPINFOHEADER h; RGBQUAD pal[2]; } b8 = {};
    b8.h.biSize = sizeof(b8.h); b8.h.biWidth = 4; b8.h.biHeight = 1; b8.h.biPlanes = 1; b8.h.biBitCount = 8;
    b8.pal[1].rgbGreen = 255;
    unsigned char px[4] = {1, 1, 1, 1};
    StretchDIBits(dc, 0, 0, 64, 4, 0, 0, 4, 1, px, (BITMAPINFO*)&b8, DIB_RGB_COLORS, SRCCOPY);
    check(GetPixel(dc, 40, 2) == RGB(0, 255, 0), "StretchDIBits 8-bit palette");
    SIZE sz;
    SelectObject(dc, g_font_small);
    GetTextExtentPoint32A(dc, "Hello", 5, &sz);
    check(sz.cx > 20 && sz.cx < 80 && sz.cy > 10 && sz.cy < 30, "GetTextExtentPoint32");
    FillRect(dc, &all, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SetTextColor(dc, RGB(0, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    TextOutA(dc, 2, 2, "WWW", 3);
    int dark = 0;
    for (int y = 0; y < 20; y++) for (int x = 0; x < 40; x++) if ((GetPixel(dc, x, y) & 0xFF) < 128) dark++;
    check(dark > 20, "TextOut draws glyphs");
    TEXTMETRICA tm;
    GetTextMetricsA(dc, &tm);
    check(tm.tmHeight > 8 && tm.tmAscent > 6, "GetTextMetrics");
    SelectObject(dc2, old2);
    DeleteObject(bmp2);
    DeleteDC(dc2);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteObject(red);
    DeleteObject(blue);
    DeleteDC(dc);
    log("GDI self-test: %d/%d passed", pass, total);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)l;
        log("WM_CREATE (%ls)", cs->lpszName);
        g_font_big = CreateFontW(-24, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_font_small = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_font_mono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        self_test();
        SetTimer(h, 1, 16, nullptr);
        return 0;
    }
    case WM_SIZE:
        log("WM_SIZE %dx%d", LOWORD(l), HIWORD(l));
        make_back_buffer(h, LOWORD(l), HIWORD(l));
        return 0;
    case WM_TIMER: {
        g_frame++;
        g_bx += g_vx; g_by += g_vy;
        if (g_bx < 20 || g_bx > g_w - 20) g_vx = -g_vx;
        if (g_by < 20 || g_by > g_h - 80) g_vy = -g_vy;
        for (auto& b : g_bursts) if (b.age > 0) b.age--;
        if (g_frame % 120 == 0) log("frame %d (timer animation running)", g_frame);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        render(h);
        EndPaint(h, &ps);
        if (g_frame == 1) log("first frame painted");
        return 0;
    }
    case WM_ERASEBKGND: return 1;                    // the back buffer covers everything
    case WM_KEYDOWN:
        g_last_vk = (int)w;
        log("WM_KEYDOWN vk=0x%02X", (int)w);
        if (w == VK_ESCAPE) DestroyWindow(h);
        if (w == VK_F1) MessageBoxW(h, L"win32run runs this window with silicon's desktop.", L"About", MB_OK | MB_ICONINFORMATION);
        return 0;
    case WM_CHAR: {
        size_t n = strlen(g_typed);
        if (w == 8) { if (n) g_typed[n - 1] = 0; }
        else if (w >= 32 && w < 127 && n < sizeof(g_typed) - 1) { g_typed[n] = (char)w; g_typed[n + 1] = 0; }
        log("WM_CHAR '%c' -> \"%s\"", w >= 32 && w < 127 ? (char)w : '?', g_typed);
        return 0;
    }
    case WM_MOUSEMOVE: g_mx = (short)LOWORD(l); g_my = (short)HIWORD(l); return 0;
    case WM_LBUTTONDOWN: {
        g_click_x = (short)LOWORD(l); g_click_y = (short)HIWORD(l);
        g_clicks++;
        for (auto& b : g_bursts) if (b.age <= 0) { b = {g_click_x, g_click_y, 30, RGB(255, 255, 255)}; break; }
        log("WM_LBUTTONDOWN at %d,%d (click %d)", g_click_x, g_click_y, g_clicks);
        return 0;
    }
    case WM_RBUTTONDOWN:
        g_ball = RGB(rand() % 200 + 55, rand() % 200 + 55, rand() % 200 + 55);
        log("WM_RBUTTONDOWN: the ball changes colour");
        return 0;
    case WM_CLOSE: log("WM_CLOSE"); DestroyWindow(h); return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        log("WM_DESTROY after %d frames", g_frame);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    g_start = GetTickCount();
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"Win32runDemo";
    if (!RegisterClassExW(&wc)) { log("RegisterClassEx failed"); return 1; }
    RECT r = {0, 0, 640, 420};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowExW(0, L"Win32runDemo", L"win32run GDI demo", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!h) { log("CreateWindowEx failed: %lu", GetLastError()); return 1; }
    ShowWindow(h, show);
    UpdateWindow(h);
    RECT cr;
    GetClientRect(h, &cr);
    log("window created, client %ldx%ld", cr.right, cr.bottom);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    log("message loop ended (wParam %d)", (int)msg.wParam);
    return (int)msg.wParam;
}
