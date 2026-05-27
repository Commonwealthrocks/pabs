// __burn.cpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "__burn.hpp"
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
const CLSID clsid_msftdiscformattodata = {0x2735412A, 0x7F64, 0x5B0F, {0x8F, 0x00, 0x5D, 0x77, 0xAF, 0xBE, 0x26, 0x1E}};
const CLSID clsid_msftdiscformattoerase = {0x2735412B, 0x7F64, 0x5B0F, {0x8F, 0x00, 0x5D, 0x77, 0xAF, 0xBE, 0x26, 0x1E}};
enum IMAPI_F2_DATA_WRITE_ACTION
{
    IMAPI_F2_DATA_WRITE_ACTION_VALIDATING_MEDIA = 0,
    IMAPI_F2_DATA_WRITE_ACTION_FORMATTING_MEDIA = 1,
    IMAPI_F2_DATA_WRITE_ACTION_INITIALIZING_HARDWARE = 2,
    IMAPI_F2_DATA_WRITE_ACTION_CALIBRATING_POWER = 3,
    IMAPI_F2_DATA_WRITE_ACTION_WRITING_DATA = 4,
    IMAPI_F2_DATA_WRITE_ACTION_FINALIZATION = 5,
    IMAPI_F2_DATA_WRITE_ACTION_COMPLETED = 6,
    IMAPI_F2_DATA_WRITE_ACTION_VERIFYING = 7
};
class burn_event_sink : public IDispatch
{
private:
    LONG m_cref;
    burn_context *m_ctx;
    uint64_t m_last_bytes_written;
    std::chrono::steady_clock::time_point m_last_time;
    bool m_is_finalizing;
    uint32_t m_finalizing_start;

public:
    burn_event_sink(burn_context *ctx) : m_cref(1), m_ctx(ctx), m_last_bytes_written(0), m_is_finalizing(false), m_finalizing_start(0)
    {
        m_last_time = std::chrono::steady_clock::now();
    }
    virtual ~burn_event_sink() {}
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDispatch) || IsEqualIID(riid, imapi_iid_disc_data_events))
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
            if (pDispParams->cArgs >= 2)
            {
                IDispatch *pProgress = pDispParams->rgvarg[0].pdispVal;
                IDispatch *pFormat2Data = pDispParams->rgvarg[1].pdispVal;
                if (m_ctx->abort_requested)
                {
                    if (pFormat2Data)
                        imapi_dispatch_call(pFormat2Data, L"CancelWrite", DISPATCH_METHOD, nullptr, nullptr, 0);
                    imapi_set_stat(m_ctx, "Aborting...");
                }
                if (pProgress)
                {
                    VARIANT vAction;
                    VariantInit(&vAction);
                    if (SUCCEEDED(imapi_dispatch_get(pProgress, L"CurrentAction", &vAction)))
                    {
                        if (vAction.vt != VT_I4)
                            return S_OK;
                        LONG action = vAction.lVal;
                        VARIANT vElapsed;
                        VariantInit(&vElapsed);
                        if (SUCCEEDED(imapi_dispatch_get(pProgress, L"ElapsedTime", &vElapsed)) && vElapsed.vt == VT_I4)
                        {
                            m_ctx->elapsed_seconds = vElapsed.lVal;
                        }
                        if (action == IMAPI_F2_DATA_WRITE_ACTION_WRITING_DATA || action == IMAPI_F2_DATA_WRITE_ACTION_FINALIZATION)
                        {
                            if (action == IMAPI_F2_DATA_WRITE_ACTION_WRITING_DATA)
                                imapi_set_stat(m_ctx, "Burning...");
                            else
                                imapi_set_stat(m_ctx, "Finalizing...");
                            VARIANT vTotal, vLast, vStart;
                            VariantInit(&vTotal);
                            VariantInit(&vLast);
                            VariantInit(&vStart);
                            imapi_dispatch_get(pProgress, L"SectorCount", &vTotal);
                            imapi_dispatch_get(pProgress, L"LastWrittenLba", &vLast);
                            imapi_dispatch_get(pProgress, L"StartLba", &vStart);
                            LONG written = 0;
                            if (action == IMAPI_F2_DATA_WRITE_ACTION_FINALIZATION)
                            {
                                if (!m_is_finalizing)
                                {
                                    m_is_finalizing = true;
                                    m_finalizing_start = m_ctx->elapsed_seconds;
                                }
                                float larp_progress = (float)(m_ctx->elapsed_seconds - m_finalizing_start) / 15.0f * 100.0f; // guess 15s avg
                                if (larp_progress > 99.0f)
                                    larp_progress = 99.0f;
                                m_ctx->progress_percent = larp_progress;
                                m_ctx->write_speed_mbps = 0.0f;
                            }
                            else if (vTotal.vt == VT_I4 && vLast.vt == VT_I4 && vStart.vt == VT_I4 && vTotal.lVal > 0)
                            {
                                written = vLast.lVal - vStart.lVal;
                                if (written < 0)
                                    written = 0;
                                m_ctx->progress_percent = (float)written / (float)vTotal.lVal * 100.0f;
                            }
                            uint64_t current_bytes = (uint64_t)written * 2048;
                            auto now = std::chrono::steady_clock::now();
                            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_time).count();
                            if (dt >= 1000)
                            {
                                if (current_bytes >= m_last_bytes_written)
                                {
                                    uint64_t diff = current_bytes - m_last_bytes_written;
                                    m_ctx->write_speed_mbps = (float)diff / 1024.0f / 1024.0f / (dt / 1000.0f);
                                }
                                m_last_bytes_written = current_bytes;
                                m_last_time = now;
                            }
                        } // m; Wow this API really is more simpler than SCSI!
                        else if (action == IMAPI_F2_DATA_WRITE_ACTION_FORMATTING_MEDIA)
                        {
                            imapi_set_stat(m_ctx, "Formatting...");
                        }
                        else if (action == IMAPI_F2_DATA_WRITE_ACTION_VALIDATING_MEDIA)
                        {
                            imapi_set_stat(m_ctx, "Validating...");
                        }
                        else if (action == IMAPI_F2_DATA_WRITE_ACTION_CALIBRATING_POWER)
                        {
                            imapi_set_stat(m_ctx, "Calibrating laser..."); // m; This sounds stupid
                                                                           // c; well fuck you too then 😂😂
                        }
                    }
                }
            }
        }
        return S_OK;
    }
};
class PatchedIStream : public IStream
{
private:
    LONG m_cref;
    IStream *m_base;
    uint32_t m_pvd_sector;
    uint32_t m_svd_sector;
    char m_label_ascii[32];
    char m_label_utf16be[32];
    PatchedIStream(IStream *base, uint32_t pvd, uint32_t svd) : m_cref(1), m_base(base), m_pvd_sector(pvd), m_svd_sector(svd)
    {
        m_base->AddRef();
    }
public:
    PatchedIStream(IStream *base, const char *label, uint32_t pvd, uint32_t svd) : m_cref(1), m_base(base), m_pvd_sector(pvd), m_svd_sector(svd)
    {
        m_base->AddRef();
        memset(m_label_ascii, ' ', 32);
        memset(m_label_utf16be, 0, 32);
        size_t len = strlen(label);
        if (len > 32)
            len = 32;
        for (size_t i = 0; i < len; ++i)
        {
            m_label_ascii[i] = label[i];
            m_label_utf16be[i * 2] = 0;
            m_label_utf16be[i * 2 + 1] = label[i];
        }
        for (size_t i = len; i < 16; ++i)
        {
            m_label_utf16be[i * 2] = 0;
            m_label_utf16be[i * 2 + 1] = ' ';
        }
    }
    virtual ~PatchedIStream()
    {
        m_base->Release();
    }
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IStream) || IsEqualIID(riid, IID_ISequentialStream))
        {
            *ppv = static_cast<IStream *>(this);
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
        ULONG r = InterlockedDecrement(&m_cref);
        if (r == 0)
            delete this;
        return r;
    }
    STDMETHODIMP Read(void *pv, ULONG cb, ULONG *pcbRead) override
    {
        LARGE_INTEGER zero = {};
        ULARGE_INTEGER pos = {};
        m_base->Seek(zero, STREAM_SEEK_CUR, &pos);
        HRESULT hr = m_base->Read(pv, cb, pcbRead);
        if (FAILED(hr) || !pcbRead || *pcbRead == 0)
            return hr;
        uint64_t start_offset = pos.QuadPart;
        uint64_t end_offset = start_offset + *pcbRead;
        auto apply_patch = [&](uint32_t sector, const char *patch)
        {
            if (sector == 0)
                return;
            uint64_t patch_start = sector * 2048ULL + 40;
            uint64_t patch_end = patch_start + 32;
            if (start_offset < patch_end && end_offset > patch_start)
            {
                uint64_t overlap_start = (start_offset > patch_start) ? start_offset : patch_start;
                uint64_t overlap_end = (end_offset < patch_end) ? end_offset : patch_end;
                uint64_t buf_offset = overlap_start - start_offset;
                uint64_t patch_offset = overlap_start - patch_start;
                memcpy((uint8_t *)pv + buf_offset, patch + patch_offset, overlap_end - overlap_start);
            }
        };
        apply_patch(m_pvd_sector, m_label_ascii);
        apply_patch(m_svd_sector, m_label_utf16be);
        return hr;
    }
    STDMETHODIMP Write(const void *pv, ULONG cb, ULONG *pcbWritten) override { return E_NOTIMPL; }
    STDMETHODIMP Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition) override
    {
        return m_base->Seek(dlibMove, dwOrigin, plibNewPosition);
    }
    STDMETHODIMP SetSize(ULARGE_INTEGER libNewSize) override { return E_NOTIMPL; }
    STDMETHODIMP CopyTo(IStream *pstm, ULARGE_INTEGER cb, ULARGE_INTEGER *pcbRead, ULARGE_INTEGER *pcbWritten) override { return E_NOTIMPL; }
    STDMETHODIMP Commit(DWORD grfCommitFlags) override { return E_NOTIMPL; }
    STDMETHODIMP Revert() override { return E_NOTIMPL; }
    STDMETHODIMP LockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) override { return E_NOTIMPL; }
    STDMETHODIMP UnlockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) override { return E_NOTIMPL; }
    STDMETHODIMP Stat(STATSTG *pstatstg, DWORD grfStatFlag) override { return m_base->Stat(pstatstg, grfStatFlag); }
    STDMETHODIMP Clone(IStream **ppstm) override
    {
        IStream *base_clone = nullptr;
        HRESULT hr = m_base->Clone(&base_clone);
        if (FAILED(hr))
            return hr;
        PatchedIStream *wrapper = new PatchedIStream(base_clone, m_pvd_sector, m_svd_sector);
        memcpy(wrapper->m_label_ascii, m_label_ascii, 32);
        memcpy(wrapper->m_label_utf16be, m_label_utf16be, 32);
        *ppstm = wrapper;
        base_clone->Release();
        return S_OK;
    }
};

