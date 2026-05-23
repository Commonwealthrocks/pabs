// log_window.cpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "ui.hpp"
#include "logger.hpp"
#include <windows.h>
#include <richedit.h>
#include <string>
#include <sstream>
static HWND log_hwnd = nullptr;
static HWND rich_hwnd = nullptr;
static int last_count = -1;
static HMODULE richedit_lib = nullptr;
#define ID_RICH_LOG 1001
#define ID_BTN_CLEAR 1002
static COLORREF level_color(log_level lvl)
{
    switch (lvl)
    {
    case log_level::ok:
        return RGB(100, 220, 100);
    case log_level::info:
        return RGB(100, 160, 255);
    case log_level::error:
        return RGB(255, 160, 60);
    case log_level::incoming_fatal:
        return RGB(255, 120, 30);
    case log_level::fatal:
        return RGB(255, 80, 80);
    case log_level::wat:
        return RGB(200, 80, 255);
    default:
        return RGB(200, 200, 200);
    }
}
static const char *level_prefix(log_level lvl)
{
    switch (lvl)
    {
    case log_level::ok:
        return "[ OK ]   ";
    case log_level::info:
        return "[ INFO ] ";
    case log_level::error:
        return "[ ERROR ]";
    case log_level::incoming_fatal:
        return "[ FATAL? ]";
    case log_level::fatal:
        return "[ FATAL ]";
    case log_level::wat:
        return "[ ??? ]  ";
    default:
        return "[ ??? ]  ";
    }
}
static void rac(const char *text, COLORREF color)
{
    int len = GetWindowTextLengthA(rich_hwnd);
    SendMessage(rich_hwnd, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    CHARFORMAT2A cf = {};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
    cf.crTextColor = color;
    cf.yHeight = 180;
    strcpy_s(cf.szFaceName, "Consolas");
    SendMessage(rich_hwnd, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessage(rich_hwnd, EM_REPLACESEL, FALSE, (LPARAM)text);
}
// m; Yeah now I see why you said we'd use Dear ImGui
// c; it took you THIS to realize?
// c; https://learn.microsoft.com/en-us/windows/win32/uxguide/guidelines
// c; if you wanna be useful
// m; I'll think about it...
static void _rebuild()
{
    std::lock_guard<std::mutex> lock(log_get_mutex());
    int count = log_ring_count();
    if (count == last_count)
        return;
    last_count = count;
    if (!rich_hwnd)
        return;
    SendMessage(rich_hwnd, WM_SETREDRAW, FALSE, 0);
    SendMessage(rich_hwnd, EM_SETSEL, 0, -1);
    SendMessage(rich_hwnd, EM_REPLACESEL, FALSE, (LPARAM) "");
    int head = log_ring_head();
    const log_entry *entries = log_ring_entries();
    for (int i = 0; i < count; i++)
    {
        int idx = (head - count + i + LOG_R_SIZE) % LOG_R_SIZE;
        const log_entry &e = entries[idx];
        COLORREF col = level_color(e.level);
        const char *pfx = level_prefix(e.level);
        char line[1024];
        snprintf(line, sizeof(line), "%s  %-20s  %s\r\n", pfx, e.func, e.message);
        rac(line, col);
    }
    SendMessage(rich_hwnd, EM_SETSEL, -1, -1);
    SendMessage(rich_hwnd, EM_SCROLLCARET, 0, 0);
    SendMessage(rich_hwnd, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(rich_hwnd, nullptr, TRUE);
}
static LRESULT CALLBACK LogWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SIZE:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int toolbar_h = 28;
        if (rich_hwnd)
            SetWindowPos(rich_hwnd, nullptr, 0, toolbar_h, rc.right, rc.bottom - toolbar_h, SWP_NOZORDER);
        HWND btn = GetDlgItem(hwnd, ID_BTN_CLEAR);
        if (btn)
            SetWindowPos(btn, nullptr, rc.right - 72, 3, 68, 22, SWP_NOZORDER);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN_CLEAR)
        {
            if (GetKeyState(VK_SHIFT) & 0x8000)
            {
                log_wipe();
                SendMessage(rich_hwnd, EM_SETSEL, 0, -1);
                SendMessage(rich_hwnd, EM_REPLACESEL, FALSE, (LPARAM) "");
                last_count = 0;
            }
            else
            {
                _log_clear();
                last_count = -1;
                SendMessage(rich_hwnd, EM_SETSEL, 0, -1);
                SendMessage(rich_hwnd, EM_REPLACESEL, FALSE, (LPARAM) "");
                last_count = 0;
            }
        }
        break;
    case WM_ERASEBKGND:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.bottom = 28;
        HBRUSH br = CreateSolidBrush(RGB(30, 30, 30));
        FillRect((HDC)wp, &rc, br);
        DeleteObject(br);
        return 1;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
// c; we could honestly re-make this in imgui since we use viewports now, but hey if this works DON'T fucking touch it
void _log_init(HINSTANCE hinstance, HWND)
{
    richedit_lib = LoadLibraryW(L"msftedit.dll");
    if (!richedit_lib)
        richedit_lib = LoadLibraryW(L"riched20.dll");
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = LogWndProc;
        wc.hInstance = hinstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(22, 22, 22));
        wc.lpszClassName = L"logs";
        RegisterClassExW(&wc);
        registered = true;
    }
    log_hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"logs", L"PABS - logs", WS_OVERLAPPEDWINDOW, 100, 620, 760, 320, nullptr, nullptr, hinstance, nullptr);
    if (!log_hwnd)
        return;
    BOOL dark = TRUE;
    HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
    if (!dwm)
        dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm)
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
        auto fn = reinterpret_cast<HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD)>(
            GetProcAddress(dwm, "DwmSetWindowAttribute"));
