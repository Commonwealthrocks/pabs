// __imapi_com.cpp
// last updated: 12/06/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "__imapi_com.hpp"
#include "logger.hpp"
#include <ocidl.h>
const CLSID imapi_clsid_disc_master = {0x2735412E, 0x7F64, 0x5B0F, {0x8F, 0x00, 0x5D, 0x77, 0xAF, 0xBE, 0x26, 0x1E}};
const CLSID imapi_clsid_disc_recorder = {0x2735412D, 0x7F64, 0x5B0F, {0x8F, 0x00, 0x5D, 0x77, 0xAF, 0xBE, 0x26, 0x1E}};
const IID imapi_iid_disc_data_events = {0x2735413C, 0x7F64, 0x5B0F, {0x8F, 0x00, 0x5D, 0x77, 0xAF, 0xBE, 0x26, 0x1E}};
std::wstring imapi_u8_to_w(const std::string &u8)
{
    if (u8.empty())
        return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), nullptr, 0);
    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), &wstr[0], size);
    return wstr;
}
HRESULT imapi_dispatch_call(IDispatch *d, const wchar_t *name, WORD flags, VARIANT *ret, VARIANT *args, UINT n_args)
{
    if (!d)
        return E_POINTER;
    DISPID dispid;
    LPOLESTR nm = (LPOLESTR)name;
    HRESULT hr = d->GetIDsOfNames(IID_NULL, &nm, 1, LOCALE_USER_DEFAULT, &dispid);
    if (FAILED(hr))
        return hr;
    DISPPARAMS dp = {nullptr, nullptr, 0, 0};
    DISPID putid = DISPID_PROPERTYPUT;
    if (n_args > 0)
    {
        dp.rgvarg = args;
        dp.cArgs = n_args;
        if (flags & DISPATCH_PROPERTYPUT)
        {
            dp.cNamedArgs = 1;
            dp.rgdispidNamedArgs = &putid;
        }
    }
    EXCEPINFO ex = {};
    UINT arg_err = 0;
    hr = d->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, flags, &dp, ret, &ex, &arg_err);
    if (ex.bstrDescription)
    {
        if (FAILED(hr))
        {
            int n = WideCharToMultiByte(CP_UTF8, 0, ex.bstrDescription, -1, nullptr, 0, nullptr, nullptr);
            std::string desc(n > 0 ? n - 1 : 0, 0);
            if (n > 0)
                WideCharToMultiByte(CP_UTF8, 0, ex.bstrDescription, -1, &desc[0], n, nullptr, nullptr);
            LOG_ERRF("COM '%ls': %s", name, desc.c_str());
        }
        SysFreeString(ex.bstrDescription);
    }
    if (ex.bstrSource)
        SysFreeString(ex.bstrSource);
    if (ex.bstrHelpFile)
        SysFreeString(ex.bstrHelpFile);
    return hr;
}
HRESULT imapi_dispatch_get(IDispatch *d, const wchar_t *name, VARIANT *out)
{
    return imapi_dispatch_call(d, name, DISPATCH_PROPERTYGET, out, nullptr, 0);
}
HRESULT imapi_dispatch_put(IDispatch *d, const wchar_t *name, VARIANT *arg)
{
    return imapi_dispatch_call(d, name, DISPATCH_PROPERTYPUT, nullptr, arg, 1);
}
HRESULT imapi_open_disc_master(IDispatch **out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    return CoCreateInstance(imapi_clsid_disc_master, nullptr, CLSCTX_ALL, IID_IDispatch, (void **)out);
}
HRESULT imapi_find_recorder(IDispatch *pDiscMaster, const std::string &drive_vol_path, IDispatch **ppRecorder)
{
    if (!pDiscMaster || !ppRecorder)
        return E_POINTER;
    *ppRecorder = nullptr;
    std::string target = drive_vol_path;
    if (target.length() >= 2)
        target = target.substr(0, 2);
    VARIANT vCount;
    VariantInit(&vCount);
    HRESULT hr = imapi_dispatch_get(pDiscMaster, L"Count", &vCount);
    if (FAILED(hr))
        return hr;
    if (vCount.vt != VT_I4)
        return E_UNEXPECTED;
    for (LONG i = 0; i < vCount.lVal; ++i)
    {
        VARIANT vIndex;
        VariantInit(&vIndex);
        vIndex.vt = VT_I4;
        vIndex.lVal = i;
        VARIANT vUniqueId;
        VariantInit(&vUniqueId);
        if (SUCCEEDED(imapi_dispatch_call(pDiscMaster, L"Item", DISPATCH_PROPERTYGET | DISPATCH_METHOD, &vUniqueId, &vIndex, 1)))
        {
            IDispatch *pTempRec = nullptr;
            if (SUCCEEDED(CoCreateInstance(imapi_clsid_disc_recorder, nullptr, CLSCTX_ALL, IID_IDispatch, (void **)&pTempRec)))
            {
                VARIANT vArgId;
                VariantInit(&vArgId);
                VariantCopy(&vArgId, &vUniqueId);
                if (SUCCEEDED(imapi_dispatch_call(pTempRec, L"InitializeDiscRecorder", DISPATCH_METHOD, nullptr, &vArgId, 1)))
                {
                    VARIANT vPaths;
                    VariantInit(&vPaths);
                    if (SUCCEEDED(imapi_dispatch_get(pTempRec, L"VolumePathNames", &vPaths)) && vPaths.vt == (VT_ARRAY | VT_VARIANT))
                    {
                        SAFEARRAY *psa = vPaths.parray;
                        VARIANT *pData;
                        if (SUCCEEDED(SafeArrayAccessData(psa, (void **)&pData)))
                        {
                            LONG lBound, uBound;
                            SafeArrayGetLBound(psa, 1, &lBound);
                            SafeArrayGetUBound(psa, 1, &uBound);
                            for (LONG j = lBound; j <= uBound; ++j)
                            {
                                if (pData[j].vt == VT_BSTR)
                                {
                                    std::wstring pathStr = pData[j].bstrVal;
                                    if (pathStr.length() >= 2)
                                    {
                                        std::string narrow(pathStr.begin(), pathStr.begin() + 2);
                                        if (_strnicmp(narrow.c_str(), target.c_str(), 2) == 0)
                                        {
                                            *ppRecorder = pTempRec;
                                            (*ppRecorder)->AddRef();
                                            break;
                                        }
                                    }
                                }
                            }
                            SafeArrayUnaccessData(psa);
                        }
                    }
                    VariantClear(&vPaths);
                }
                VariantClear(&vArgId);
                pTempRec->Release();
            }
            VariantClear(&vUniqueId);
        }
        if (*ppRecorder)
            break;
    }
    VariantClear(&vCount);
    return *ppRecorder ? S_OK : E_FAIL;
}
bool imapi_connpt_advise(IDispatch *src, REFIID conn_pt_iid, IUnknown *sink, DWORD *cookie)
{
    IConnectionPointContainer *cpc = nullptr;
    if (FAILED(src->QueryInterface(IID_IConnectionPointContainer, (void **)&cpc)))
        return false;
    IConnectionPoint *cp = nullptr;
    HRESULT hr = cpc->FindConnectionPoint(conn_pt_iid, &cp);
    cpc->Release();
    if (FAILED(hr))
        return false;
    hr = cp->Advise(sink, cookie);
    cp->Release();
    return SUCCEEDED(hr);
}
void imapi_connpt_rm(IDispatch *src, REFIID conn_pt_iid, DWORD cookie)
{
    IConnectionPointContainer *cpc = nullptr;
    if (FAILED(src->QueryInterface(IID_IConnectionPointContainer, (void **)&cpc)))
        return;
    IConnectionPoint *cp = nullptr;
    if (SUCCEEDED(cpc->FindConnectionPoint(conn_pt_iid, &cp)))
    {
        cp->Unadvise(cookie);
        cp->Release();
    }
    cpc->Release();
}

