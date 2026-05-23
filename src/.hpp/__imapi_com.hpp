// __imapi_com.hpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
#include <windows.h>
#include <oleauto.h>
#include <mutex>
#include <cstdio>
#include <string>
extern const CLSID imapi_clsid_disc_master;
extern const CLSID imapi_clsid_disc_recorder;
extern const IID imapi_iid_disc_data_events;
#define imapi_set_stat(ctx, txt)                                               \
    do                                                                         \
    {                                                                          \
        std::lock_guard<std::mutex> _imapi_slk((ctx)->status_mutex);           \
        snprintf((ctx)->status_text, sizeof((ctx)->status_text), "%s", (txt)); \
    } while (0)
std::wstring imapi_u8_to_w(const std::string &u8);
HRESULT imapi_dispatch_call(IDispatch *d, const wchar_t *name, WORD flags, VARIANT *ret, VARIANT *args, UINT n_args);
HRESULT imapi_dispatch_get(IDispatch *d, const wchar_t *name, VARIANT *out);
HRESULT imapi_dispatch_put(IDispatch *d, const wchar_t *name, VARIANT *arg);
HRESULT imapi_open_disc_master(IDispatch **out);
HRESULT imapi_find_recorder(IDispatch *disc_master, const std::string &drive_vol_path, IDispatch **out_recorder);
bool imapi_connpt_advise(IDispatch *src, REFIID conn_pt_iid, IUnknown *sink, DWORD *cookie);
void imapi_connpt_rm(IDispatch *src, REFIID conn_pt_iid, DWORD cookie);

// end