#pragma GCC diagnostic pop
        if (fn)
            fn(log_hwnd, 20, &dark, sizeof(dark));
    }
    rich_hwnd = CreateWindowExA(0, "RICHEDIT50W", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 28, 760, 292, log_hwnd, (HMENU)ID_RICH_LOG, hinstance, nullptr);
    if (!rich_hwnd)
    {
        rich_hwnd = CreateWindowExA(0, "RichEdit20A", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 28, 760, 292, log_hwnd, (HMENU)ID_RICH_LOG, hinstance, nullptr);
    }
    if (rich_hwnd)
    {
        SendMessage(rich_hwnd, EM_SETBKGNDCOLOR, 0, (LPARAM)RGB(18, 18, 18));
        SendMessage(rich_hwnd, EM_SETLIMITTEXT, (WPARAM)0x7FFFFFFF, 0);
        CHARFORMAT2A cf = {};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR;
        cf.yHeight = 180;
        cf.crTextColor = RGB(200, 200, 200);
        strcpy_s(cf.szFaceName, "Consolas");
        SendMessage(rich_hwnd, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    }
    HWND btn = CreateWindowExA(0, "BUTTON", "Clear", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 760 - 72, 3, 68, 22, log_hwnd, (HMENU)ID_BTN_CLEAR, hinstance, nullptr);
    (void)btn;
    SendMessage(log_hwnd, WM_SIZE, 0, MAKELPARAM(760, 320));
}
void _log_quit()
{
    if (log_hwnd)
    {
        DestroyWindow(log_hwnd);
        log_hwnd = nullptr;
        rich_hwnd = nullptr;
    }
    if (richedit_lib)
    {
        FreeLibrary(richedit_lib);
        richedit_lib = nullptr;
    }
}
void show_log_window(bool show)
{
    if (!log_hwnd)
        return;
    ShowWindow(log_hwnd, show ? SW_SHOW : SW_HIDE);
    if (show)
        _rebuild();
}
bool log_window_visib()
{
    return log_hwnd && IsWindowVisible(log_hwnd);
}
void gui_log_pmp()
{
    if (!log_hwnd || !rich_hwnd)
        return;
    if (!IsWindowVisible(log_hwnd))
        return;
    _rebuild();
}

// end