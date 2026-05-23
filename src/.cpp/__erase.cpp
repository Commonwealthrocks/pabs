// __erase.cpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "__erase.hpp"
#include "__imapi_com.hpp"
#include "logger.hpp"
#include <windows.h>
#include <oleauto.h>
#include <ocidl.h>
#include <olectl.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <string>
#include <chrono>
static const CLSID clsid_msftdiscformattoerase = {0x2735412B, 0x7F64, 0x5B0F, {0x8F, 0x00, 0x5D, 0x77, 0xAF, 0xBE, 0x26, 0x1E}};
static const IID iid_discformattoeraseevents = {0x2735413A, 0x7F64, 0x5B0F, {0x8F, 0x00, 0x5D, 0x77, 0xAF, 0xBE, 0x26, 0x1E}};
class EraseEventSink : public IDispatch
{
private:
    LONG m_cref;
    erase_context *m_ctx;

public:
    EraseEventSink(erase_context *ctx) : m_cref(1), m_ctx(ctx) {}
    virtual ~EraseEventSink() {}
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDispatch) || IsEqualIID(riid, iid_discformattoeraseevents))
        {
            *ppv = static_cast<IDispatch *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG)
    AddRef() override { return InterlockedIncrement(&m_cref); }
    STDMETHODIMP_(ULONG)
    Release() override
    {
        ULONG ulRef = InterlockedDecrement(&m_cref);
        if (ulRef == 0)
            delete this;
        return ulRef;
    }
    STDMETHODIMP GetTypeInfoCount(UINT *pctinfo) override
    {
        *pctinfo = 0;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetTypeInfo(UINT iTInfo, LCID lcid, ITypeInfo **ppTInfo) override { return E_NOTIMPL; }
    STDMETHODIMP GetIDsOfNames(REFIID riid, LPOLESTR *rgszNames, UINT cNames, LCID lcid, DISPID *rgDispId) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr) override
    {
        if (dispIdMember == 0x200)
        {
            if (pDispParams->cArgs >= 3)
            {
                LONG estimatedTotal = 0;
                if (pDispParams->rgvarg[0].vt == VT_I4)
                    estimatedTotal = pDispParams->rgvarg[0].lVal;
                LONG elapsed = 0;
                if (pDispParams->rgvarg[1].vt == VT_I4)
                    elapsed = pDispParams->rgvarg[1].lVal;
                IDispatch *pEraser = nullptr;
                if (pDispParams->rgvarg[2].vt == VT_DISPATCH)
                    pEraser = pDispParams->rgvarg[2].pdispVal;
                if (m_ctx->abort_requested && pEraser)
                {
                    imapi_set_stat(m_ctx, "Aborting...");
                }
                m_ctx->elapsed_seconds = elapsed;
                m_ctx->estimated_total_seconds = estimatedTotal;
            }
        }
        return S_OK;
    }
};
static void erase_thread_func(erase_context *ctx)
{
    LOG_INFO("Erase starting");
    imapi_set_stat(ctx, "Initializing...");
    if (!ctx->drive)
    {
        LOG_ERR("Invalid drive context for erase.");
        imapi_set_stat(ctx, "[ ERROR ] Setup failed");
        ctx->is_running = false;
        return;
    }
    if (ctx->drive->valid)
    { // c; close raw should be here, now below our coinit
        drives_close(*ctx->drive);
    }
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        LOG_ERRF("CoInitializeEx[...] failed: 0x%08X", hr);
        imapi_set_stat(ctx, "[ ERROR ] COM init");
        ctx->is_running = false;
        return;
    }
    IDispatch *pDiscMaster = nullptr;
    IDispatch *pRecorder = nullptr;
    IDispatch *pEraser = nullptr;
    EraseEventSink *pEventSink = nullptr;
    DWORD dwCookie = 0;
    imapi_set_stat(ctx, "Searching for drive...");
    hr = imapi_open_disc_master(&pDiscMaster);
    if (FAILED(hr))
    {
        LOG_ERR("Failed to create MsftDiscMaster2");
        goto cleanup;
    }
    hr = imapi_find_recorder(pDiscMaster, ctx->drive->info.path, &pRecorder);
    if (FAILED(hr) || !pRecorder)
    {
        LOG_ERR("Could not find IMAPIv2 recorder matching selected drive letter.");
        imapi_set_stat(ctx, "[ ERROR ] Drive not found");
        goto cleanup;
    }
    imapi_set_stat(ctx, "Preparing to erase...");
    hr = CoCreateInstance(clsid_msftdiscformattoerase, nullptr, CLSCTX_ALL, IID_IDispatch, (void **)&pEraser);
    if (FAILED(hr))
    {
        LOG_ERR("Failed to create MsftDiscFormat2Erase");
        goto cleanup;
    }
    {
        VARIANT vRecArg;
        VariantInit(&vRecArg);
        vRecArg.vt = VT_DISPATCH;
        vRecArg.pdispVal = pRecorder;
        hr = imapi_dispatch_put(pEraser, L"Recorder", &vRecArg);
        if (FAILED(hr))
        {
            LOG_ERR("Failed to attach recorder to eraser");
            goto cleanup;
        }
        VARIANT vClient;
        VariantInit(&vClient);
        vClient.vt = VT_BSTR;
        vClient.bstrVal = SysAllocString(L"erase engine");
        imapi_dispatch_put(pEraser, L"ClientName", &vClient);
        VariantClear(&vClient);
        VARIANT vFull;
        VariantInit(&vFull);
        vFull.vt = VT_BOOL;
        vFull.boolVal = ctx->options.full_erase ? VARIANT_TRUE : VARIANT_FALSE;
        imapi_dispatch_put(pEraser, L"FullErase", &vFull);
    }
    pEventSink = new EraseEventSink(ctx);
    if (!imapi_connpt_advise(pEraser, iid_discformattoeraseevents, pEventSink, &dwCookie))
    {
        LOG_WARN("Failed to setup progress events; progress won't update");
    }
    if (ctx->options.full_erase)
    {
        imapi_set_stat(ctx, "Erasing disc (full)...");
    }
    else
    {
        imapi_set_stat(ctx, "Erasing disc (quick)...");
    }
    LOG_INFO("Calling EraseMedia");
    hr = imapi_dispatch_call(pEraser, L"EraseMedia", DISPATCH_METHOD, nullptr, nullptr, 0);
    if (SUCCEEDED(hr))
    {
        LOG_OK("Erase finished successfully");
        imapi_set_stat(ctx, "Finished");
        if (ctx->options.eject_when_done && pRecorder)
        {
            LOG_INFO("Ejecting disc");
            imapi_dispatch_call(pRecorder, L"EjectMedia", DISPATCH_METHOD, nullptr, nullptr, 0);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::wstring wDriveStr = imapi_u8_to_w(ctx->drive->info.path);
            SHChangeNotify(SHCNE_MEDIAINSERTED, SHCNF_PATHW, wDriveStr.c_str(), NULL);
        }
    }
    else if (hr == E_ABORT || ctx->abort_requested || hr == (HRESULT)0xC0AA020A)
    {
        LOG_WARN("Erase aborted by user");
        imapi_set_stat(ctx, "Aborted");
    }
    else
    {
        LOG_ERRF("Erase failed with HRESULT: 0x%08X", hr);
        imapi_set_stat(ctx, "[ ERROR ] Erase failed");
    }