HRESULT what_the_fuck_does_my_reader_support(IDispatch *recorder, drive_capa *caps)
{
    if (!recorder || !caps)
        return E_POINTER;
    caps->read_cd = false;
    caps->read_dvd = false;
    caps->read_bd = false;
    caps->write_cd = false;
    caps->write_dvd = false;
    caps->write_bd = false;

    VARIANT vProfiles;
    VariantInit(&vProfiles);
    HRESULT hr = imapi_dispatch_get(recorder, L"SupportedProfiles", &vProfiles);
    if (FAILED(hr))
        return hr;

    if (vProfiles.vt == (VT_ARRAY | VT_VARIANT) && vProfiles.parray)
    {
        SAFEARRAY *psa = vProfiles.parray;
        VARIANT *pData;
        if (SUCCEEDED(SafeArrayAccessData(psa, (void **)&pData)))
        {
            LONG lBound, uBound;
            SafeArrayGetLBound(psa, 1, &lBound);
            SafeArrayGetUBound(psa, 1, &uBound);
            for (LONG i = lBound; i <= uBound; ++i)
            {
                if (pData[i].vt == VT_I4)
                {
                    LONG profile = pData[i].lVal;
                    if (profile == 0x0008)
                        caps->read_cd = true;
                    if (profile == 0x0009 || profile == 0x000a)
                    {
                        caps->read_cd = true;
                        caps->write_cd = true;
                    }
                    if (profile == 0x0010)
                        caps->read_dvd = true;
                    if (profile >= 0x0011 && profile <= 0x002b)
                    {
                        caps->read_dvd = true;
                        caps->write_dvd = true;
                    }
                    if (profile == 0x0040)
                        caps->read_bd = true;
                    if (profile >= 0x0041 && profile <= 0x0043)
                    {
                        caps->read_bd = true;
                        caps->write_bd = true;
                    }
                }
            }
            SafeArrayUnaccessData(psa);
        }
    }
    VariantClear(&vProfiles);
    return S_OK;
}

// end