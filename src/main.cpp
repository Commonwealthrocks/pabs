// main.cpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include <windows.h>
#include <cstdio>
#include "ui.hpp"
#include "logger.hpp"
#include "config.hpp"
#include "error_handler.hpp"
#include "__drives.hpp"
static ID3D11Device *pd3d_device = nullptr;
static ID3D11DeviceContext *dev_context = nullptr;
static IDXGISwapChain *swap_chain = nullptr;
static ID3D11RenderTargetView *mrtv = nullptr;
bool create_device(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT create_device_flags = 0;
    D3D_FEATURE_LEVEL feature_level;
    const D3D_FEATURE_LEVEL feature_level_array[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, create_device_flags, feature_level_array, 2, D3D11_SDK_VERSION, &sd, &swap_chain, &pd3d_device, &feature_level, &dev_context);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, create_device_flags, feature_level_array, 2, D3D11_SDK_VERSION, &sd, &swap_chain, &pd3d_device, &feature_level, &dev_context);
    if (res != S_OK)
        return false;
    ID3D11Texture2D *p_back_buffer;
    swap_chain->GetBuffer(0, IID_PPV_ARGS(&p_back_buffer));
    pd3d_device->CreateRenderTargetView(p_back_buffer, nullptr, &mrtv);
    p_back_buffer->Release();
    return true;
}
void cleanup_d3()
{
    if (mrtv)
    {
        mrtv->Release();
        mrtv = nullptr;
    }
    if (swap_chain)
    {
        swap_chain->Release();
        swap_chain = nullptr;
    }
    if (dev_context)
    {
        dev_context->Release();
        dev_context = nullptr;
    }
    if (pd3d_device)
    {
        pd3d_device->Release();
        pd3d_device = nullptr;
    }
}
void create_rendertt()
{
    ID3D11Texture2D *p_back_buffer;
    swap_chain->GetBuffer(0, IID_PPV_ARGS(&p_back_buffer));
    pd3d_device->CreateRenderTargetView(p_back_buffer, nullptr, &mrtv);
    p_back_buffer->Release();
}
void cleanup_rendertt()
{
    if (mrtv)
    {
        mrtv->Release();
        mrtv = nullptr;
    }
}
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg)
    {
    case WM_SIZE:
        if (pd3d_device != nullptr && wParam != SIZE_MINIMIZED)
        {
            cleanup_rendertt();
            swap_chain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            create_rendertt();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    case WM_DROPFILES:
        drag_drop_handle((HDROP)wParam);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    err_init();
    char exe_dir[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe_dir, MAX_PATH);
    char *sep = strrchr(exe_dir, '\\');
    if (sep)
        *(sep + 1) = '\0';
    char cache_dir[MAX_PATH];
    snprintf(cache_dir, MAX_PATH, "%sassets\\__cache\\no", exe_dir);
    CreateDirectoryA(cache_dir, NULL);
    char log_path[MAX_PATH];
    snprintf(log_path, MAX_PATH, "%sassets\\__cache\\no\\pabs.log", exe_dir);
    log_init(log_path);
    _log_init(hInstance, nullptr);
    LOG_INFO("PABS starting up");
    LOG_INFO("Current app version: v0.2a");
    drive_info drives[PABS_MAX_DRIVES];
    int drive_count = drives_enum(drives);
    LOG_INFOF("Found %d optical drive(s)", drive_count);
    for (int i = 0; i < drive_count; ++i)
    {
        LOG_INFOF("  %s - %s %s [%s] disc: %s", drives[i].path, drives[i].vendor, drives[i].product, drives_type_str(drives[i].type), drives[i].disc_present ? "yeah" : "nah");
    }
    HICON hIcon = LoadIconW(hInstance, L"IDI_APPICON");
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, hIcon, nullptr, nullptr, nullptr, L"PABS", hIcon};
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"PABS - PYROFOREVER's actual burning software", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 100, 100, 620, 580, nullptr, nullptr, wc.hInstance, nullptr);
    BOOL dark = TRUE;
    HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
    if (!dwm)
        dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm)
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
        auto fn = reinterpret_cast<HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD)>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
#pragma GCC diagnostic pop
        if (fn)
            fn(hwnd, 20, &dark, sizeof(dark));
    }
    char icon_path[MAX_PATH];
    snprintf(icon_path, MAX_PATH, "%sassets\\imgs\\icons\\pabs.ico", exe_dir);
    HICON hIconBig = (HICON)LoadImageA(nullptr, icon_path, IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
    HICON hIconSmall = (HICON)LoadImageA(nullptr, icon_path, IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
    if (hIconBig)
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    if (hIconSmall)
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
    if (!create_device(hwnd))
    {
        cleanup_d3();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ::ShowWindow(hwnd, nShowCmd);
    ::UpdateWindow(hwnd);
    ::DragAcceptFiles(hwnd, TRUE);
    LOG_OK("Window and renderer up");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    static char ini_path[MAX_PATH];
    snprintf(ini_path, MAX_PATH, "%sassets\\__cache\\no\\imgui.ini", exe_dir);
    io.IniFilename = ini_path;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // c; https://github.com/ocornut/imgui/wiki/Multi-Viewports
    io.ConfigWindowsMoveFromTitleBarOnly = true;        // m; Gem alert.
    io.Fonts->AddFontDefault();
    static const ImWchar for_quotes_in_range[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x0180, 0x024F, 0};
    ImFontConfig fc;
    fc.OversampleH = 2;
    fc.OversampleV = 1;
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, &fc, for_quotes_in_range))
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\tahoma.ttf", 16.0f, &fc, for_quotes_in_range);
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(pd3d_device, dev_context);
    ui_init(pd3d_device);
    poll_drives_start(drives, drive_count);
    bool done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        gui_render(drives, drive_count);
        ImGui::Render();
        const float clear_colour_wa[4] = {0.15f, 0.15f, 0.15f, 1.0f};
        dev_context->OMSetRenderTargets(1, &mrtv, nullptr);
        dev_context->ClearRenderTargetView(mrtv, clear_colour_wa);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swap_chain->Present(1, 0);
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            dev_context->OMSetRenderTargets(1, &mrtv, nullptr);
        }
    }
    gui_shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_d3();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    _log_quit();
    return 0;
}

// end