// ui.hpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
#include "__drives.hpp"
#include <d3d11.h>
#include <windows.h>
bool ui_init(ID3D11Device *device); // c; yo do not change "ID3D11Device" at ALL
void gui_shutdown();                // m; ImGui internal?
                                    // c; yeah.
                                    // m; Moron.
void gui_render(drive_info drives[], int drive_count);
void _log_init(HINSTANCE hinstance, HWND parent);
void _log_quit();
void show_log_window(bool show);
void gui_log_pmp();
bool log_window_visib();
void drag_drop_handle(HDROP hDrop);
void poll_drives_start(drive_info *drives, int drive_count);

// end