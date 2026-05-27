// __build.cpp
// last updated: 27/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "__build.hpp"
#include "__imapi_com.hpp"
#include "logger.hpp"
#include <windows.h>
#include <oleauto.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <string>
#include <chrono>
#include <algorithm>
static HRESULT add_dir(IDispatch *pDir, const std::wstring &src_path, build_context *ctx)
{
    std::wstring pat = src_path;
    if (!pat.empty() && pat.back() != L'\\' && pat.back() != L'/')
        pat += L'\\';
    pat += L'*';
    WIN32_FIND_DATAW ffd;
    HANDLE hf = FindFirstFileW(pat.c_str(), &ffd);
    if (hf == INVALID_HANDLE_VALUE)
        return S_OK;
    HRESULT hr_out = S_OK;
    do
    {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0)
            continue;
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;
        if (ctx->abort_requested)
        {
            hr_out = E_ABORT;
            break;
        }
        std::wstring child = src_path;
        if (!child.empty() && child.back() != L'\\' && child.back() != L'/')
            child += L'\\';
        child += ffd.cFileName;
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            VARIANT arg;
            VariantInit(&arg);
            arg.vt = VT_BSTR;
            arg.bstrVal = SysAllocString(ffd.cFileName);
            HRESULT hr = imapi_dispatch_call(pDir, L"AddDirectory", DISPATCH_METHOD, nullptr, &arg, 1);
            SysFreeString(arg.bstrVal);
            if (FAILED(hr))
            {
                std::string name(ffd.cFileName, ffd.cFileName + wcslen(ffd.cFileName));
                LOG_WARNF("Skipped dir '%s': 0x%08X", name.c_str(), hr);
                continue;
            }
            VARIANT vIdx;
            VariantInit(&vIdx);
            vIdx.vt = VT_BSTR;
            vIdx.bstrVal = SysAllocString(ffd.cFileName);
            VARIANT vSub;
            VariantInit(&vSub);
            hr = imapi_dispatch_call(pDir, L"Item", DISPATCH_PROPERTYGET | DISPATCH_METHOD, &vSub, &vIdx, 1);
            SysFreeString(vIdx.bstrVal);
            if (SUCCEEDED(hr) && vSub.vt == VT_DISPATCH && vSub.pdispVal)
            {
                hr = add_dir(vSub.pdispVal, child, ctx);
                if (FAILED(hr) && hr != E_ABORT)
                    hr_out = hr;
            }
            VariantClear(&vSub);
        }
        else
        {
            HANDLE hFile = CreateFileW(child.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile == INVALID_HANDLE_VALUE)
            {
                std::string name(ffd.cFileName, ffd.cFileName + wcslen(ffd.cFileName));
                LOG_WARNF("Skipped locked file '%s'", name.c_str());
                continue;
            }
            IStream *pStream = nullptr;
            HRESULT hr = SHCreateStreamOnFileW(child.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, &pStream);
            CloseHandle(hFile);
            if (FAILED(hr) || !pStream)
            {
                std::string name(ffd.cFileName, ffd.cFileName + wcslen(ffd.cFileName));
                LOG_WARNF("Skipped file '%s': stream open failed 0x%08X", name.c_str(), hr);
                continue;
            }
            VARIANT args[2];
            VariantInit(&args[0]);
            VariantInit(&args[1]);
            args[0].vt = VT_UNKNOWN;
            args[0].punkVal = pStream;
            args[1].vt = VT_BSTR;
            args[1].bstrVal = SysAllocString(ffd.cFileName);
            hr = imapi_dispatch_call(pDir, L"AddFile", DISPATCH_METHOD, nullptr, args, 2);
            SysFreeString(args[1].bstrVal);
            pStream->Release();
            if (FAILED(hr))
            {
                std::string name(ffd.cFileName, ffd.cFileName + wcslen(ffd.cFileName));
                LOG_WARNF("Failed to add file '%s': 0x%08X", name.c_str(), hr);
            }
        }
    } while (FindNextFileW(hf, &ffd));
    FindClose(hf);
    return hr_out;
}
static uint64_t dir_size_bs(const std::string &dir_path)
{
    std::string pattern = dir_path;
    if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/')
        pattern += "\\";
    pattern += "*";
    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE)
        return 0;
    uint64_t total = 0;
    do
    {
        const char *name = ffd.cFileName;
        if (!name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;
        std::string child = dir_path;
        if (!child.empty() && child.back() != '\\' && child.back() != '/')
            child += "\\";
        child += name;
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            total += dir_size_bs(child);
        }
        else
        {
            uint64_t sz = ((uint64_t)ffd.nFileSizeHigh << 32) | ffd.nFileSizeLow;
            total += sz;
        }
    } while (FindNextFileA(hFind, &ffd));
    FindClose(hFind);
    return total;
}
static const CLSID clsid_msftfilesysimg = {0x2C941FC5, 0x975B, 0x59BE, {0xA9, 0x60, 0x9A, 0x2A, 0x26, 0x28, 0x53, 0xA5}};
static const CLSID clsid_msftdiscformattodata = {0x2735412A, 0x7F64, 0x5B0F, {0x8F, 0x00, 0x5D, 0x77, 0xAF, 0xBE, 0x26, 0x1E}};
enum
{
    IMAPI_F2_DATA_WRITE_ACTION_WRITING_DATA = 4,
    IMAPI_F2_DATA_WRITE_ACTION_FORMATTING_MEDIA = 1,
    IMAPI_F2_DATA_WRITE_ACTION_FINALIZATION = 5,
    IMAPI_F2_DATA_WRITE_ACTION_VALIDATING_MEDIA = 0,
    IMAPI_F2_DATA_WRITE_ACTION_CALIBRATING_POWER = 3,
};
enum
{
    FSI_FILE_SYSTEM_ISO9660 = 1,
    FSI_FILE_SYSTEM_JOLIET = 2,
    FSI_FILE_SYSTEM_UDF = 4
};
void _build_list::add_path(const std::string &path)
{
    DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return;
    for (const auto &e : entries)
    {
        if (e.path == path)
            return;
    }
    build_entry entry;
    entry.path = path;
    entry.selected = false;
    entry.is_directory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t pos = path.find_last_of("\\/");
    entry.display_name = (pos != std::string::npos) ? path.substr(pos + 1) : path;
    if (entry.is_directory)
    {
        entry.size_bytes = dir_size_bs(path);
    }
    else
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad))
        {
            entry.size_bytes = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        }
        else
        {
            entry.size_bytes = 0;
        }
    }
    entries.push_back(entry);
}
void _build_list::rm_selected()
{
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const build_entry &e)
                                 { return e.selected; }),
                  entries.end()); // c; why does vscode format it like this? i'll never know and i'm too lazy to change it
}
void _build_list::clear()
{
    entries.clear();
}
uint64_t _build_list::total_size() const
{
    uint64_t total = 0;
    for (const auto &e : entries)
        total += e.size_bytes;
    return total;
}
class build_event_sink : public IDispatch
{
    LONG m_cref;
    build_context *m_ctx;
    uint64_t m_last_bytes;
    std::chrono::steady_clock::time_point m_last_time;
    bool m_is_finalizing;
    uint32_t m_finalizing_start;

public:
    build_event_sink(build_context *ctx) : m_cref(1), m_ctx(ctx), m_last_bytes(0), m_is_finalizing(false), m_finalizing_start(0)
    {
        m_last_time = std::chrono::steady_clock::now();
    }
    virtual ~build_event_sink() {}
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
                imapi_dispatch_call(pFormat, L"CancelWrite", DISPATCH_METHOD, nullptr, nullptr, 0);
                imapi_set_stat(m_ctx, "Aborting...");
            }
            if (pProgress)
            {
                VARIANT vAction;
                VariantInit(&vAction);
                if (SUCCEEDED(imapi_dispatch_get(pProgress, L"CurrentAction", &vAction)))
                {
                    if (vAction.vt == VT_I4)
                    {
                        LONG action = vAction.lVal;
                        VARIANT vElapsed;
                        VariantInit(&vElapsed);
                        if (SUCCEEDED(imapi_dispatch_get(pProgress, L"ElapsedTime", &vElapsed)) && vElapsed.vt == VT_I4)
                            m_ctx->elapsed_seconds = vElapsed.lVal;
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
                            if (action == IMAPI_F2_DATA_WRITE_ACTION_FINALIZATION)
                            {
                                if (!m_is_finalizing)
                                {
                                    m_is_finalizing = true;
                                    m_finalizing_start = m_ctx->elapsed_seconds;
                                }
                                float larp_progress = (float)(m_ctx->elapsed_seconds - m_finalizing_start) / 15.0f * 100.0f;
                                if (larp_progress > 99.0f) // c; we don't know how long this part would take, so we gonna larp it
                                    larp_progress = 99.0f;
                                m_ctx->progress_percent = larp_progress;
                                m_ctx->write_speed_mbps = 0.0f;
                            }
                            else if (vTotal.vt == VT_I4 && vTotal.lVal > 0)
                            {
                                LONG total = vTotal.lVal;
                                LONG last = (vLast.vt == VT_I4) ? vLast.lVal : 0;
                                LONG start = (vStart.vt == VT_I4) ? vStart.lVal : 0;
                                LONG written = last - start;
                                if (written < 0)
                                    written = 0;
                                m_ctx->progress_percent = (float)written / (float)total * 100.0f;
                                uint64_t cur = (uint64_t)written * 2048;
                                auto now = std::chrono::steady_clock::now();
                                auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_time).count();
                                if (dt >= 1000)
                                {
                                    if (cur >= m_last_bytes)
                                        m_ctx->write_speed_mbps = (float)(cur - m_last_bytes) / 1024.0f / 1024.0f / (dt / 1000.0f);
                                    m_last_bytes = cur;
                                    m_last_time = now;
                                }
                            }
                        }
                        else if (action == IMAPI_F2_DATA_WRITE_ACTION_FORMATTING_MEDIA)
                            imapi_set_stat(m_ctx, "Formatting...");

                        else if (action == IMAPI_F2_DATA_WRITE_ACTION_VALIDATING_MEDIA)
                            imapi_set_stat(m_ctx, "Validating...");
                        else if (action == IMAPI_F2_DATA_WRITE_ACTION_CALIBRATING_POWER)
                            imapi_set_stat(m_ctx, "Calibrating laser..."); // c; same bullshi from __burn.cpp in theory so i can js copy n' paste 95% of it
                    }
                }
            }
        }
        return S_OK;
    }
};
static void build_thread_func(build_context *ctx)
{
    ctx->is_running = true;
    LOG_INFO("Build engine starting");
    imapi_set_stat(ctx, "Initializing...");
    if (!ctx->file_list || ctx->file_list->entries.empty())
    {
        LOG_ERR("No files to build");
        imapi_set_stat(ctx, "[ ERROR ] No files");
        ctx->is_running = false;
        return;
    }
    if (!ctx->build_to_iso && (!ctx->drive || !ctx->drive->valid))
    {
        LOG_ERR("Selected reader / writer is invalid");
        imapi_set_stat(ctx, "[ ERROR ] Invalid reader / writer");
        ctx->is_running = false;
        return;
    }
    else if (ctx->build_to_iso && ctx->iso_output_path.empty())
    {
        LOG_ERR("Invalid output path");
        imapi_set_stat(ctx, "[ ERROR ] Invalid path");
        ctx->is_running = false;
        return;
    }
    else if (!ctx->build_to_iso && ctx->drive)
    {
        bool is_bd_drive = (ctx->drive->info.type == drive_type::bdrom || ctx->drive->info.type == drive_type::bdrw);
        if (is_bd_drive && ctx->filesystem_mode != iso_mode::udf)
        {
            LOG_ERR("BD media requires UDF mode in this build path");
            imapi_set_stat(ctx, "[ ERROR ] ISO mode invalid for BD");
            ctx->is_running = false;
            return;
        }
    }
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        LOG_ERRF("CoInitializeEx[...] failed: 0x%08X", hr);
        imapi_set_stat(ctx, "[ ERROR ] COM init");
        ctx->is_running = false;
        return;
    }
    IDispatch *pFSI = nullptr;
    IDispatch *pRoot = nullptr;
    IDispatch *pResult = nullptr;
    IStream *pStream = nullptr;
    IDispatch *pDiscMaster = nullptr;
    IDispatch *pRecorder = nullptr;
    IDispatch *pDataWriter = nullptr;
    build_event_sink *pEventSink = nullptr;
    DWORD dwCookie = 0;
    uint64_t image_bytes = 0;
    uint64_t approx_bytes = 0;
    uint64_t free_media_bytes = 0;
    if (ctx->file_list)
        approx_bytes = ctx->file_list->total_size();
    if (!ctx->build_to_iso)
    {
        imapi_set_stat(ctx, "Checking media...");
        hr = imapi_open_disc_master(&pDiscMaster);
        if (FAILED(hr))
        {
            LOG_ERR("Failed to create MsftDiscMaster2");
            imapi_set_stat(ctx, "[ ERROR ] IMAPI init");
            goto cleanup;
        }
        hr = imapi_find_recorder(pDiscMaster, ctx->drive->info.path, &pRecorder);
        if (FAILED(hr) || !pRecorder)
        {
            LOG_ERR("Could not find IMAPIv2 recorder for drive");
            imapi_set_stat(ctx, "[ ERROR ] Drive not found");
            goto cleanup;
        }
        hr = CoCreateInstance(clsid_msftdiscformattodata, nullptr, CLSCTX_ALL, IID_IDispatch, (void **)&pDataWriter);
        if (FAILED(hr))
        {
            LOG_ERR("Failed to create MsftDiscFormat2Data");
            imapi_set_stat(ctx, "[ ERROR ] IMAPI init");
            goto cleanup;
        }
        {
            VARIANT vRec;
            VariantInit(&vRec);
            vRec.vt = VT_DISPATCH;
            vRec.pdispVal = pRecorder;
            hr = imapi_dispatch_put(pDataWriter, L"Recorder", &vRec);
            if (FAILED(hr))
            {
                LOG_ERR("Failed to attach recorder");
                imapi_set_stat(ctx, "[ ERROR ] Recorder attach");
                goto cleanup;
            }
            VARIANT vClient;
            VariantInit(&vClient);
            vClient.vt = VT_BSTR;
            vClient.bstrVal = SysAllocString(L"build engine");
            imapi_dispatch_put(pDataWriter, L"ClientName", &vClient);
            VariantClear(&vClient);
        }
        {
            VARIANT vBlank;
            VariantInit(&vBlank);
            if (SUCCEEDED(imapi_dispatch_get(pDataWriter, L"MediaHeuristicallyBlank", &vBlank)) && vBlank.vt == VT_BOOL)
            {
                if (vBlank.boolVal == VARIANT_FALSE)
                {
                    LOG_ERR("Data cannot be burned further more; disc is not blank");
                    imapi_set_stat(ctx, "[ ERROR ] Disc not blank");
                    VariantClear(&vBlank);
                    goto cleanup;
                }
            }
            VariantClear(&vBlank);
        }
        {
            VARIANT vFree;
            VariantInit(&vFree);
            if (SUCCEEDED(imapi_dispatch_get(pDataWriter, L"FreeSectorsOnMedia", &vFree)) && (vFree.vt == VT_I4 || vFree.vt == VT_UI4))
            {
                free_media_bytes = (uint64_t)(uint32_t)vFree.lVal * 2048ULL;
                if (approx_bytes > 0 && free_media_bytes > 0 && approx_bytes > free_media_bytes)
                {
                    LOG_ERRF("Requested data too large for disc (est.); need %llu bytes, free %llu bytes", (unsigned long long)approx_bytes, (unsigned long long)free_media_bytes);
                    imapi_set_stat(ctx, "[ ERROR ] Image too large");
                    VariantClear(&vFree);
                    goto cleanup;
                }
            }
            VariantClear(&vFree);
        }
    }
    imapi_set_stat(ctx, "Creating filesystem...");
    hr = CoCreateInstance(clsid_msftfilesysimg, nullptr, CLSCTX_ALL, IID_IDispatch, (void **)&pFSI);
    if (FAILED(hr))
    {
        LOG_ERRF("Failed to create MsftFileSystemImage: 0x%08X", hr);
        goto cleanup;
    }
    {
        int fs_mask = FSI_FILE_SYSTEM_JOLIET;
        if (ctx->filesystem_mode == iso_mode::iso_9660)
            fs_mask = FSI_FILE_SYSTEM_ISO9660;
        else if (ctx->filesystem_mode == iso_mode::joliet)
            fs_mask = FSI_FILE_SYSTEM_ISO9660 | FSI_FILE_SYSTEM_JOLIET;
        else if (ctx->filesystem_mode == iso_mode::udf)
            fs_mask = FSI_FILE_SYSTEM_UDF;
        else if (ctx->filesystem_mode == iso_mode::hybrid)
            fs_mask = FSI_FILE_SYSTEM_ISO9660 | FSI_FILE_SYSTEM_JOLIET | FSI_FILE_SYSTEM_UDF;
        VARIANT vType;
        VariantInit(&vType);
        vType.vt = VT_I4;
        vType.lVal = fs_mask;
        imapi_dispatch_put(pFSI, L"FileSystemsToCreate", &vType);
        // c; IMAPIv2 defaults to when presenting "free data" around 333,333 sectors (which is CD size); and is an edge case for DVDs
        // m; Does this fix it though?
        // c; yeah
        {
            LONG freeBlocks = 0x7FFFFFFF;
            if (!ctx->build_to_iso && pDataWriter)
            {
                VARIANT vFree;
                VariantInit(&vFree);
                if (SUCCEEDED(imapi_dispatch_get(pDataWriter, L"FreeSectorsOnMedia", &vFree)) &&
                    (vFree.vt == VT_I4 || vFree.vt == VT_UI4) && vFree.lVal > 0)
                    freeBlocks = vFree.lVal;
                VariantClear(&vFree);
            }
            VARIANT vBlocks;
            VariantInit(&vBlocks);
            vBlocks.vt = VT_I4;
            vBlocks.lVal = freeBlocks;
            imapi_dispatch_put(pFSI, L"FreeMediaBlocks", &vBlocks);
        }
        if (!ctx->volume_label.empty())
        {
            std::wstring wLabel = imapi_u8_to_w(ctx->volume_label);
            VARIANT vLabel;
            VariantInit(&vLabel);
            vLabel.vt = VT_BSTR;
            vLabel.bstrVal = SysAllocString(wLabel.c_str());
            imapi_dispatch_put(pFSI, L"VolumeName", &vLabel);
            VariantClear(&vLabel);
        }
    }
    {
        VARIANT vRoot;
        VariantInit(&vRoot);
        hr = imapi_dispatch_get(pFSI, L"Root", &vRoot);
        if (FAILED(hr) || vRoot.vt != VT_DISPATCH)
        {
            LOG_ERR("Failed to get filesystem root");
            goto cleanup;
        }
        pRoot = vRoot.pdispVal;
        pRoot->AddRef();
        VariantClear(&vRoot);
    }
    imapi_set_stat(ctx, "Adding files...");
    LOG_INFOF("Adding %d item(s) to filesystem image", (int)ctx->file_list->entries.size());
    for (size_t i = 0; i < ctx->file_list->entries.size(); ++i)
    {
        if (ctx->abort_requested)
        {
            imapi_set_stat(ctx, "Aborted");
            goto cleanup;
        }
        const build_entry &entry = ctx->file_list->entries[i];
        if (entry.is_directory)
        {
            std::wstring wPath = imapi_u8_to_w(entry.path);
            HRESULT hr_add = add_dir(pRoot, wPath, ctx);
            if (hr_add == E_ABORT)
            {
                imapi_set_stat(ctx, "Aborted");
                goto cleanup;
            }
        }
        else
        {
            std::wstring wName = imapi_u8_to_w(entry.display_name);
            std::wstring wPath = imapi_u8_to_w(entry.path);
            IStream *pFileStream = nullptr;
            hr = SHCreateStreamOnFileW(wPath.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, &pFileStream);
            if (FAILED(hr))
            {
                LOG_ERRF("Failed to open file stream for '%s': 0x%08X", entry.path.c_str(), hr);
                continue;
            }
            VARIANT args[2];
            VariantInit(&args[0]);
            VariantInit(&args[1]);
            args[0].vt = VT_UNKNOWN;
            args[0].punkVal = pFileStream;
            args[1].vt = VT_BSTR;
            args[1].bstrVal = SysAllocString(wName.c_str());
            hr = imapi_dispatch_call(pRoot, L"AddFile", DISPATCH_METHOD, nullptr, args, 2);
            SysFreeString(args[1].bstrVal);
            pFileStream->Release();
            if (FAILED(hr))
            {
                LOG_ERRF("Failed to add file '%s': 0x%08X", entry.path.c_str(), hr);
                imapi_set_stat(ctx, "[ ERROR ] Add file failed");
                goto cleanup;
            }
        }
    }
    imapi_set_stat(ctx, "Building image...");
    {
        VARIANT vResult;
        VariantInit(&vResult);
        hr = imapi_dispatch_call(pFSI, L"CreateResultImage", DISPATCH_METHOD, &vResult, nullptr, 0);
        if (FAILED(hr) || vResult.vt != VT_DISPATCH)
        {
            LOG_ERRF("CreateResultImage failed: 0x%08X", hr);
            goto cleanup;
        }
        pResult = vResult.pdispVal;
        pResult->AddRef();
        VariantClear(&vResult);
        VARIANT vStream;
        VariantInit(&vStream);
        hr = imapi_dispatch_get(pResult, L"ImageStream", &vStream);
        if (FAILED(hr) || vStream.vt != VT_UNKNOWN)
        {
            LOG_ERR("Failed to get image stream");
            goto cleanup;
        }
        hr = vStream.punkVal->QueryInterface(IID_IStream, (void **)&pStream);
        VariantClear(&vStream);
        if (FAILED(hr))
        {
            LOG_ERR("Failed to get IStream from image result");
            goto cleanup;
        }
    }
    LOG_OK("Filesystem image created");
    {
        STATSTG stat = {};
        if (SUCCEEDED(pStream->Stat(&stat, STATFLAG_NONAME)))
        {
            image_bytes = stat.cbSize.QuadPart;
        }
    }
    if (ctx->build_to_iso)
    {
        imapi_set_stat(ctx, "Writing ISO...");
        {
            char out_dir[MAX_PATH] = {0};
            strncpy(out_dir, ctx->iso_output_path.c_str(), sizeof(out_dir) - 1);
            if (!PathRemoveFileSpecA(out_dir))
                strncpy(out_dir, ".", sizeof(out_dir) - 1);
            ULARGE_INTEGER freeBytes = {}, totalBytes = {}, totalFreeBytes = {};
            if (GetDiskFreeSpaceExA(out_dir, &freeBytes, &totalBytes, &totalFreeBytes))
            {
                if (image_bytes > 0 && freeBytes.QuadPart < image_bytes)
                {
                    LOG_ERRF("Not enough disk space for ISO: need %llu bytes, have %llu bytes free", (unsigned long long)image_bytes, (unsigned long long)freeBytes.QuadPart);
                    imapi_set_stat(ctx, "[ ERROR ] Not enough disk space");
                    goto cleanup;
                }
            }
        }
        HANDLE hFile = CreateFileA(ctx->iso_output_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            LOG_ERR("Failed to create output ISO file");
            imapi_set_stat(ctx, "[ ERROR ] Create file");
            goto cleanup;
        }
        STATSTG stat;
        if (FAILED(pStream->Stat(&stat, STATFLAG_NONAME)))
        {
            CloseHandle(hFile);
            goto cleanup;
        }
        uint64_t totalBytes = stat.cbSize.QuadPart;
        uint64_t writtenBytes = 0;
        const DWORD BUF_SIZE = 1024 * 1024;
        char *buffer = new char[BUF_SIZE];
        DWORD bytesWritten;
        ULONG bytesRead;
        auto start_time = std::chrono::steady_clock::now();
        auto last_time = start_time;
        uint64_t last_bytes = 0;
        while (writtenBytes < totalBytes)
        {
            if (ctx->abort_requested)
            {
                imapi_set_stat(ctx, "Aborted");
                CloseHandle(hFile);
                DeleteFileA(ctx->iso_output_path.c_str());
                delete[] buffer;
                goto cleanup;
            }
            if (FAILED(pStream->Read(buffer, BUF_SIZE, &bytesRead)) || bytesRead == 0)
                break;
            if (!WriteFile(hFile, buffer, bytesRead, &bytesWritten, nullptr))
                break;
            writtenBytes += bytesWritten;
            ctx->progress_percent = (float)writtenBytes / totalBytes * 100.0f;
            auto now = std::chrono::steady_clock::now();
            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count();
            if (dt >= 1000)
            {
                ctx->write_speed_mbps = (float)(writtenBytes - last_bytes) / 1024.0f / 1024.0f / (dt / 1000.0f);
                ctx->elapsed_seconds = (uint32_t)std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
                last_bytes = writtenBytes;
                last_time = now;
            }
        }
        delete[] buffer;
        CloseHandle(hFile);
        if (writtenBytes >= totalBytes)
        {
            LOG_OK("ISO creation finished");
            imapi_set_stat(ctx, "Quick, wasn't it?");
            ctx->progress_percent = 100.0f;
        }
        else
        {
            LOG_ERR("ISO creation failed during write");
            imapi_set_stat(ctx, "[ ERROR ] Write failed");
        }
        goto cleanup;
    }
    if (!pDataWriter)
    {
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
            LOG_ERR("Could not find IMAPI recorder for drive");
            imapi_set_stat(ctx, "[ ERROR ] Drive not found");
            goto cleanup;
        }
        imapi_set_stat(ctx, "Preparing media...");
        hr = CoCreateInstance(clsid_msftdiscformattodata, nullptr, CLSCTX_ALL, IID_IDispatch, (void **)&pDataWriter);
        if (FAILED(hr))
        {
            LOG_ERR("Failed to create MsftDiscFormat2Data"); // m; Why would you ever log this?
            goto cleanup;                                    // c; why not?
                                                             // m; Sys would already be fucked here
                                                             // c; oki
        }
        {
            VARIANT vRec;
            VariantInit(&vRec);
            vRec.vt = VT_DISPATCH;
            vRec.pdispVal = pRecorder;
            hr = imapi_dispatch_put(pDataWriter, L"Recorder", &vRec);
            if (FAILED(hr))
            {
                LOG_ERR("Failed to attach recorder");
                goto cleanup;
            }
            VARIANT vClient;
            VariantInit(&vClient);
            vClient.vt = VT_BSTR;
            vClient.bstrVal = SysAllocString(L"build engine");
            imapi_dispatch_put(pDataWriter, L"ClientName", &vClient);
            VariantClear(&vClient);
        }
    }
    {
        VARIANT vFree;
        VariantInit(&vFree);
        if (SUCCEEDED(imapi_dispatch_get(pDataWriter, L"FreeSectorsOnMedia", &vFree)) && (vFree.vt == VT_I4 || vFree.vt == VT_UI4))
        {
            uint64_t free_bytes = (uint64_t)(uint32_t)vFree.lVal * 2048ULL;
            if (image_bytes > 0 && image_bytes > free_bytes)
            {
                LOG_ERRF("Image too large for disc: need %llu bytes, free %llu bytes", (unsigned long long)image_bytes, (unsigned long long)free_bytes); // m; These one-liners gotta stop man
                imapi_set_stat(ctx, "[ ERROR ] Image too large");
                VariantClear(&vFree);
                goto cleanup;
            }
        }
        VariantClear(&vFree);
    }
    {
        VARIANT vBlank;
        VariantInit(&vBlank);
        if (SUCCEEDED(imapi_dispatch_get(pDataWriter, L"MediaHeuristicallyBlank", &vBlank)) && vBlank.vt == VT_BOOL)
        {
            if (vBlank.boolVal == VARIANT_FALSE)
            {
                LOG_ERR("Disc is not blank; please insert a blank disc");
                imapi_set_stat(ctx, "[ ERROR ] Disc not blank");
                goto cleanup;
            }
        }
        VariantClear(&vBlank);
    }
    pEventSink = new build_event_sink(ctx);
    if (!imapi_connpt_advise(pDataWriter, imapi_iid_disc_data_events, pEventSink, &dwCookie))
        LOG_WARN("Failed to setup progress events");
    imapi_set_stat(ctx, "Starting burn...");
    LOG_INFO("Starting build n' burn");
    if (ctx->drive && ctx->drive->valid)
        drives_close(*ctx->drive);
    {
        VARIANT vStreamArg;
        VariantInit(&vStreamArg);
        vStreamArg.vt = VT_UNKNOWN;
        vStreamArg.punkVal = pStream;
        hr = imapi_dispatch_call(pDataWriter, L"Write", DISPATCH_METHOD, nullptr, &vStreamArg, 1);
    }
    if (SUCCEEDED(hr))
    {
        LOG_OK("Build n' burn finished");
        imapi_set_stat(ctx, "Quick, wasn't it?");
        ctx->progress_percent = 100.0f;
        if (ctx->eject_when_done && !ctx->verify_after_burn && pRecorder)
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
        LOG_WARN("Build aborted by user");
        imapi_set_stat(ctx, "Aborted");
    }
    else
    {
        LOG_ERRF("Build n' burn failed: 0x%08X", hr);
        imapi_set_stat(ctx, "[ ERROR ] Burn failed");
    }