static void burn_thread_func(burn_context *ctx)
{
    ctx->is_running = true;
    LOG_INFO("IMAPIv2 starting");
    imapi_set_stat(ctx, "Initializing...");
    if (!ctx->drive || !ctx->image || !ctx->image->is_valid)
    {
        LOG_ERR("Invalid drive or image context.");
        imapi_set_stat(ctx, "[ ERROR ] Setup failed");
        ctx->is_running = false;
        return;
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
    IDispatch *pDataWriter = nullptr;
    IStream *pStream = nullptr;
    burn_event_sink *pEventSink = nullptr;
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
        LOG_ERR("Could not find IMAPIv2 recorder matching selected drive letter");
        imapi_set_stat(ctx, "[ ERROR ] Drive not found");
        goto cleanup;
    }
    imapi_set_stat(ctx, "Preparing media...");
    hr = CoCreateInstance(clsid_msftdiscformattodata, nullptr, CLSCTX_ALL, IID_IDispatch, (void **)&pDataWriter);
    if (FAILED(hr))
    {
        LOG_ERR("Failed to create MsftDiscFormat2Data");
        goto cleanup;
    }
    {
        VARIANT vRecArg;
        VariantInit(&vRecArg);
        vRecArg.vt = VT_DISPATCH;
        vRecArg.pdispVal = pRecorder;
        hr = imapi_dispatch_put(pDataWriter, L"Recorder", &vRecArg);
        if (FAILED(hr))
        {
            LOG_ERR("Failed to attach recorder to format");
            goto cleanup;
        }
        VARIANT vClient;
        VariantInit(&vClient);
        vClient.vt = VT_BSTR;
        vClient.bstrVal = SysAllocString(L"burn engine");
        imapi_dispatch_put(pDataWriter, L"ClientName", &vClient);
        VariantClear(&vClient);
        {
            VARIANT vSpeed;
            VariantInit(&vSpeed);
            vSpeed.vt = VT_I4;
            if (ctx->options.speed_multiplier <= 0)
            {
                vSpeed.lVal = -1;
            }
            else
            {
                vSpeed.lVal = ctx->options.speed_multiplier * 1385;
            }
            imapi_dispatch_put(pDataWriter, L"RequestedWriteSpeed", &vSpeed);
        }
        {
            VARIANT vBufe;
            VariantInit(&vBufe);
            vBufe.vt = VT_BOOL;
            vBufe.boolVal = VARIANT_TRUE;
            imapi_dispatch_put(pDataWriter, L"BufferUnderrunFreeEnabled", &vBufe);
        }
    }
    imapi_set_stat(ctx, "Opening image stream...");
    {
        VARIANT varBlank;
        VariantInit(&varBlank);
        if (SUCCEEDED(imapi_dispatch_get(pDataWriter, L"MediaHeuristicallyBlank", &varBlank)) && varBlank.vt == VT_BOOL)
        {
            if (varBlank.boolVal == VARIANT_FALSE)
            {
                LOG_ERR("Disc is not rewriteable and contains data; please insert a blank disc.");
                imapi_set_stat(ctx, "[ ERROR ] Disc not blank");
                goto cleanup;
            }
        }
        VariantClear(&varBlank);
        std::wstring wPath = imapi_u8_to_w(ctx->image->path);
        hr = SHCreateStreamOnFileW(wPath.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, &pStream);
        if (FAILED(hr))
        {
            LOG_ERR("Failed to open ISO file as COM stream.");
            imapi_set_stat(ctx, "[ ERROR ] Open ISO"); // c; error is pretty unclear; why?
            goto cleanup;
        }

        if (!ctx->image->has_udf && !ctx->options.volume_label.empty() && ctx->options.volume_label != ctx->image->volume_id)
        {
            LOG_INFOF("Patching ISO stream volume label to '%s'", ctx->options.volume_label.c_str());
            uint32_t svd = ctx->image->has_joliet ? ctx->image->joliet_svd_sector : 0;
            uint32_t pvd = ctx->image->has_iso9660 ? 16 : 0;
            if (!ctx->image->has_iso9660 && !ctx->image->has_joliet)
                pvd = 16; // default
            IStream *wrapper = new PatchedIStream(pStream, ctx->options.volume_label.c_str(), pvd, svd);
            pStream->Release();
            pStream = wrapper;
        }
    }
    pEventSink = new burn_event_sink(ctx);
    if (!imapi_connpt_advise(pDataWriter, imapi_iid_disc_data_events, pEventSink, &dwCookie))
    {
        LOG_WARN("Failed to setup progress event, progress updates won't move a bit");
    }
    imapi_set_stat(ctx, "Starting burn...");
    LOG_INFO("Starting IMAPIv2");
    {
        VARIANT vStreamArg;
        VariantInit(&vStreamArg);
        vStreamArg.vt = VT_UNKNOWN;
        vStreamArg.punkVal = pStream;
        hr = imapi_dispatch_call(pDataWriter, L"Write", DISPATCH_METHOD, nullptr, &vStreamArg, 1);
    }
    if (SUCCEEDED(hr))
    {
        LOG_OK("Burn finished; quick, wasn't it?");
        imapi_set_stat(ctx, "Quick, wasn't it?");
        ctx->progress_percent = 100.0f;
        if (ctx->options.eject_when_done && !ctx->options.verify_after_burn && pRecorder)
        {
            LOG_INFO("Ejecting disc");
            imapi_dispatch_call(pRecorder, L"EjectMedia", DISPATCH_METHOD, nullptr, nullptr, 0);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::wstring wDriveStr = imapi_u8_to_w(ctx->drive->info.path);
            SHChangeNotify(SHCNE_MEDIAREMOVED, SHCNF_PATHW, wDriveStr.c_str(), NULL);
        }
    }
    else if (hr == E_ABORT || ctx->abort_requested || hr == (HRESULT)0xC0AA020A)
    {
        LOG_WARN("Burn aborted by user");
        imapi_set_stat(ctx, "Aborted");
    }
    else
    {
        LOG_ERRF("Burn failed with HRESULT: 0x%08X", hr);
        imapi_set_stat(ctx, "[ ERROR ] Burn failed");
    }
cleanup:
    if (pEventSink && dwCookie && pDataWriter)
        imapi_connpt_rm(pDataWriter, imapi_iid_disc_data_events, dwCookie);
    if (pEventSink)
        pEventSink->Release();
    if (pStream)
        pStream->Release();
    if (pDataWriter)
        pDataWriter->Release();
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
void burn_init(burn_context &ctx, drive_handle *drive, image_context *image, const burn_options &options)
{
    if (ctx.worker_thread)
    {
        if (ctx.worker_thread->joinable())
            ctx.worker_thread->join();
        delete ctx.worker_thread;
        ctx.worker_thread = nullptr;
    }
    ctx.drive = drive;
    ctx.image = image;
    ctx.options = options;
    ctx.is_running = false;
    ctx.abort_requested = false;
    ctx.progress_percent = 0.0f;
    ctx.write_speed_mbps = 0.0f;
    ctx.elapsed_seconds = 0;
    {
        std::lock_guard<std::mutex> lk(ctx.status_mutex);
        snprintf(ctx.status_text, sizeof(ctx.status_text), "Idle");
    }
    ctx.worker_thread = nullptr;
}
bool burn_start(burn_context &ctx)
{
    if (ctx.is_running)
        return false;
    ctx.abort_requested = false;
    ctx.progress_percent = 0.0f;
    ctx.write_speed_mbps = 0.0f;
    ctx.elapsed_seconds = 0;
    {
        std::lock_guard<std::mutex> lk(ctx.status_mutex);
        snprintf(ctx.status_text, sizeof(ctx.status_text), "Starting...");
    }
    if (ctx.worker_thread && ctx.worker_thread->joinable())
    {
        ctx.worker_thread->join();
        delete ctx.worker_thread;
    }
    ctx.worker_thread = new std::thread(burn_thread_func, &ctx);
    return true;
}
void burn_abort(burn_context &ctx)
{
    if (ctx.is_running)
    {
        ctx.abort_requested = true;
        LOG_WARN("burn_abort - abort requested");
    }
}
void burn_progress(const burn_context &ctx, float &out_percent, float &out_mbps, uint32_t &out_elapsed_sec)
{
    out_percent = ctx.progress_percent.load();
    out_mbps = ctx.write_speed_mbps.load();
    out_elapsed_sec = ctx.elapsed_seconds.load();
}
bool burn_is_rw(const char *drive_path)
{
    bool is_rw = false;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool co_init = SUCCEEDED(hr);
    IDispatch *pDiscMaster = nullptr;
    IDispatch *pRecorder = nullptr;
    IDispatch *pEraser = nullptr;
    hr = imapi_open_disc_master(&pDiscMaster);
    if (FAILED(hr))
        goto cleanup;
    hr = imapi_find_recorder(pDiscMaster, std::string(drive_path ? drive_path : ""), &pRecorder);
    if (SUCCEEDED(hr) && pRecorder)
    {
        if (SUCCEEDED(CoCreateInstance(clsid_msftdiscformattoerase, nullptr, CLSCTX_ALL, IID_IDispatch, (void **)&pEraser)))
        {
            VARIANT vRecArg;
            VariantInit(&vRecArg);
            vRecArg.vt = VT_DISPATCH;
            vRecArg.pdispVal = pRecorder;
            if (SUCCEEDED(imapi_dispatch_put(pEraser, L"Recorder", &vRecArg)))
            {
                VARIANT vSupported;
                VariantInit(&vSupported);
                if (SUCCEEDED(imapi_dispatch_get(pEraser, L"IsCurrentMediaSupported", &vSupported)))
                {
                    if (vSupported.vt == VT_BOOL && vSupported.boolVal == VARIANT_TRUE)
                    {
                        is_rw = true;
                    }
                }
            }
            pEraser->Release();
        }
        pRecorder->Release();
    }
cleanup:
    if (pDiscMaster)
        pDiscMaster->Release();
    if (co_init)
        CoUninitialize();
    return is_rw;
}

// end