cleanup:
    if (pEventSink && dwCookie && pEraser)
        imapi_connpt_rm(pEraser, iid_discformattoeraseevents, dwCookie);
    if (pEventSink)
        pEventSink->Release();
    if (pEraser)
        pEraser->Release();
    if (pRecorder)
        pRecorder->Release();
    if (pDiscMaster)
        pDiscMaster->Release();
    CoUninitialize();
    LOG_INFO("IMAPIv2 shutting down");
    if (ctx->drive && ctx->drive->valid)
    {
        drives_close(*ctx->drive);
    }
    ctx->is_running = false;
}
void erase_init(erase_context &ctx, drive_handle *drive, const erase_options &options)
{
    if (ctx.worker_thread)
    {
        if (ctx.worker_thread->joinable())
            ctx.worker_thread->join();
        delete ctx.worker_thread;
        ctx.worker_thread = nullptr;
    }
    ctx.drive = drive;
    ctx.options = options;
    ctx.is_running = false;
    ctx.abort_requested = false;
    ctx.elapsed_seconds = 0;
    ctx.estimated_total_seconds = 0;
    {
        std::lock_guard<std::mutex> lk(ctx.status_mutex);
        snprintf(ctx.status_text, sizeof(ctx.status_text), "Idle");
    }
}
bool erase_start(erase_context &ctx)
{
    if (ctx.is_running)
        return false;
    ctx.is_running = true;
    ctx.abort_requested = false;
    ctx.elapsed_seconds = 0;
    ctx.estimated_total_seconds = 0;
    {
        std::lock_guard<std::mutex> lk(ctx.status_mutex);
        snprintf(ctx.status_text, sizeof(ctx.status_text), "Starting...");
    }
    if (ctx.worker_thread && ctx.worker_thread->joinable())
    {
        ctx.worker_thread->join();
        delete ctx.worker_thread;
    }
    ctx.worker_thread = new std::thread(erase_thread_func, &ctx);
    return true;
}
void erase_abort(erase_context &ctx)
{
    if (ctx.is_running)
    {
        ctx.abort_requested = true;
        LOG_WARN("Abort requested");
    }
}
void erase_progress(const erase_context &ctx, uint32_t &out_elapsed_sec, uint32_t &out_estimated_total_sec)
{
    out_elapsed_sec = ctx.elapsed_seconds.load();
    out_estimated_total_sec = ctx.estimated_total_seconds.load();
}

// end