cleanup:
    if (pEventSink && dwCookie && pDataWriter)
        imapi_connpt_rm(pDataWriter, imapi_iid_disc_data_events, dwCookie);
    if (pEventSink)
        pEventSink->Release();
    if (pStream)
        pStream->Release();
    if (pResult)
        pResult->Release();
    if (pRoot)
        pRoot->Release();
    if (pFSI)
        pFSI->Release();
    if (pDataWriter)
        pDataWriter->Release();
    if (pRecorder)
        pRecorder->Release();
    if (pDiscMaster)
        pDiscMaster->Release();
    CoUninitialize();
    if (ctx->drive && ctx->drive->valid)
        drives_close(*ctx->drive);
    LOG_INFO("Build engine shutting down");
    ctx->is_running = false;
}
void build_init(build_context &ctx, _build_list *files, drive_handle *drive, const std::string &volume_label, int speed, bool verify_mode, bool eject, iso_mode mode)
{
    if (ctx.worker_thread)
    {
        if (ctx.worker_thread->joinable())
            ctx.worker_thread->join();
        delete ctx.worker_thread;
        ctx.worker_thread = nullptr;
    }
    ctx.file_list = files;
    ctx.drive = drive;
    ctx.volume_label = volume_label;
    ctx.speed_multiplier = speed;
    ctx.verify_after_burn = verify_mode;
    ctx.eject_when_done = eject;
    ctx.build_to_iso = false;
    ctx.filesystem_mode = mode;
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
void build_init_iso(build_context &ctx, _build_list *files, const std::string &output_path, const std::string &volume_label, iso_mode mode)
{
    if (ctx.worker_thread)
    {
        if (ctx.worker_thread->joinable())
            ctx.worker_thread->join();
        delete ctx.worker_thread;
        ctx.worker_thread = nullptr;
    }
    ctx.file_list = files;
    ctx.drive = nullptr;
    ctx.iso_output_path = output_path;
    ctx.volume_label = volume_label;
    ctx.build_to_iso = true;
    ctx.filesystem_mode = mode;
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
bool build_start(build_context &ctx)
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
    ctx.worker_thread = new std::thread(build_thread_func, &ctx);
    return true;
}
void build_abort(build_context &ctx)
{
    if (ctx.is_running)
    {
        ctx.abort_requested = true;
        LOG_WARN("Abort requested");
    }
}

// end