// __audio_h.cpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "__audio_h.hpp"
#include "__imapi_com.hpp"
#include "where_how_why_ffmpeg.hpp"
#include "logger.hpp"
#include "config.hpp"
#include <windows.h>
#include <oleauto.h>
#include <objidl.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <chrono>
#include <thread>
#include <cstdio>
const CLSID clsid_msftdiscformat2tao = {0x27354129, 0x7F64, 0x5B0F, {0x8F, 0x00, 0x5D, 0x77, 0xAF, 0xBE, 0x26, 0x1E}};
const IID iddd_discformat2taoe = {0x2735413D, 0x7F64, 0x5B0F, {0x8F, 0x00, 0x5D, 0x77, 0xAF, 0xBE, 0x26, 0x1E}};
enum
{
    IMAPI_FORMAT2_TAO_WRITE_ACTION_UNKNOWN = 0,
    IMAPI_FORMAT2_TAO_WRITE_ACTION_PREPARING = 1,
    IMAPI_FORMAT2_TAO_WRITE_ACTION_WRITING = 2,
    IMAPI_FORMAT2_TAO_WRITE_ACTION_FINISHING = 3,
    IMAPI_FORMAT2_TAO_WRITE_ACTION_VERIFYING = 4
};

static bool wav_header_maybe(const std::string &path, uint64_t &out_pcm_size)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    uint8_t header[44];
    if (fread(header, 1, 44, f) != 44)
    {
        fclose(f);
        return false;
    }
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVEfmt ", 8) != 0)
    {
        fclose(f);
        return false;
    }
    uint16_t format = *(uint16_t *)(header + 20);
    uint16_t channels = *(uint16_t *)(header + 22);
    uint32_t rate = *(uint32_t *)(header + 24);
    uint16_t bits_per_s = *(uint16_t *)(header + 34);
    if (format != 1 || channels != 2 || rate != 44100 || bits_per_s != 16)
    {
        fclose(f);
        return false;
    }
    uint32_t __size = 0;
    if (memcmp(header + 36, "data", 4) == 0)
    {
        __size = *(uint32_t *)(header + 40);
    }
    else
    {
        fseek(f, 0, SEEK_END);
        __size = ftell(f) - 44;
    }
    out_pcm_size = __size;
    fclose(f);
    return true;
}
static bool wav_buf_hdr(const uint8_t *buf, size_t len, uint64_t &out_pcm_size)
{
    if (len < 44)
        return false;
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVEfmt ", 8) != 0)
        return false;
    uint16_t format, channels, bits_per_s;
    uint32_t rate;
    memcpy(&format, buf + 20, 2);
    memcpy(&channels, buf + 22, 2);
    memcpy(&rate, buf + 24, 4);
    memcpy(&bits_per_s, buf + 34, 2);
    if (format != 1 || channels != 2 || rate != 44100 || bits_per_s != 16)
        return false;
    uint32_t __size = 0;
    if (len >= 44 && memcmp(buf + 36, "data", 4) == 0)
        memcpy(&__size, buf + 40, 4);
    else
        __size = (uint32_t)(len > 44 ? len - 44 : 0);
    out_pcm_size = __size;
    return true;
}
static int track_id = 0;
static std::atomic<int> pending_converts{0};
static std::atomic<int> ok_converts{0};
void audio_list::add_path(const std::string &path)
{
    std::lock_guard<std::mutex> lk(list_mutex);
    for (const auto &e : entries)
    {
        if (e.original_path == path)
            return;
    }
    audio_track entry;
    entry.original_path = path;
    entry.selected = false;
    size_t pos = path.find_last_of("\\/");
    entry.display_name = (pos != std::string::npos) ? path.substr(pos + 1) : path;
    uint64_t pcm_size = 0;
    if (wav_header_maybe(path, pcm_size))
    {
        entry.pcm_path = path;
        entry.size_bytes = pcm_size;
        entry.is_temp = false;
        entry.is_converting = false;
        entry.pcm_ram.clear();
        entries.push_back(entry);
    }
    else
    {
        entry.pcm_path = "";
        entry.pcm_ram.clear();
        entry.size_bytes = 0;
        entry.is_temp = true;
        entry.is_converting = true;
        entries.push_back(entry);
        bool to_disk = _config.where_audio_go;
        std::string temp_wav;
        if (to_disk)
        {
            std::string cache = cache_audio_dir();
            CreateDirectoryA(cache.c_str(), nullptr);
            temp_wav = cache + "\\track_" + std::to_string(++track_id) + ".wav";
        }
        ++pending_converts;
        ++ok_converts;
        std::thread([this, path, temp_wav, to_disk]()
                    {
            uint64_t sz = 0;
            bool ok = false;
            std::vector<uint8_t> ram;
            if (to_disk)
                ok = ff2wav(path, temp_wav) && wav_header_maybe(temp_wav, sz);
            else
                ok = ff2wav_in_ram(path, ram) && wav_buf_hdr(ram.data(), ram.size(), sz);
            std::lock_guard<std::mutex> inner_lk(list_mutex);
            for (auto it = entries.begin(); it != entries.end(); ++it)
            {
                if (it->original_path == path && it->is_converting)
                {
                    it->is_converting = false;
                    if (ok)
                    {
                        if (to_disk)
                        {
                            it->pcm_path = temp_wav;
                            it->pcm_ram.clear();
                        }
                        else
                        {
                            it->pcm_path.clear();
                            it->pcm_ram = std::move(ram);
                        }
                        it->size_bytes = sz;
                        int remaining = --pending_converts;
                        if (remaining == 0)
                        {
                            int n = ok_converts.exchange(0);
                            LOG_INFOF("Added %d track(s) to tracklist", n);
                        }
                    }
                    else
                    {
                        --ok_converts;
                        LOG_ERRF("Failed to decode: %s", path.c_str());
                        int remaining = --pending_converts;
                        if (remaining == 0 && ok_converts.load() > 0)
                        {
                            int n = ok_converts.exchange(0);
                            LOG_INFOF("Added %d track(s) to tracklist", n);
                        }
                        entries.erase(it);
                    }
                    break;
                }
            } })
            .detach();
    }
}
void audio_list::rm_selected()
{
    std::lock_guard<std::mutex> lk(list_mutex);
    auto it = entries.begin();
    while (it != entries.end())
    {
        if (it->selected)
        {
            if (it->is_temp && !it->pcm_path.empty())
                DeleteFileA(it->pcm_path.c_str());
            it->pcm_ram.clear();
            it = entries.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
void audio_list::clear()
{
    std::lock_guard<std::mutex> lk(list_mutex);
    for (auto &e : entries)
    {
        if (e.is_temp && !e.pcm_path.empty())
            DeleteFileA(e.pcm_path.c_str());
        e.pcm_ram.clear();
    }
    entries.clear();
}
uint64_t audio_list::total_size() const
{
    std::lock_guard<std::mutex> lk(const_cast<std::mutex &>(list_mutex));
    uint64_t total = 0;
    for (const auto &e : entries)
        total += e.size_bytes;
    return total;
}
class wav_pcm_iss : public IStream
{
private:
    LONG m_cref;
    IStream *m_base;
    uint64_t m_data_size;
    uint64_t m_bytes_read;
    audio_context *m_ctx;

public:
    wav_pcm_iss(IStream *base, uint64_t size, audio_context *ctx) : m_cref(1), m_base(base), m_bytes_read(0), m_ctx(ctx)
    {
        m_data_size = ((size + 2351) / 2352) * 2352;
        m_base->AddRef();
        LARGE_INTEGER pos;
        pos.QuadPart = 44;
        m_base->Seek(pos, STREAM_SEEK_SET, nullptr);
    }
    virtual ~wav_pcm_iss() { m_base->Release(); }
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
            delete this; // m; Bradar.
        return r;        // c; yo shut the fuck up
    }
    STDMETHODIMP Read(void *pv, ULONG cb, ULONG *pcbRead) override
    {
        ULONG to_read = cb;
        if (m_data_size > 0 && (m_bytes_read + cb > m_data_size))
        {
            to_read = (ULONG)(m_data_size - m_bytes_read);
        }
        if (to_read == 0)
        {
            if (pcbRead)
                *pcbRead = 0;
            return S_FALSE;
        }
        ULONG base_bytes = 0;
        HRESULT hr = m_base->Read(pv, to_read, &base_bytes);
        if (SUCCEEDED(hr))
        {
            if (base_bytes < to_read)
            {
                memset((uint8_t *)pv + base_bytes, 0, to_read - base_bytes);
                base_bytes = to_read;
            }
            m_bytes_read += base_bytes;
            if (pcbRead)
                *pcbRead = base_bytes;
        }
        return hr;
    }
    STDMETHODIMP Write(const void *, ULONG, ULONG *) override { return E_NOTIMPL; }
    STDMETHODIMP Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition) override
    {
        if (dwOrigin == STREAM_SEEK_SET) // c; logically, intercept and seek at 44 instead of null
        {
            dlibMove.QuadPart += 44;
            HRESULT hr = m_base->Seek(dlibMove, dwOrigin, plibNewPosition);
            if (SUCCEEDED(hr) && plibNewPosition)
                plibNewPosition->QuadPart -= 44;
            return hr;
        }
        return m_base->Seek(dlibMove, dwOrigin, plibNewPosition);
    }
    STDMETHODIMP SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    STDMETHODIMP CopyTo(IStream *, ULARGE_INTEGER, ULARGE_INTEGER *, ULARGE_INTEGER *) override { return E_NOTIMPL; }
    STDMETHODIMP Commit(DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP Revert() override { return E_NOTIMPL; }
    STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP Stat(STATSTG *pstatstg, DWORD grfStatFlag) override
    {
        HRESULT hr = m_base->Stat(pstatstg, grfStatFlag);
        if (SUCCEEDED(hr))
            pstatstg->cbSize.QuadPart = m_data_size;
        return hr;
    }
    STDMETHODIMP Clone(IStream **ppstm) override { return E_NOTIMPL; }
};
class _ae_sink : public IDispatch
{
    LONG m_cref;
    audio_context *m_ctx;

public:
    _ae_sink(audio_context *ctx) : m_cref(1), m_ctx(ctx) {}
    virtual ~_ae_sink() {}
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDispatch) || IsEqualIID(riid, iddd_discformat2taoe))
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
        ULONG r = InterlockedDecrement(&m_cref);
        if (r == 0)
            delete this;
        return r;
    }
    STDMETHODIMP GetTypeInfoCount(UINT *p) override
    {
        *p = 0;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo **) override { return E_NOTIMPL; }
    STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR *, UINT, LCID, DISPID *) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(DISPID dispId, REFIID, LCID, WORD, DISPPARAMS *pParams, VARIANT *, EXCEPINFO *, UINT *) override
    {
        if (dispId == 0x200 && pParams->cArgs >= 2)
        {
            IDispatch *pProgress = pParams->rgvarg[0].pdispVal;
            IDispatch *pFormat = pParams->rgvarg[1].pdispVal;
            if (m_ctx->abort_requested)
            {
                imapi_dispatch_call(pFormat, L"CancelAddTrack", DISPATCH_METHOD, nullptr, nullptr, 0);
                imapi_set_stat(m_ctx, "Aborting...");
            }
            if (pProgress)
            {
                VARIANT vAction, vElapsed, vTrack;
                VariantInit(&vAction);
                VariantInit(&vElapsed);
                VariantInit(&vTrack);
                imapi_dispatch_get(pProgress, L"CurrentAction", &vAction);
                imapi_dispatch_get(pProgress, L"ElapsedTime", &vElapsed);
                imapi_dispatch_get(pProgress, L"CurrentTrackNumber", &vTrack);
                if (vElapsed.vt == VT_I4)
                    m_ctx->elapsed_seconds = vElapsed.lVal;
                if (vAction.vt == VT_I4)
                {
                    LONG action = vAction.lVal;
                    if (action == IMAPI_FORMAT2_TAO_WRITE_ACTION_PREPARING)
                        imapi_set_stat(m_ctx, "Preparing...");
                    else if (action == IMAPI_FORMAT2_TAO_WRITE_ACTION_WRITING)
                    {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "Writing track %d...", vTrack.vt == VT_I4 ? (int)vTrack.lVal : 0);
                        imapi_set_stat(m_ctx, buf);
                    }
                    else if (action == IMAPI_FORMAT2_TAO_WRITE_ACTION_FINISHING)
                        imapi_set_stat(m_ctx, "Finishing track...");
                    else if (action == IMAPI_FORMAT2_TAO_WRITE_ACTION_VERIFYING)
                        imapi_set_stat(m_ctx, "Verifying...");
                }
            }
        }
        return S_OK;
    }
};
static void audio_thread_func(audio_context *ctx)
{
    LOG_INFO("Audio engine starting");
    imapi_set_stat(ctx, "Initializing...");
    if (!ctx->tracks || ctx->tracks->entries.empty())
    {
        LOG_ERR("No tracks to burn");
        imapi_set_stat(ctx, "[ ERROR ] No tracks");
        ctx->is_running = false;
        return;
    }
    if (!ctx->drive || !ctx->drive->valid)
    {
        LOG_ERR("Selected reader / writer is invalid");
        imapi_set_stat(ctx, "[ ERROR ] Invalid reader / writer");
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
    IDispatch *pTao = nullptr;
    _ae_sink *pEventSink = nullptr;
    DWORD dwCookie = 0;
    imapi_set_stat(ctx, "Searching for drive...");
    hr = imapi_open_disc_master(&pDiscMaster);
    if (FAILED(hr))
        goto cleanup;
    hr = imapi_find_recorder(pDiscMaster, ctx->drive->info.path, &pRecorder);
    if (FAILED(hr) || !pRecorder)
    {
        LOG_ERR("Could not find IMAPIv2 recorder for drive");
        imapi_set_stat(ctx, "[ ERROR ] Drive not found");
        goto cleanup;
    }
    imapi_set_stat(ctx, "Burning...");
    hr = CoCreateInstance(clsid_msftdiscformat2tao, nullptr, CLSCTX_ALL, IID_IDispatch, (void **)&pTao);
    if (FAILED(hr))
    {
        LOG_ERR("Failed to create MsftDiscFormat2TrackAtOnce"); // m; Real great names, man.
                                                                // c; yeah well i didn't make them, ok?
        goto cleanup;
    }
    {
        VARIANT vRec;
        VariantInit(&vRec);
        vRec.vt = VT_DISPATCH;
        vRec.pdispVal = pRecorder;
        hr = imapi_dispatch_put(pTao, L"Recorder", &vRec);
        if (FAILED(hr))
            goto cleanup;
        VARIANT vClient;
        VariantInit(&vClient);
        vClient.vt = VT_BSTR;
        vClient.bstrVal = SysAllocString(L"audio engine");
        imapi_dispatch_put(pTao, L"ClientName", &vClient);
        SysFreeString(vClient.bstrVal);
        VARIANT vSpeed;
        VariantInit(&vSpeed);
        vSpeed.vt = VT_I4;
        vSpeed.lVal = (ctx->speed_multiplier <= 0) ? -1 : ctx->speed_multiplier * 1385; // m; Wait
        imapi_dispatch_put(pTao, L"RequestedWriteSpeed", &vSpeed);
    }
    {
        VARIANT vBlank;
        VariantInit(&vBlank);
        if (SUCCEEDED(imapi_dispatch_get(pTao, L"MediaHeuristicallyBlank", &vBlank)) && vBlank.vt == VT_BOOL)
        {
            if (vBlank.boolVal == VARIANT_FALSE)
            {
                LOG_ERR("Data cannot be burned further more; disc is not blank");
                imapi_set_stat(ctx, "[ ERROR ] Disc not blank");
                goto cleanup;
            }
        }
    }
    hr = imapi_dispatch_call(pTao, L"PrepareMedia", DISPATCH_METHOD, nullptr, nullptr, 0);
    if (FAILED(hr))
    {
        LOG_ERRF("PrepareMedia failed: 0x%08X", hr);
        imapi_set_stat(ctx, "[ ERROR ] Media prepare");
        goto cleanup;
    }
    pEventSink = new _ae_sink(ctx);
    imapi_connpt_advise(pTao, iddd_discformat2taoe, pEventSink, &dwCookie);
    {
        uint64_t total_size = ctx->tracks->total_size();
        uint64_t burned_so_far = 0;
        LOG_INFOF("Writing %d files as raw audio data", (int)ctx->tracks->entries.size());
        for (size_t i = 0; i < ctx->tracks->entries.size(); ++i)
        {
            if (ctx->abort_requested)
                break;
            const auto &entry = ctx->tracks->entries[i];
            IStream *pFileStream = nullptr;
            if (!entry.pcm_ram.empty())
            {
                SIZE_T szb = (SIZE_T)entry.pcm_ram.size();
                HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, szb);
                if (!hg)
                {
                    LOG_ERR("RAM track: GlobalAlloc failed");
                    continue;
                }
                void *pv = GlobalLock(hg);
                if (!pv)
                {
                    GlobalFree(hg);
                    continue;
                }
                memcpy(pv, entry.pcm_ram.data(), szb);
                GlobalUnlock(hg);
                hr = CreateStreamOnHGlobal(hg, TRUE, &pFileStream);
                if (FAILED(hr))
                {
                    GlobalFree(hg);
                    LOG_ERRF("RAM track: CreateStreamOnHGlobal 0x%08X", hr);
                    continue;
                }
            }
            else
            {
                std::wstring wPath = imapi_u8_to_w(entry.pcm_path);
                hr = SHCreateStreamOnFileW(wPath.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, &pFileStream);
                if (FAILED(hr))
                {
                    LOG_ERRF("Failed to open audio track %s", entry.pcm_path.c_str());
                    continue;
                }
            }
            wav_pcm_iss *pWavStream = new wav_pcm_iss(pFileStream, entry.size_bytes, ctx);
            pFileStream->Release();
            VARIANT vStream;
            VariantInit(&vStream);
            vStream.vt = VT_UNKNOWN;
            vStream.punkVal = pWavStream;
            auto start_t = std::chrono::steady_clock::now();
            hr = imapi_dispatch_call(pTao, L"AddAudioTrack", DISPATCH_METHOD, nullptr, &vStream, 1);
            pWavStream->Release();
            if (FAILED(hr))
            {
                LOG_ERRF("Failed to write raw audio data to CD (track %d: %s), error: 0x%08X", (int)i + 1, entry.display_name.c_str(), hr);
                break;
            }
            burned_so_far += entry.size_bytes;
            ctx->progress_percent = (float)burned_so_far / total_size * 100.0f;
            auto end_t = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(end_t - start_t).count();
            if (dt > 0.0f)
            {
                ctx->write_speed_mbps = (entry.size_bytes / 1024.0f / 1024.0f) / dt;
            }
        }
    }
    if (!ctx->abort_requested)
    {
        imapi_set_stat(ctx, "Closing session...");
        hr = imapi_dispatch_call(pTao, L"ReleaseMedia", DISPATCH_METHOD, nullptr, nullptr, 0);
        if (SUCCEEDED(hr))
        {
            LOG_OK("Burn finished; quick wasn't it? [ AUDIO CD ]");
            imapi_set_stat(ctx, "Quick, wasn't it?");
            ctx->progress_percent = 100.0f;
            if (ctx->eject_when_done && pRecorder)
            {
                LOG_INFO("Ejecting disc");
                imapi_dispatch_call(pRecorder, L"EjectMedia", DISPATCH_METHOD, nullptr, nullptr, 0);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::wstring wDriveStr = imapi_u8_to_w(ctx->drive->info.path);
                SHChangeNotify(SHCNE_MEDIAINSERTED, SHCNF_PATHW, wDriveStr.c_str(), NULL);
            }
        }
        else
        {
            LOG_ERRF("ReleaseMedia failed: 0x%08X", hr);
            imapi_set_stat(ctx, "[ ERROR ] Finalization");
        }
    }
cleanup:
    if (pEventSink && dwCookie && pTao)
        imapi_connpt_rm(pTao, iddd_discformat2taoe, dwCookie);
    if (pEventSink)
        pEventSink->Release();
    if (pTao)
        pTao->Release();
    if (pRecorder)
        pRecorder->Release();
    if (pDiscMaster)
        pDiscMaster->Release();
    CoUninitialize();
    if (ctx->drive && ctx->drive->valid)
        drives_close(*ctx->drive);
    cache_audio_sw();
    LOG_INFO("Audio engine shutting down");
    ctx->is_running = false;
}
void audio_init(audio_context &ctx, drive_handle *drive, audio_list *tracks, int speed_mult, bool eject)
{
    if (ctx.worker_thread)
    {
        if (ctx.worker_thread->joinable())
            ctx.worker_thread->join();
        delete ctx.worker_thread;
        ctx.worker_thread = nullptr;
    }
    ctx.drive = drive;
    ctx.tracks = tracks;
    ctx.speed_multiplier = speed_mult;
    ctx.eject_when_done = eject;
    ctx.is_running = false;
    ctx.abort_requested = false;
    ctx.progress_percent = 0.0f;
    ctx.write_speed_mbps = 0.0f;
    ctx.elapsed_seconds = 0;
    {
        std::lock_guard<std::mutex> lk(ctx.status_mutex);
        snprintf(ctx.status_text, sizeof(ctx.status_text), "Idle");
    }
}
bool audio_start(audio_context &ctx)
{
    if (ctx.is_running)
        return false;
    ctx.is_running = true;
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
    ctx.worker_thread = new std::thread(audio_thread_func, &ctx);
    return true;
}
void audio_abort(audio_context &ctx)
{
    if (ctx.is_running)
    {
        ctx.abort_requested = true;
        LOG_WARN("Abort requested");
    }
}
void audio_progress(const audio_context &ctx, float &out_percent, float &out_mbps, uint32_t &out_elapsed_sec)
{
    out_percent = ctx.progress_percent.load();
    out_mbps = ctx.write_speed_mbps.load();
    out_elapsed_sec = ctx.elapsed_seconds.load();
}

// end