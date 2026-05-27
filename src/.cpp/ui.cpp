// ui.cpp
// last updated: 27/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "ui.hpp"
#include "imgui.h"
#include "config.hpp"
#include "where_how_why_ffmpeg.hpp"
#include "logger.hpp"
#include "__image.hpp"
#include "__burn.hpp"
#include "__audio_h.hpp"
#include "__build.hpp"
#include "__read.hpp"
#include "__erase.hpp"
#include "__verify.hpp"
#include "outs.hpp"
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <winioctl.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <d3d11.h>
#include <fstream>
#include <sstream>
#include <random>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#pragma GCC diagnostic pop
static burn_context burn_ctx;
static read_context read_ctx;
static image_context image_ctx;
static drive_handle burn_drive = {};
static drive_handle read_drive = {};
static drive_handle build_drive = {};
static drive_handle erase_drive = {};
static drive_handle verify_drive = {};
static drive_handle audio_drive = {};
static build_context build_ctx;
static audio_context audio_ctx;
static erase_context erase_ctx;
static verify_context verify_ctx;
static _build_list build_files_disc;
static _build_list build_files_iso;
static audio_list audio_tracks;
static int selected_drive_idx = 0;
static std::atomic<bool> poll_disc_flags[PABS_MAX_DRIVES];
static std::atomic<bool> _poll_thread = {false};
static std::thread *poll_thread = nullptr;
static void poll_thread_func(drive_info *drives, int drive_count)
{
    while (_poll_thread.load())
    {
        for (int i = 0; i < drive_count; ++i)
            poll_disc_flags[i].store(drives_has_media(drives[i].path));
        for (int ms = 0; ms < 1500 && _poll_thread.load(); ms += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
static char volume_label_buf[256] = {};
static const char *iso_mode_label(int mode)
{
    switch (mode)
    {
    case 0:
        return "ISO9660";
    case 1:
        return "Joliet (9660)";
    case 2:
        return "UDF";
    case 3:
        return "Hybrid";
    default:
        return "Joliet (9660)";
    }
}
static iso_mode iso_config(int mode)
{
    if (mode == 0)
        return iso_mode::iso_9660;
    if (mode == 2)
        return iso_mode::udf;
    if (mode == 3)
        return iso_mode::hybrid;
    return iso_mode::joliet;
}
static const char *format_size(uint64_t bytes) // c; conversion logic for modes like "Files to ISO" or "Files to disc"
                                               // c; >> B -> KBs -> MBs -> GBs
                                               // m; What about petabytes?
                                               // c; are you trying to make me kill myself
{
    static char buf[32];
    if (bytes >= 1073741824ULL)
        snprintf(buf, sizeof(buf), "%.1f GB", bytes / 1073741824.0);
    else if (bytes >= 1048576ULL)
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024ULL)
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    return buf;
}
struct disc_info
{
    bool media_present = false;
    bool profile_known = false;
    uint16_t profile = 0;
    bool disc_info_we_kinda_know = false;
    uint8_t disc_status = 0xFF;
    uint8_t _state_session = 0xFF;
    bool is_eraseable = false;
    bool erasable = false;
    bool session_info = false;
    uint8_t _first_track = 0;
    uint8_t sessions = 0;
    uint8_t first_track_l_session = 0;
    uint8_t last_track_l_session = 0;
    bool read_capacity_ff = false;
    uint32_t total_sectors = 0;
    uint32_t sector_size = 2048;
    bool capacity_known = false;
    uint64_t capacity_bytes = 0;
    bool first_read_ok = false;
    bool data_in_1st_sector = false;
    bool atip_known = false;
    uint8_t atip_leadin_m = 0;
    uint8_t atip_leadin_s = 0;
    uint8_t atip_leadin_f = 0;
    uint8_t atip_leadout_m = 0;
    uint8_t atip_leadout_s = 0;
    uint8_t atip_leadout_f = 0;
    bool nwa_known = false;
    uint32_t next_writable_lba = 0;
};
static const char *disc_profile_str(uint16_t profile)
{ // c; what the fuck is a DVD-RAM?
  // m; My dad has one of them, that's why I accounted for it!
  // c; you're a fucking loser...
  // m; (Insert same boat gif)
    switch (profile)
    {
    case 0x0008:
        return "CD-ROM";
    case 0x0009:
        return "CD-R";
    case 0x000A:
        return "CD-RW";
    case 0x0010:
        return "DVD-ROM";
    case 0x0011:
        return "DVD-R sequential";
    case 0x0012:
        return "DVD-RAM";
    case 0x0013:
        return "DVD-RW restricted";
    case 0x0014:
        return "DVD-RW sequential";
    case 0x001A:
        return "DVD+RW";
    case 0x001B:
        return "DVD+R";
    case 0x002B:
        return "DVD+R DL";
    case 0x0040:
        return "BD-ROM";
    case 0x0041:
        return "BD-R";
    case 0x0043:
        return "BD-RE";
    default:
        return "unknown / other";
    }
}
static const char *disc_status_str(uint8_t status)
{
    switch (status & 0x03)
    {
    case 0:
        return "blank";
    case 1:
        return "appendable / incomplete";
    case 2:
        return "finalized / complete";
    default:
        return "idk";
    }
}
static const char *last_disc_sess_str(uint8_t state)
{
    switch (state & 0x03)
    {
    case 0:
        return "empty";
    case 1:
        return "incomplete";
    case 2:
        return "complete";
    default:
        return "idk";
    }
}
static bool _current_profile(HANDLE h, uint16_t &out_profile)
{
    BYTE buf[16] = {};
    sptd_packet pkt;
    sptd_init(pkt, buf, sizeof(buf), SCSI_IOCTL_DATA_IN);
    pkt.sptd.CdbLength = 10;
    pkt.sptd.Cdb[0] = 0x46;
    pkt.sptd.Cdb[1] = 0x02;
    pkt.sptd.Cdb[7] = 0x00;
    pkt.sptd.Cdb[8] = (BYTE)sizeof(buf);
    if (!sptd_send(h, pkt))
        return false;
    out_profile = (uint16_t)(((uint16_t)buf[6] << 8) | buf[7]);
    return true;
}
static bool _disc_info(HANDLE h, uint8_t &out_disc_status, uint8_t &out_last_session_state, bool &out_erasable, uint8_t &out_first_track, uint8_t &out_sessions, uint8_t &out_first_track_ls, uint8_t &out_last_track_ls)
{
    BYTE buf[64] = {};
    sptd_packet pkt;
    sptd_init(pkt, buf, sizeof(buf), SCSI_IOCTL_DATA_IN);
    pkt.sptd.CdbLength = 10;
    pkt.sptd.Cdb[0] = 0x51;
    pkt.sptd.Cdb[7] = 0x00;
    pkt.sptd.Cdb[8] = (BYTE)sizeof(buf);
    if (!sptd_send(h, pkt))
        return false;
    out_disc_status = (uint8_t)(buf[2] & 0x03);
    out_last_session_state = (uint8_t)((buf[2] >> 2) & 0x03);
    out_erasable = (buf[2] & 0x10) != 0;
    out_first_track = buf[3];
    out_sessions = buf[4];
    out_first_track_ls = buf[5];
    out_last_track_ls = buf[6];
    return true;
}
static bool _read_cap(HANDLE h, uint32_t &out_total_sectors, uint32_t &out_sector_size)
{
    BYTE buf[8] = {};
    sptd_packet pkt;
    sptd_init(pkt, buf, sizeof(buf), SCSI_IOCTL_DATA_IN);
    pkt.sptd.CdbLength = 10;
    pkt.sptd.Cdb[0] = 0x25;
    if (!sptd_send(h, pkt))
        return false;
    uint32_t last_lba = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];  // m; Could you make these lines any longer?
    uint32_t block_len = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | (uint32_t)buf[7]; // c; are you being serious or?
    if (block_len == 0)                                                                                                    // m; NO.
        return false;
    out_total_sectors = last_lba + 1;
    out_sector_size = block_len;
    return true;
}
static bool probe_atip(HANDLE h, uint8_t &out_in_m, uint8_t &out_in_s, uint8_t &out_in_f, uint8_t &out_out_m, uint8_t &out_out_s, uint8_t &out_out_f)
{
    BYTE buf[64] = {};
    sptd_packet pkt;
    sptd_init(pkt, buf, sizeof(buf), SCSI_IOCTL_DATA_IN);
    pkt.sptd.CdbLength = 10;
    pkt.sptd.Cdb[0] = 0x43;
    pkt.sptd.Cdb[1] = 0x02;
    pkt.sptd.Cdb[2] = 0x04;
    pkt.sptd.Cdb[7] = 0x00;
    pkt.sptd.Cdb[8] = (BYTE)sizeof(buf);
    if (!sptd_send(h, pkt))
        return false;
    out_in_m = buf[8];
    out_in_s = buf[9];
    out_in_f = buf[10];
    out_out_m = buf[12];
    out_out_s = buf[13];
    out_out_f = buf[14];
    return true;
}
static bool next_rw_address(HANDLE h, uint32_t &out_lba)
{
    BYTE buf[8] = {};
    sptd_packet pkt;
    sptd_init(pkt, buf, sizeof(buf), SCSI_IOCTL_DATA_IN);
    pkt.sptd.CdbLength = 10;
    pkt.sptd.Cdb[0] = 0x52;
    pkt.sptd.Cdb[1] = 0x01;
    pkt.sptd.Cdb[2] = 0x00;
    pkt.sptd.Cdb[3] = 0x00;
    pkt.sptd.Cdb[4] = 0x00;
    pkt.sptd.Cdb[5] = 0x01;
    pkt.sptd.Cdb[7] = 0x00;
    pkt.sptd.Cdb[8] = (BYTE)sizeof(buf);
    if (!sptd_send(h, pkt))
        return false;
    out_lba = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | (uint32_t)buf[7];
    return true;
}
static disc_info probe_disc(drive_handle &h)
{
    disc_info info = {};
    if (!h.valid)
        return info;
    info.media_present = drives_test_ready(h);
    if (!info.media_present)
        return info;
    uint16_t profile = 0;
    if (_current_profile(h.win_handle, profile))
    {
        info.profile_known = true;
        info.profile = profile;
    }
    uint8_t disc_status = 0;
    uint8_t last_session_state = 0;
    bool erasable = false;
    uint8_t first_track = 0;
    uint8_t sessions = 0;
    uint8_t first_track_l_session = 0;
    uint8_t last_track_l_session = 0;
    if (_disc_info(h.win_handle, disc_status, last_session_state, erasable, first_track, sessions, first_track_l_session, last_track_l_session))
    {
        info.disc_info_we_kinda_know = true;
        info.disc_status = disc_status;
        info._state_session = last_session_state;
        info.is_eraseable = true;
        info.erasable = erasable;
        info.session_info = true;
        info._first_track = first_track;
        info.sessions = sessions;
        info.first_track_l_session = first_track_l_session;
        info.last_track_l_session = last_track_l_session;
    }
    uint32_t sectors = 0;
    uint32_t sector_size = 2048;
    if (_read_cap(h.win_handle, sectors, sector_size))
    {
        info.read_capacity_ff = true;
        info.total_sectors = sectors;
        info.sector_size = sector_size;
        if (sectors > 64)
        {
            info.capacity_known = true;
            info.capacity_bytes = (uint64_t)sectors * (uint64_t)sector_size;
        }
    }
    else
    {
        GET_LENGTH_INFORMATION lenInfo = {};
        DWORD bytesReturned = 0;
        if (DeviceIoControl(h.win_handle, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &lenInfo, sizeof(lenInfo), &bytesReturned, nullptr))
        {
            uint64_t cap = (uint64_t)lenInfo.Length.QuadPart;
            if (cap >= 2048ULL * 64ULL)
            {
                info.capacity_known = true;
                info.capacity_bytes = cap;
                info.total_sectors = (uint32_t)(cap / 2048ULL);
                info.sector_size = 2048;
            }
        }
    }
    BYTE first_sector[2048] = {};
    DWORD read = 0;
    LARGE_INTEGER zero = {};
    if (SetFilePointerEx(h.win_handle, zero, nullptr, FILE_BEGIN) &&
        ReadFile(h.win_handle, first_sector, sizeof(first_sector), &read, nullptr) &&
        read > 0)
    {
        info.first_read_ok = true;
        for (DWORD i = 0; i < read; ++i)
        {
            if (first_sector[i] != 0)
            {
                info.data_in_1st_sector = true;
                break;
            }
        }
    }
    if (probe_atip(h.win_handle, info.atip_leadin_m, info.atip_leadin_s, info.atip_leadin_f, info.atip_leadout_m, info.atip_leadout_s, info.atip_leadout_f))
    {
        info.atip_known = true;
    }
    uint32_t nwa = 0;
    if (next_rw_address(h.win_handle, nwa))
    {
        info.nwa_known = true;
        info.next_writable_lba = nwa;
    }
    return info;
}
static void format_u32(uint32_t v, char *out, size_t out_sz)
{
    char tmp[32] = {};
    snprintf(tmp, sizeof(tmp), "%u", v);
    int len = (int)strlen(tmp);
    int commas = (len - 1) / 3;
    int out_len = len + commas;
    if ((size_t)(out_len + 1) > out_sz)
    {
        snprintf(out, out_sz, "%u", v);
        return;
    }
    out[out_len] = '\0';
    int i = len - 1, j = out_len - 1, group = 0;
    while (i >= 0)
    {
        out[j--] = tmp[i--];
        if (++group == 3 && i >= 0)
        {
            out[j--] = ',';
            group = 0;
        }
    }
}
static void style_setup() // c; this could be its own seperate file, then again it's my project
{
    ImGuiStyle &style = ImGui::GetStyle();
    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.15f, 0.15f, 0.15f, 0.94f);
    colors[ImGuiCol_Border] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.60f, 0.85f, 0.75f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
}
static ID3D11ShaderResourceView *load_svg_to_texture(ID3D11Device *device, const char *filepath, int width, int height)
{
    NSVGimage *image = nsvgParseFromFile(filepath, "px", 96.0f);
    if (!image)
        return nullptr;
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast)
    {
        nsvgDelete(image);
        return nullptr;
    }
    unsigned char *img_data = new unsigned char[width * height * 4];
    float minx = 1e10f, miny = 1e10f, maxx = -1e10f, maxy = -1e10f;
    for (NSVGshape *shape = image->shapes; shape != NULL; shape = shape->next)
    {
        if (!(shape->flags & NSVG_FLAGS_VISIBLE))
            continue;
        if (shape->bounds[0] < minx)
            minx = shape->bounds[0];
        if (shape->bounds[1] < miny)
            miny = shape->bounds[1];
        if (shape->bounds[2] > maxx)
            maxx = shape->bounds[2];
        if (shape->bounds[3] > maxy)
            maxy = shape->bounds[3];
    }
    float vb_sx = (float)width / image->width;
    float vb_sy = (float)height / image->height;
    float vb_scale = vb_sx < vb_sy ? vb_sx : vb_sy;
    float tx = 0, ty = 0, scale = vb_scale;
    float content_w = maxx - minx;
    float content_h = maxy - miny;
    if (content_w > 0.01f && content_h > 0.01f)
    {
        float pad = 0.05f;
        float padx = content_w * pad;
        float pady = content_h * pad;
        minx -= padx;
        miny -= pady;
        content_w += padx * 2.0f;
        content_h += pady * 2.0f;
        float cb_sx = (float)width / content_w;
        float cb_sy = (float)height / content_h;
        float cb_scale = cb_sx < cb_sy ? cb_sx : cb_sy;
        scale = cb_scale;
        tx = ((float)width - content_w * scale) * 0.5f - minx * scale;
        ty = ((float)height - content_h * scale) * 0.5f - miny * scale;
    }
    nsvgRasterize(rast, image, tx, ty, scale, img_data, width, height, width * 4);
    for (int i = 0; i < width * height * 4; i += 4)
    {
        img_data[i] = 255;
        img_data[i + 1] = 255;
        img_data[i + 2] = 255;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA subResource = {};
    subResource.pSysMem = img_data;
    subResource.SysMemPitch = desc.Width * 4;
    subResource.SysMemSlicePitch = 0;
    ID3D11Texture2D *pTexture = nullptr;
    device->CreateTexture2D(&desc, &subResource, &pTexture);
    ID3D11ShaderResourceView *out_srv = nullptr;
    if (pTexture)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = desc.MipLevels;
        srvDesc.Texture2D.MostDetailedMip = 0;
        device->CreateShaderResourceView(pTexture, &srvDesc, &out_srv);
        pTexture->Release();
    }
    delete[] img_data;
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    return out_srv;
}
static ID3D11ShaderResourceView *tex_write = nullptr;
static ID3D11ShaderResourceView *tex_read = nullptr;
static ID3D11ShaderResourceView *tex_build = nullptr;
static ID3D11ShaderResourceView *tex_verify = nullptr;
static ID3D11ShaderResourceView *tex_discovery_mode = nullptr;
static ID3D11ShaderResourceView *tex_burn = nullptr;
static ID3D11ShaderResourceView *tex_logs = nullptr;
static ID3D11ShaderResourceView *tex_reload = nullptr;
static ID3D11ShaderResourceView *tex_quit = nullptr;
static ID3D11ShaderResourceView *tex_iso_to_disc = nullptr;
static ID3D11ShaderResourceView *tex_disc_to_iso = nullptr;
static ID3D11ShaderResourceView *tex_files_to_disc = nullptr;
static ID3D11ShaderResourceView *tex_files_to_iso = nullptr;
static ID3D11ShaderResourceView *tex_settings = nullptr;
static ID3D11ShaderResourceView *tex_save = nullptr;
static ID3D11ShaderResourceView *tex_clean = nullptr;
static ID3D11ShaderResourceView *tex_abort = nullptr;
static ID3D11ShaderResourceView *tex_s_sector = nullptr;
static ID3D11ShaderResourceView *tex_create = nullptr;
static char verify_compare_path[4096] = "";
ID3D11ShaderResourceView *tex_about = nullptr;
static ID3D11ShaderResourceView *tex_boardkey = nullptr;
static ID3D11ShaderResourceView *tex_quick_info = nullptr;
static ID3D11ShaderResourceView *tex_audio_disc = nullptr;
static int session_write_speed = 0;
static int session_read_speed = 0;
static bool session_verify = false;
static bool session_eject = true;
static int session_write_mode = 0;
static bool session_mode_select = false;
static int session_iso_mode = 0;
static char vol_label[256] = {};
static bool drive_check = false;
static bool drive_missing = false;
static void reset_from_config(bool disc_is_cd = false)
// m; You think our style will be annoying to audit/review?
// c; yeah we'll make it readable when activision puts wolfenstein 2009 back on sale
// m; So never?
// c; pretty much
{
    session_write_speed = disc_is_cd ? _config.cd_write_speed : _config.dvd_write_speed;
    session_read_speed = disc_is_cd ? _config.cd_read_speed : _config.dvd_read_speed;
    session_verify = _config.verify_after_burn;
    session_eject = _config.eject_when_done;
    session_write_mode = _config.write_mode;
    session_mode_select = _config.use_mode_select;
    session_iso_mode = _config.iso_mode;
    snprintf(vol_label, sizeof(vol_label), "%s", _config.volume_label.c_str());
}
static std::string current_quote = "";
bool ui_init(ID3D11Device *device)
{
    config_load(_config);
    cache_audio_sw();
    set_sfx_vol();
    style_setup();
    snprintf(burn_ctx.status_text, sizeof(burn_ctx.status_text), "Idle");
    if (!_config.last_image_path.empty())
    {
        image_open(image_ctx, _config.last_image_path.c_str());
        image_validate(image_ctx);
        if (image_ctx.is_valid && !image_ctx.volume_id.empty())
        {
            snprintf(vol_label, sizeof(vol_label), "%s", image_ctx.volume_id.c_str());
        }
    }
    char exe_dir[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe_dir, MAX_PATH);
    char *sep = strrchr(exe_dir, '\\');
    if (sep)
        *(sep + 1) = '\0';
    parse_tooltips(exe_dir);
    char quote_path[MAX_PATH];
    snprintf(quote_path, MAX_PATH, "%sassets/txt_data/p_quotes", exe_dir);
    std::ifstream q_file(quote_path);
    if (q_file.is_open())
    {
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(q_file, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                lines.push_back(line);
        }
        if (!lines.empty())
        {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, lines.size() - 1);
            current_quote = lines[dis(gen)];
        }
    }
    if (current_quote.empty())
        current_quote = "Uh oh...";
    auto load_svg = [&](const char *rel_path, int w, int h) -> ID3D11ShaderResourceView *
    {
        char full[MAX_PATH];
        snprintf(full, MAX_PATH, "%sassets/%s", exe_dir, rel_path);
        return load_svg_to_texture(device, full, w, h);
    };
    tex_write = load_svg("imgs/write.svg", 96, 96);
    tex_read = load_svg("imgs/read.svg", 96, 96);
    tex_build = load_svg("imgs/build.svg", 96, 96);
    tex_verify = load_svg("imgs/checkmark.svg", 96, 96);
    tex_discovery_mode = load_svg("imgs/disc.svg", 96, 96);
    tex_burn = load_svg("imgs/burn.svg", 48, 48);
    tex_logs = load_svg("imgs/logs.svg", 24, 24);
    tex_reload = load_svg("imgs/reload.svg", 24, 24);
    tex_quit = load_svg("imgs/quit.svg", 24, 24);
    tex_iso_to_disc = load_svg("imgs/iso_to_disc.svg", 120, 60);
    tex_disc_to_iso = load_svg("imgs/disc_to_iso.svg", 120, 60);
    tex_files_to_disc = load_svg("imgs/files_to_disc.svg", 120, 60);
    tex_files_to_iso = load_svg("imgs/files_to_iso.svg", 120, 60);
    tex_settings = load_svg("imgs/settings.svg", 24, 24);
    tex_save = load_svg("imgs/save.svg", 32, 32);   // c; still blurry what the FUCK is wrong with this 😭😭
    tex_clean = load_svg("imgs/clean.svg", 24, 24); // m; I give up too.
    tex_abort = load_svg("imgs/abort.svg", 64, 64); // c; holy FUCK i fixed it and don't ask me how cause i don't know
    tex_s_sector = load_svg("imgs/s_sector.svg", 24, 24);
    tex_create = load_svg("imgs/create.svg", 24, 24);
    tex_about = load_svg("imgs/about.svg", 24, 24);
    tex_boardkey = load_svg("imgs/boardkey.svg", 24, 24);
    tex_quick_info = load_svg("imgs/quick_info.svg", 24, 24); // c; will fix later
    tex_audio_disc = load_svg("imgs/audio_disc.svg", 32, 32); // c; this too
    strncpy(volume_label_buf, _config.volume_label.c_str(), sizeof(volume_label_buf) - 1);
    reset_from_config();
    snprintf(build_ctx.status_text, sizeof(build_ctx.status_text), "Idle");
    LOG_OK("GUI initialized");
    return true;
}
void poll_drives_start(drive_info *drives, int drive_count)
{
    for (int i = 0; i < PABS_MAX_DRIVES; ++i)
        poll_disc_flags[i].store(false);
    if (drive_count > 0)
    {
        for (int i = 0; i < drive_count; ++i)
            poll_disc_flags[i].store(drives[i].disc_present);
        _poll_thread.store(true);
        poll_thread = new std::thread(poll_thread_func, drives, drive_count);
    }
}
void gui_shutdown()
{
    _poll_thread.store(false);
    if (poll_thread)
    {
        if (poll_thread->joinable())
            poll_thread->join();
        delete poll_thread;
        poll_thread = nullptr;
    }
    if (burn_ctx.worker_thread)
    {
        burn_ctx.abort_requested = true;
        if (burn_ctx.worker_thread->joinable())
            burn_ctx.worker_thread->join();
        delete burn_ctx.worker_thread;
        burn_ctx.worker_thread = nullptr;
    }
    if (read_ctx.worker_thread)
    {
        read_ctx.abort_requested = true;
        if (read_ctx.worker_thread->joinable())
            read_ctx.worker_thread->join();
        delete read_ctx.worker_thread;
        read_ctx.worker_thread = nullptr;
    }
    if (build_ctx.worker_thread)
    {
        build_ctx.abort_requested = true;
        if (build_ctx.worker_thread->joinable())
            build_ctx.worker_thread->join();
        delete build_ctx.worker_thread;
        build_ctx.worker_thread = nullptr;
    }
    if (audio_ctx.worker_thread)
    {
        audio_ctx.abort_requested = true;
        if (audio_ctx.worker_thread->joinable())
            audio_ctx.worker_thread->join();
        delete audio_ctx.worker_thread;
        audio_ctx.worker_thread = nullptr;
    }
    image_close(image_ctx);
    if (burn_drive.valid)
        drives_close(burn_drive);
    if (read_drive.valid)
        drives_close(read_drive);
    if (build_drive.valid)
        drives_close(build_drive);
    if (tex_write)
        tex_write->Release();
    if (tex_read)
        tex_read->Release();
    if (tex_build)
        tex_build->Release();
    if (tex_verify)
        tex_verify->Release();
    if (tex_discovery_mode)
        tex_discovery_mode->Release();
    if (tex_burn)
        tex_burn->Release();
    if (tex_logs)
        tex_logs->Release();
    if (tex_reload)
        tex_reload->Release();
    if (tex_quit)
        tex_quit->Release();
    if (tex_iso_to_disc)
        tex_iso_to_disc->Release();
    if (tex_disc_to_iso)
        tex_disc_to_iso->Release();
    if (tex_files_to_disc)
        tex_files_to_disc->Release();
    if (tex_files_to_iso)
        tex_files_to_iso->Release();
    if (tex_settings)
        tex_settings->Release();
    if (tex_save)
        tex_save->Release();
    if (tex_clean)
        tex_clean->Release();
    if (tex_abort)
        tex_abort->Release();
    if (tex_s_sector)
        tex_s_sector->Release();
    if (tex_create)
        tex_create->Release();
    if (tex_about)
        tex_about->Release();
    if (tex_boardkey)
        tex_boardkey->Release();
    if (tex_quick_info)
        tex_quick_info->Release();
    if (tex_audio_disc)
        tex_audio_disc->Release();
    LOG_OK("GUI shutting down");
}
static void show_browse_dialog()
{
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "Disc images\0*.iso;*.bin;*.cue;*.img;*.ccd;*.mds;*.mdf;*.nrg;*.toast;*.cdr;*.raw\0All files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = "iso";
    char initial_dir[MAX_PATH] = {0};
    if (!_config.last_image_path.empty())
    {
        snprintf(initial_dir, sizeof(initial_dir), "%s", _config.last_image_path.c_str());
        if (PathRemoveFileSpecA(initial_dir))
            ofn.lpstrInitialDir = initial_dir;
    }
    else if (!_config.last_browse_dir.empty())
    {
        snprintf(initial_dir, sizeof(initial_dir), "%s", _config.last_browse_dir.c_str());
        ofn.lpstrInitialDir = initial_dir;
    }
    if (GetOpenFileNameA(&ofn))
    {
        snprintf(initial_dir, sizeof(initial_dir), "%s", filename);
        if (PathRemoveFileSpecA(initial_dir))
            _config.last_browse_dir = initial_dir;
        _config.last_image_path = filename;
        image_close(image_ctx);
        image_open(image_ctx, filename);
        image_validate(image_ctx);
        if (image_ctx.is_valid && !image_ctx.volume_id.empty())
        {
            snprintf(vol_label, sizeof(vol_label), "%s", image_ctx.volume_id.c_str());
        }
        LOG_INFOF("GUI: selected image %s", filename);
    }
}
static void show_save_dialog()
{
    char filename[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "ISO files\0*.iso\0All fles\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = "iso";
    char initial_dir[MAX_PATH] = {0};
    if (!_config.last_read_path.empty())
    {
        snprintf(initial_dir, sizeof(initial_dir), "%s", _config.last_read_path.c_str());
        if (PathRemoveFileSpecA(initial_dir))
            ofn.lpstrInitialDir = initial_dir;
    }
    else if (!_config.last_browse_dir.empty())
    {
        snprintf(initial_dir, sizeof(initial_dir), "%s", _config.last_browse_dir.c_str());
        ofn.lpstrInitialDir = initial_dir;
    }
    if (GetSaveFileNameA(&ofn))
    {
        _config.last_read_path = filename;
        snprintf(initial_dir, sizeof(initial_dir), "%s", filename);
        if (PathRemoveFileSpecA(initial_dir))
            _config.last_browse_dir = initial_dir;
        LOG_INFOF("GUI: selected save path %s", filename);
    }
}
static void open_img_path(const char *path)
{
    if (!path || !*path)
        return;
    _config.last_image_path = path;
    image_close(image_ctx);
    image_open(image_ctx, path);
    image_validate(image_ctx);
    if (image_ctx.is_valid && !image_ctx.volume_id.empty())
    {
        snprintf(vol_label, sizeof(vol_label), "%s", image_ctx.volume_id.c_str());
    }
    LOG_INFOF("GUI: loaded image %s", path);
}
static bool has_any_ext_ci(const char *path, const char *ext)
{
    if (!path || !ext)
        return false;
    const char *dot = strrchr(path, '.');
    if (!dot)
        return false;
    return _stricmp(dot, ext) == 0;
}
static bool is_img_file(const char *path)
{
    static const char *exts[] = {".iso", ".bin", ".cue", ".img", ".ccd", ".mds", ".mdf", ".nrg", ".toast", ".cdr", ".raw", nullptr};
    for (int i = 0; exts[i]; ++i)
        if (has_any_ext_ci(path, exts[i]))
            return true;
    return false;
}
static bool _is_audio(const char *path)
{
    static const char *exts[] = {".wav", ".mp3", ".flac", ".ogg", ".opus", ".oga", ".m4a", ".aac", ".wma", ".ape", ".wv", ".ac3", ".eac3", ".tta", ".aiff", ".aif", ".mp2", ".mka", nullptr};
    for (int i = 0; exts[i]; ++i)
        if (has_any_ext_ci(path, exts[i]))
            return true;
    return false;
}
static const char *validate_vol_label(const char *lbl, int iso_mode_sel)
{
    int len = (int)strlen(lbl);
    if (len == 0)
        return nullptr;
    int max_len = (iso_mode_sel == 1) ? 16 : 11;
    if (len > max_len)
    {
        static char msg[64];
        snprintf(msg, sizeof(msg), "Label too long (max %d chars for this ISO mode)", max_len);
        return msg;
    }
    for (int i = 0; i < len; ++i)
    {
        char c = lbl[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok)
            return "Only A-Z, 0-9 and _ are allowed";
    }
    return nullptr;
}
static bool is_status(const char *status, const char *needle)
{
    if (!status || !needle)
        return false;
    return strcmp(status, needle) == 0;
}
static void show_friendly_build_error_dialog(const char *mode_title, const char *status)
{
    if (is_status(status, "[ ERROR ] Image too large"))
    {
        cd_error("ERROR", "The selected files are too big to fit on\nthis disc.");
        return;
    }
    if (is_status(status, "[ ERROR ] Disc not blank"))
    {
        cd_error("ERROR", "The inserted disc is not blank, try a different one.");
        return;
    }
    if (is_status(status, "[ ERROR ] ISO mode invalid for BD"))
    {
        cd_error("ERROR", "Blu-ray media in this app requires UDF filesystem mode.\nSwitch ISO mode to UDF and retry.");
        return;
    }
    cd_error("ERROR", "The operation failed; check the logs for details.");
}
enum class app_mode
{
    starter_sector, // c; yo i THOUGHT we agreed to keep this named "imgburn_default_mode_lol"
    read_mode,      // m; I got too lazy typing that over and over.
    write_mode,     // c; is your keyboard missing the tab key?
    build_mode,
    build_iso,
    audio_cd,
    verify_mode,
    are_you,
    settings
};
static app_mode _app_mode = app_mode::starter_sector;
static app_mode _pending_mode = app_mode::starter_sector;
static bool show_discard = false;
static bool settings_dirty = false;
static pabs_config temp_config;
static char temp_label[256] = {};
static char ffmpeg_ov_ui[520] = {};
static void request_mode_change(app_mode new_mode)
{
    if (_app_mode == app_mode::settings && settings_dirty && new_mode != app_mode::settings)
    {
        _pending_mode = new_mode;
        show_discard = true;
    }
    else
    {
        _app_mode = new_mode;
        const char *title_suffix = "starter sector";
        switch (new_mode)
        {
        case app_mode::starter_sector:
            title_suffix = "starter sector";
            break;
        case app_mode::read_mode:
            title_suffix = "disc to ISO mode";
            break;
        case app_mode::build_mode:
            title_suffix = "files to disc mode";
            break;
        case app_mode::write_mode:
            title_suffix = "ISO to disc mode";
            break;
        case app_mode::verify_mode:
            title_suffix = "verify disc mode";
            break;
        case app_mode::are_you:
            title_suffix = "are you?";
            break;
        case app_mode::settings:
            title_suffix = "settings tab";
            break;
        case app_mode::build_iso:
            title_suffix = "files to ISO mode";
            break;
        case app_mode::audio_cd:
            title_suffix = "audio CD mode";
            break;
        default:
            break;
        }
        char window_title[256];
        snprintf(window_title, sizeof(window_title), "PABS - PYROFOREVER's actual burning software - %s", title_suffix);
        HWND hwnd = (HWND)ImGui::GetMainViewport()->PlatformHandleRaw;
        if (hwnd)
            SetWindowTextA(hwnd, window_title);
    }
}
static bool cen_img_button(const char *id, ID3D11ShaderResourceView *tex, const char *text, ImVec2 size, ImVec2 tex_size = ImVec2(64, 64))
{
    bool clicked = false;
    ImGui::PushID(id);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    if (ImGui::Button("", size))
        clicked = true;
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    float offset_y = active ? 2.0f : (hovered ? -2.0f : 0.0f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (tex)
    {
        ImVec2 img_pos(pos.x + (size.x - tex_size.x) * 0.5f, pos.y + ((76.0f - tex_size.y) * 0.5f) + 4.0f + offset_y);
        dl->AddImage((ImTextureID)tex, img_pos, ImVec2(img_pos.x + tex_size.x, img_pos.y + tex_size.y));
    }
    ImVec2 text_size = ImGui::CalcTextSize(text);
    ImVec2 text_pos(pos.x + (size.x - text_size.x) * 0.5f, pos.y + 76 + offset_y);
    ImU32 text_col = hovered ? ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)) : ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddText(text_pos, text_col, text);
    ImGui::PopID();
    return clicked;
}
static bool button_w_icon(const char *id, ID3D11ShaderResourceView *tex, const char *text, ImVec2 size)
{
    bool clicked = false;
    ImGui::PushID(id);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    if (ImGui::Button("", size))
        clicked = true;
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    float offset_y = active ? 2.0f : (hovered ? -1.0f : 0.0f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 tex_size(32, 32);
    ImVec2 text_size = ImGui::CalcTextSize(text);
    float actual_width = (size.x <= 0) ? ImGui::GetContentRegionAvail().x : size.x;
    float total_content_w = tex_size.x + 15.0f + text_size.x;
    float start_x = pos.x + (actual_width - total_content_w) * 0.5f;
    if (tex)
    {
        ImVec2 img_pos(start_x, pos.y + (size.y - tex_size.y) * 0.5f + offset_y);
        dl->AddImage((ImTextureID)tex, img_pos, ImVec2(img_pos.x + tex_size.x, img_pos.y + tex_size.y));
    }
    ImVec2 text_pos(start_x + tex_size.x + 15.0f, pos.y + (size.y - text_size.y) * 0.5f + offset_y);
    ImU32 text_col = hovered ? ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)) : ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddText(text_pos, text_col, text);
    ImGui::PopID();
    return clicked;
}
static int CALLBACK browse_folder_cb(HWND hwnd, UINT uMsg, LPARAM, LPARAM lpData)
{
    if (uMsg == BFFM_INITIALIZED && lpData)
    {
        const char *path = (const char *)lpData;
        if (path && *path)
            SendMessageA(hwnd, BFFM_SETSELECTIONA, TRUE, (LPARAM)path);
    }
    return 0;
}
static void show_ffmpeg_exe_browse()
{
    char buf[MAX_PATH] = {};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = "ffmpeg\0ffmpeg.exe\0Executeables\0*.exe\0All\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    char initial_dir[MAX_PATH] = {};
    if (ffmpeg_ov_ui[0])
    {
        snprintf(initial_dir, sizeof(initial_dir), "%s", ffmpeg_ov_ui);
        DWORD a = GetFileAttributesA(ffmpeg_ov_ui);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
            ofn.lpstrInitialDir = ffmpeg_ov_ui;
        else if (PathRemoveFileSpecA(initial_dir))
            ofn.lpstrInitialDir = initial_dir;
    }
    else if (!_config.last_browse_dir.empty())
        ofn.lpstrInitialDir = _config.last_browse_dir.c_str();
    if (GetOpenFileNameA(&ofn))
    {
        snprintf(ffmpeg_ov_ui, sizeof(ffmpeg_ov_ui), "%s", buf);
        settings_dirty = true;
        char d[MAX_PATH];
        snprintf(d, sizeof(d), "%s", buf);
        if (PathRemoveFileSpecA(d))
            _config.last_browse_dir = d;
    }
}
void gui_render(drive_info drives[], int drive_count)
{
    auto combo_label = [&](int idx) -> const char *
    {
        static char buf[256];
        if (idx < 0 || idx >= drive_count)
            return "No drives found";
        snprintf(buf, sizeof(buf), "%s - %s %s [%s] %s", drives[idx].path, drives[idx].vendor, drives[idx].product, drives_type_str(drives[idx].type), drives[idx].disc_present ? "(media present)" : "(empty)");
        return buf;
    };
    auto _status_line = [&](bool engine_running)
    {
        if (engine_running)
        {
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "working...");
        }
        else if (drive_count > 0 && selected_drive_idx >= 0 && selected_drive_idx < drive_count && drives[selected_drive_idx].disc_present)
        {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Ready");
            ImGui::SameLine();
            ImGui::TextDisabled("- media present");
        }
        else if (drive_count > 0)
        {
            ImGui::TextColored(ImVec4(0.85f, 0.4f, 0.4f, 1.0f), "no disc");
        }
    };
    ImGuiIO &io = ImGui::GetIO();
    for (int i = 0; i < drive_count; ++i)
        drives[i].disc_present = poll_disc_flags[i].load();
    if (drive_count > 0 && selected_drive_idx >= drive_count)
    {
        selected_drive_idx = 0;
    }
    else if (drive_count == 0)
    {
        selected_drive_idx = 0;
    }
    bool any_engine_running = burn_ctx.is_running || read_ctx.is_running ||
                              build_ctx.is_running || verify_ctx.is_running ||
                              erase_ctx.is_running || audio_ctx.is_running;
    auto on_erase_clicked = [&]()
    {
        if (drive_count == 0)
        {
            LOG_ERR("Erase requested but no reader / writer found");
            cd_error("ERROR", "No reader / writer :(");
        }
        else if (!drives[selected_drive_idx].disc_present)
        {
            LOG_ERR("Erase requested but no disc is present");
            cd_error("ERROR", "No disc in the first place :(");
        }
        else
        {
            if (drives[selected_drive_idx].type == drive_type::cdrom ||
                drives[selected_drive_idx].type == drive_type::dvdrom ||
                drives[selected_drive_idx].type == drive_type::bdrom ||
                drives[selected_drive_idx].type == drive_type::unknown)
            {
                LOG_ERR("Erase requested but drive does not support erasing");
                cd_error("ERROR", "Not a -RW disc :(");
            }
            else
            {
                if (!burn_is_rw(drives[selected_drive_idx].path))
                {
                    LOG_ERR("Erase requested but inserted media is not rewritable");
                    cd_error("ERROR", "Not a -RW disc :(");
                }
                else
                {
                    LOG_INFO("Erase requested and media is rewritable; prompting user"); // c; about time
                    cd_clean_disc();
                }
            }
        }
    };
    if (_app_mode == app_mode::starter_sector) // c; imgburn default mode
    {
    }
    if (ImGui::BeginMainMenuBar())
    {
        float mb_h = ImGui::GetFrameHeight();
        auto CustomMenu = [&](const char *label) -> bool
        {
            ImVec2 text_size = ImGui::CalcTextSize(label);
            bool is_open = ImGui::IsPopupOpen(label);
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
            if (ImGui::Selectable(label, is_open, 0, ImVec2(text_size.x + 8, 0)))
            {
                if (is_open)
                    ImGui::CloseCurrentPopup();
                else
                    ImGui::OpenPopup(label);
            }
            ImGui::PopStyleColor();
            ImVec2 item_pos = ImGui::GetItemRectMin();
            if (ImGui::IsPopupOpen(label))
            {
                ImGui::SetNextWindowPos(ImVec2(item_pos.x, item_pos.y + mb_h));
            }
            bool open = ImGui::BeginPopup(label, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
            if (open)
            {
                bool popup_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_RootAndChildWindows);
                bool menubar_hovered = (ImGui::GetMousePos().y <= mb_h);
                if (!popup_hovered && !menubar_hovered && ImGui::IsMouseClicked(0))
                {
                    ImGui::CloseCurrentPopup();
                }
            }
            return open;
        };
        if (CustomMenu("Mode"))
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (tex_s_sector)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_s_sector, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            if (ImGui::MenuItem("      Starter sector", NULL, _app_mode == app_mode::starter_sector))
                request_mode_change(app_mode::starter_sector);
            ImGui::Separator();
            p = ImGui::GetCursorScreenPos();
            if (tex_read)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_read, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            if (ImGui::MenuItem("      Read", NULL, _app_mode == app_mode::read_mode))
                request_mode_change(app_mode::read_mode);
            p = ImGui::GetCursorScreenPos();
            if (tex_build)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_build, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            if (ImGui::MenuItem("      Build", NULL, _app_mode == app_mode::build_mode))
                request_mode_change(app_mode::build_mode);
            p = ImGui::GetCursorScreenPos();
            if (tex_write)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_write, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            if (ImGui::MenuItem("      Write", NULL, _app_mode == app_mode::write_mode))
                request_mode_change(app_mode::write_mode);
            p = ImGui::GetCursorScreenPos();
            if (tex_verify)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_verify, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            if (ImGui::MenuItem("      Verify", NULL, _app_mode == app_mode::verify_mode))
                request_mode_change(app_mode::verify_mode);
            p = ImGui::GetCursorScreenPos();
            if (tex_audio_disc)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_audio_disc, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            if (ImGui::MenuItem("      Audio", NULL, _app_mode == app_mode::audio_cd))
                request_mode_change(app_mode::audio_cd);
            p = ImGui::GetCursorScreenPos();
            if (tex_discovery_mode)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_discovery_mode, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            if (ImGui::MenuItem("      Are you?", NULL, _app_mode == app_mode::are_you)) // m; Are you?
                request_mode_change(app_mode::are_you);                                  // c; are you?
            ImGui::EndPopup();
        }
        if (CustomMenu("Action"))
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (tex_logs)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_logs, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            bool log_vis = log_window_visib();
            if (ImGui::MenuItem("      Logs", NULL, log_vis))
                show_log_window(!log_vis);
            p = ImGui::GetCursorScreenPos();
            if (tex_settings)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_settings, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            if (ImGui::MenuItem("      Settings"))
                request_mode_change(app_mode::settings);
            p = ImGui::GetCursorScreenPos();
            if (tex_clean)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_clean, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            ImGui::BeginDisabled(any_engine_running);
            if (ImGui::MenuItem("      Erase disc"))
            {
                on_erase_clicked();
            }
            ImGui::EndDisabled();
            if (any_engine_running && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                const char *busy =
                    burn_ctx.is_running ? "burning" : read_ctx.is_running ? "reading"
                                                  : build_ctx.is_running  ? "building"
                                                  : verify_ctx.is_running ? "verifying"
                                                                          : "erasing";
                ImGui::SetTooltip("Disc is busy (%s)", busy);
            }
            ImGui::Separator();
            p = ImGui::GetCursorScreenPos();
            if (tex_reload)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_reload, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            if (ImGui::MenuItem("      Reload software"))
            {
                LOG_INFO("GUI: reload software clicked; resetting state...");
                config_load(_config);
                reset_from_config();
                drive_check = false;
                cd_ok("Reload software", "The software state and configuration have been reloaded."); // c; does this even do anything?
                                                                                                      // m; Yeah, check it out!
                                                                                                      // c; oh shit you were right.
            }
            p = ImGui::GetCursorScreenPos();
            if (tex_quit)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_quit, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            if (ImGui::MenuItem("      Quit"))
            {
                PostQuitMessage(0);
            }
            ImGui::EndPopup();
        }
        if (CustomMenu("Work"))
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (tex_create)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_create, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            if (ImGui::MenuItem("      Create CUE file..."))
                _trigger_mk_cue();
            p = ImGui::GetCursorScreenPos();
            if (tex_create)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_create, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            if (ImGui::MenuItem("      Create DVD file..."))
                _trigger_mk_dvd();
            ImGui::EndPopup();
        }
        if (CustomMenu("Help"))
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (tex_about)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_about, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            if (ImGui::MenuItem("      About PABS"))
                _trigger_about();
            p = ImGui::GetCursorScreenPos();
            if (tex_boardkey)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_boardkey, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            if (ImGui::MenuItem("      Keybinds"))
                _trigger_keybinds();
            p = ImGui::GetCursorScreenPos();
            if (tex_quick_info)
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_quick_info, ImVec2(p.x + 4, p.y + 1), ImVec2(p.x + 18, p.y + 15));
            if (ImGui::MenuItem("      Quick features"))
                _trigger_quick_features();
            ImGui::EndPopup();
        }
        ImGui::EndMainMenuBar();
    }
    float menu_bar_height = ImGui::GetFrameHeight();
    ImGuiViewport *mv = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(mv->WorkPos.x, mv->WorkPos.y + menu_bar_height));
    ImGui::SetNextWindowSize(ImVec2(mv->WorkSize.x, mv->WorkSize.y - menu_bar_height));
    ImGui::SetNextWindowViewport(mv->ID);
    ImGuiWindowFlags main_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;
    ImGui::Begin("Main", nullptr, main_flags);
    if (!any_engine_running && !ImGui::IsPopupOpen(NULL, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        if (_app_mode != app_mode::starter_sector)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(3) || ImGui::IsMouseClicked(4))
            {
                request_mode_change(app_mode::starter_sector);
            }
        }
        if (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_S))
                request_mode_change(app_mode::settings);
            if (ImGui::IsKeyPressed(ImGuiKey_W))
                request_mode_change(app_mode::write_mode);
            if (ImGui::IsKeyPressed(ImGuiKey_R))
                request_mode_change(app_mode::read_mode);
            if (ImGui::IsKeyPressed(ImGuiKey_D))
                request_mode_change(app_mode::build_mode);
            if (ImGui::IsKeyPressed(ImGuiKey_I))
                request_mode_change(app_mode::build_iso);
            if (ImGui::IsKeyPressed(ImGuiKey_C))
                request_mode_change(app_mode::audio_cd);
            if (ImGui::IsKeyPressed(ImGuiKey_V))
                request_mode_change(app_mode::verify_mode);
            if (ImGui::IsKeyPressed(ImGuiKey_A))
                request_mode_change(app_mode::are_you);
            if (ImGui::IsKeyPressed(ImGuiKey_Q))
                _trigger_mk_cue();
            if (ImGui::IsKeyPressed(ImGuiKey_P))
                _trigger_mk_dvd();
        }
    }
    static app_mode prev_mode = app_mode::starter_sector; // m; We enter here.
    if (_app_mode != prev_mode)
    {
        if (_app_mode == app_mode::write_mode || _app_mode == app_mode::build_mode ||
            _app_mode == app_mode::build_iso || _app_mode == app_mode::read_mode || _app_mode == app_mode::audio_cd)
        {
            reset_from_config();
        }
        else if (_app_mode == app_mode::settings)
        {
            temp_config = _config;
            snprintf(temp_label, sizeof(temp_label), "%s", _config.volume_label.c_str());
            snprintf(ffmpeg_ov_ui, sizeof(ffmpeg_ov_ui), "%s", temp_config.ffmpeg_override.c_str());
            settings_dirty = false;
        }
        prev_mode = _app_mode;
    }
    if (show_discard)
    {
        ImGui::OpenPopup("Woah there!");
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + ImGui::GetMainViewport()->WorkSize.x * 0.5f, ImGui::GetMainViewport()->WorkPos.y + ImGui::GetMainViewport()->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Woah there!", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::Text("Your settings haven't been saved, are you sure you want to discard them?");
        ImGui::Spacing();
        if (ImGui::Button("Yeah", ImVec2(120, 0)))
        {
            settings_dirty = false;
            show_discard = false;
            _app_mode = _pending_mode;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Eh, no", ImVec2(120, 0)))
        {
            show_discard = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!drive_check && drive_count > 0 && !_config.default_drive_path.empty())
    {
        drive_check = true;
        bool found = false;
        for (int i = 0; i < drive_count; ++i)
        {
            if (_config.default_drive_path == drives[i].path)
            {
                selected_drive_idx = i;
                found = true;
                break;
            }
        }
        if (!found)
        {
            drive_missing = true;
            if (!_config.mute_warning_sfx)
            {
                PlaySoundA("SystemExclamation", NULL, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
            }
            LOG_WARNF("Default drive %s not found; falling back to first drive", _config.default_drive_path.c_str());
        }
    }
    else if (!drive_check && drive_count > 0)
    {
        drive_check = true;
    }
    if (drive_missing)
        ImGui::OpenPopup("##drive_missing");
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + ImGui::GetMainViewport()->WorkSize.x * 0.5f, ImGui::GetMainViewport()->WorkPos.y + ImGui::GetMainViewport()->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##drive_missing", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
    {
        ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.3f, 1.0f), "drive not reachable");
        ImGui::Separator();
        ImGui::Text("The default drive  \"%s\"  was not found.", _config.default_drive_path.c_str());
        ImGui::Text("Falling back to the first available drive.");
        ImGui::Spacing();
        if (ImGui::Button("  OK  "))
        {
            drive_missing = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (_app_mode == app_mode::starter_sector)
    {
        if (!current_quote.empty())
        {
            ImFont *quote_font = ImGui::GetIO().Fonts->Fonts.Size > 1 ? ImGui::GetIO().Fonts->Fonts[1] : nullptr;
            if (quote_font)
                ImGui::PushFont(quote_font);
            ImVec2 quote_size = ImGui::CalcTextSize(current_quote.c_str());
            float saved_y = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(32.0f);
            ImGui::SetCursorPosX((io.DisplaySize.x - quote_size.x) * 0.5f);
            ImGui::TextDisabled("%s", current_quote.c_str());
            if (quote_font)
                ImGui::PopFont();
            ImGui::SetCursorPosY(saved_y);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(20, 20));
        float btn_w = 200.0f;
        float btn_h = 100.0f;
        float total_h = btn_h * 3 + 40.0f;
        float avail_y = io.DisplaySize.y - ImGui::GetFrameHeight();
        float offset_y = (avail_y - total_h) * 0.25f; // m; Could be displayed better, but this can do for now.
        if (offset_y > 0.0f)
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset_y);
        ImGui::SetCursorPosX((io.DisplaySize.x - (btn_w * 2 + 20)) * 0.5f);
        ImGui::BeginGroup();
        if (cen_img_button("##btn_iso_to_disc", tex_iso_to_disc, "ISO to disc", ImVec2(btn_w, btn_h), ImVec2(120, 60)))
            request_mode_change(app_mode::write_mode);
        ImGui::SameLine();
        if (cen_img_button("##btn_disc_to_iso", tex_disc_to_iso, "Disc to ISO", ImVec2(btn_w, btn_h), ImVec2(120, 60)))
            request_mode_change(app_mode::read_mode);
        ImGui::SetCursorPosX((io.DisplaySize.x - (btn_w * 2 + 20)) * 0.5f);
        if (cen_img_button("##btn_files_to_disc", tex_files_to_disc, "Files to disc", ImVec2(btn_w, btn_h), ImVec2(120, 60)))
            request_mode_change(app_mode::build_mode);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            request_mode_change(app_mode::audio_cd);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Right click this button to burn an audio CD instead.");
        ImGui::SameLine();
        if (cen_img_button("##btn_files_to_iso", tex_files_to_iso, "Files to ISO", ImVec2(btn_w, btn_h), ImVec2(120, 60)))
            request_mode_change(app_mode::build_iso);
        ImGui::SetCursorPosX((io.DisplaySize.x - (btn_w * 2 + 20)) * 0.5f);
        if (cen_img_button("##btn_verify", tex_verify, "Verify disc", ImVec2(btn_w, btn_h), ImVec2(48, 48)))
            request_mode_change(app_mode::verify_mode);
        ImGui::SameLine();
        if (cen_img_button("##btn_discovery_mode", tex_discovery_mode, "Are you?", ImVec2(btn_w, btn_h), ImVec2(48, 48)))
            request_mode_change(app_mode::are_you);
        ImGui::EndGroup();
        ImGui::PopStyleVar();
    }
    else if (_app_mode == app_mode::read_mode) // c; disc to iso
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
        ImGui::BeginDisabled(any_engine_running);
        ImGui::BeginChild("source", ImVec2(0, 85), true);
        ImGui::TextDisabled("Source");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Drive:");
        ImGui::SameLine();
        if (drive_count > 0 && selected_drive_idx < drive_count)
        {
            ImGui::SetNextItemWidth(-30);
            if (ImGui::BeginCombo("##drive_combo", combo_label(selected_drive_idx)))
            {
                for (int i = 0; i < drive_count; i++)
                {
                    bool is_selected = (selected_drive_idx == i);
                    if (ImGui::Selectable(combo_label(i), is_selected))
                        selected_drive_idx = i;
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::ImageButton("##erase_read", (ImTextureID)tex_clean, ImVec2(16, 16)))
            {
                on_erase_clicked();
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Erase disc");
        }
        else
        {
            ImGui::TextDisabled("no reader / writer found");
        }
        _status_line(read_ctx.is_running);
        ImGui::EndChild();
        ImGui::BeginChild("output", ImVec2(0, 75), true);
        ImGui::TextDisabled("Output");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Image:");
        ImGui::SameLine();
        char path_buf[MAX_PATH];
        strncpy(path_buf, _config.last_read_path.c_str(), sizeof(path_buf) - 1);
        ImGui::SetNextItemWidth(-70);
        if (ImGui::InputText("##img_path", path_buf, sizeof(path_buf), ImGuiInputTextFlags_ReadOnly))
        {
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(60, 0)))
        {
            show_save_dialog();
        }
        ImGui::EndChild();
        ImGui::BeginChild("settings", ImVec2(0, 80), true);
        ImGui::TextDisabled("Settings");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Read speed:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        const int read_speeds[] = {1, 2, 4, 8, 16, 24, 32, 48};
        char spd_label[16];
        if (session_read_speed == 0)
            snprintf(spd_label, sizeof(spd_label), "Max");
        else
            snprintf(spd_label, sizeof(spd_label), "%dx", session_read_speed);
        if (ImGui::BeginCombo("##speed", spd_label))
        {
            if (ImGui::Selectable("Max", session_read_speed == 0))
                session_read_speed = 0;
            for (int s : read_speeds)
            {
                char item[16];
                snprintf(item, sizeof(item), "%dx", s);
                if (ImGui::Selectable(item, session_read_speed == s))
                    session_read_speed = s;
            }
            ImGui::EndCombo();
        }
        ImGui::EndChild();
        ImGui::SetCursorPos(ImVec2(10.0f, ImGui::GetWindowHeight() - 110.0f));
        if (read_ctx.is_running)
        {
            ImGui::EndDisabled();
            if (button_w_icon("##btn_abort_read", tex_abort, "Abort", ImVec2(130, 45)))
                __cd_abort(false);
            ImGui::BeginDisabled(true);
        }
        else
        {
            if (button_w_icon("##btn_read", tex_read, "Read", ImVec2(130, 45)))
            {
                if (drive_count <= 0 || _config.last_read_path.empty())
                {
                    LOG_WARN("No valid drive and (or) output path");
                    cd_error("ERROR", "No valid drive and (or) output path was selected.");
                }
                else if (!drives[selected_drive_idx].disc_present)
                {
                    cd_error("ERROR", "No disc is present in the drive in the first place.");
                }
                else
                {
                    drive_handle test_h = drives_open(drives[selected_drive_idx].path);
                    bool ready = test_h.valid && drives_test_ready(test_h);
                    if (test_h.valid)
                        drives_close(test_h);
                    if (!ready)
                    {
                        cd_error("ERROR", "Perhaps a disc is still loading? Come again!");
                    }
                    else
                    {
                        read_drive = drives_open(drives[selected_drive_idx].path);
                        if (!read_drive.valid)
                        {
                            LOG_ERR("Failed to open drive for reading");
                        }
                        else
                        {
                            read_init(read_ctx, &read_drive, _config.last_read_path);
                            read_start(read_ctx);
                        }
                    }
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
    }
    else if (_app_mode == app_mode::write_mode) // c; iso to disc
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
        ImGui::BeginDisabled(any_engine_running);
        ImGui::BeginChild("source", ImVec2(0, 60), true);
        ImGui::TextDisabled("Source");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Image:");
        ImGui::SameLine();
        char path_buf[MAX_PATH];
        strncpy(path_buf, _config.last_image_path.c_str(), sizeof(path_buf) - 1);
        ImGui::SetNextItemWidth(-70);
        if (ImGui::InputText("##img_path", path_buf, sizeof(path_buf), ImGuiInputTextFlags_ReadOnly))
        {
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(60, 0)))
        {
            show_browse_dialog();
        }
        ImGui::EndChild();
        ImGui::BeginChild("output", ImVec2(0, 90), true);
        ImGui::TextDisabled("Output");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Drive:");
        ImGui::SameLine();
        if (drive_count > 0 && selected_drive_idx < drive_count)
        {
            ImGui::SetNextItemWidth(-30);
            if (ImGui::BeginCombo("##drive_combo", combo_label(selected_drive_idx)))
            {
                for (int i = 0; i < drive_count; i++)
                {
                    bool is_selected = (selected_drive_idx == i);
                    if (ImGui::Selectable(combo_label(i), is_selected))
                    {
                        selected_drive_idx = i;
                    }
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::ImageButton("##erase_write", (ImTextureID)tex_clean, ImVec2(16, 16)))
            {
                on_erase_clicked();
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Erase disc");
            if (burn_ctx.is_running)
            {
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "working...");
            }
            else if (drives[selected_drive_idx].disc_present)
            {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Ready");
                ImGui::SameLine();
                ImGui::TextDisabled("- media present");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.85f, 0.4f, 0.4f, 1.0f), "no disc");
            }
        }
        else
        {
            ImGui::TextDisabled("no reader / writer found");
        }
        ImGui::EndChild();
        ImGui::BeginChild("settings", ImVec2(0, 200), true);
        ImGui::TextDisabled("Settings");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Volume label:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        {
            if (image_ctx.has_udf)
            {
                ImGui::BeginDisabled(true);
                ImGui::InputText("##vollabel_write", vol_label, sizeof(vol_label));
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("UDF filesystem detected.\nModifying the volume label on the fly would invalidate UDF checksums and corrupt the disc.");
            }
            else
            {
                const char *lbl_err = validate_vol_label(vol_label, _config.iso_mode);
                if (lbl_err)
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
                ImGui::InputText("##vollabel_write", vol_label, sizeof(vol_label));
                if (lbl_err)
                {
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                        ImGui::SetTooltip("%s", lbl_err);
                }
            }
        }
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Write speed:");
        ImGui::SameLine();
        static std::vector<int> empty_spd;
        bool has_spd = drive_count > 0 && !drives[selected_drive_idx].supported_write_speeds.empty();
        ImGui::SetNextItemWidth(80);
        {
            char spd_label[16];
            if (session_write_speed == 0)
                snprintf(spd_label, sizeof(spd_label), "Max");
            else
                snprintf(spd_label, sizeof(spd_label), "%dx", session_write_speed);
            if (ImGui::BeginCombo("##speed", spd_label))
            {
                if (ImGui::Selectable("Max", session_write_speed == 0))
                    session_write_speed = 0;
                if (has_spd)
                {
                    for (int s : drives[selected_drive_idx].supported_write_speeds)
                    {
                        char item[16];
                        snprintf(item, sizeof(item), "%dx", s);
                        if (ImGui::Selectable(item, session_write_speed == s))
                            session_write_speed = s;
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::Checkbox("Verify after burn", &session_verify);
        ImGui::Checkbox("Eject tray when done", &session_eject);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Write mode:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        if (ImGui::BeginCombo("##writemode", session_write_mode == 0 ? "DAO" : "TAO"))
        {
            if (ImGui::Selectable("DAO", session_write_mode == 0))
                session_write_mode = 0;
            if (ImGui::Selectable("TAO", session_write_mode == 1))
                session_write_mode = 1;
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Use MODE SELECT", &session_mode_select);
        {
            const char *detected = "no image loaded";
            if (image_ctx.is_valid)
            {
                if (image_ctx.has_udf)
                    detected = "UDF";
                else if (image_ctx.has_joliet)
                    detected = "Joliet (ISO 9660)";
                else if (image_ctx.has_iso9660)
                    detected = "ISO9660";
                else
                    detected = "idk";
            }
            ImGui::AlignTextToFramePadding();
            ImGui::Text("ISO mode:");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", detected);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("Filesystem mode is determined by the image.\nRe-burn with a different mode in Files-to-disc or Files-to-ISO.");
        }
        ImGui::EndChild();
        ImGui::SetCursorPos(ImVec2(10.0f, ImGui::GetWindowHeight() - 110.0f));
        if (burn_ctx.is_running)
        {
            ImGui::EndDisabled();
            if (button_w_icon("##btn_abort_burn", tex_abort, "Abort", ImVec2(130, 45)))
                __cd_abort(true);
            ImGui::BeginDisabled(true);
        }
        else
        {
            if (button_w_icon("##btn_burn", tex_burn, "Burn", ImVec2(130, 45))) // c; https://www.youtube.com/watch?v=jgbf03MJyS4&list=RDjgbf03MJyS4&start_radio=1
            {
                if (!image_ctx.is_valid)
                {
                    cd_error("ERROR", "No valid ISO / image file is selected.");
                }
                else if (drive_count <= 0 || selected_drive_idx < 0 || selected_drive_idx >= drive_count)
                {
                    cd_error("ERROR", "No reader / writer selected."); // m; Are the gaps between the slash intentional?
                                                                       // c; yeah
                }
                else if (!drives[selected_drive_idx].disc_present)
                {
                    cd_error("ERROR", "No disc is present in the drive in the first place.");
                }
                else
                {
                    drive_handle test_h = drives_open(drives[selected_drive_idx].path);
                    bool ready = test_h.valid && drives_test_ready(test_h);
                    if (test_h.valid)
                        drives_close(test_h);
                    if (!ready)
                    {
                        cd_error("ERROR", "Perhaps a disc is still loading? Come again!");
                    }
                    else
                    {
                        if (burn_drive.valid)
                            drives_close(burn_drive);
                        burn_drive = drives_open(drives[selected_drive_idx].path);
                        if (!burn_drive.valid)
                        {
                            LOG_ERR("[ ERROR ] Failed to open drive for writing.");
                        }
                        else
                        {
                            burn_options opt;
                            opt.speed_multiplier = session_write_speed;
                            opt.verify_after_burn = session_verify;
                            opt.eject_when_done = session_eject;
                            opt.write_mode = session_write_mode;
                            opt.use_mode_select = session_mode_select;
                            opt.volume_label = std::string(vol_label);
                            burn_init(burn_ctx, &burn_drive, &image_ctx, opt);
                            burn_start(burn_ctx);
                        }
                    }
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
    }
    else if (_app_mode == app_mode::build_mode || _app_mode == app_mode::build_iso) // c; files to iso
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
        ImGui::BeginDisabled(any_engine_running);
        _build_list &cur_list = (_app_mode == app_mode::build_iso) ? build_files_iso : build_files_disc;
        ImGui::TextDisabled("Files to add (drag n' drop here)");
        float list_h = ImGui::GetWindowHeight() - 380.0f;
        if (list_h < 60.0f)
            list_h = 60.0f;
        ImGui::BeginChild("file_list", ImVec2(0, list_h), true);
        if (ImGui::IsWindowFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A))
        {
            for (auto &e : cur_list.entries)
                e.selected = true;
        }
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
        {
            for (auto &e : cur_list.entries)
                e.selected = false;
        }
        static const int k_render_cap = 20000;
        int render_n = (int)cur_list.entries.size();
        if (render_n > k_render_cap)
            render_n = k_render_cap;
        for (int i = 0; i < render_n; ++i)
        {
            build_entry &e = cur_list.entries[i];
            char label[512];
            const char *icon = e.is_directory ? "[ DIR ]" : "     ";
            if (e.is_directory)
                snprintf(label, sizeof(label), "%s  %-40s  %s", icon, e.display_name.c_str(), format_size(e.size_bytes));
            else
                snprintf(label, sizeof(label), "%s  %-40s  %s", icon, e.display_name.c_str(), format_size(e.size_bytes));
            if (ImGui::Selectable(label, e.selected))
            {
                if (!ImGui::GetIO().KeyCtrl)
                {
                    for (auto &x : cur_list.entries)
                        x.selected = false;
                }
                e.selected = !e.selected;
            }
        }
        if (cur_list.entries.empty())
        {
            ImGui::TextDisabled("\n\n\n\n                drop files or folders here, or use the buttons below");
        }
        else if ((int)cur_list.entries.size() > k_render_cap)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("file / directory show limit is capped at %d to counter GUI lag", k_render_cap); // c; mental note since i added this to PyKryptor
        }
        ImGui::EndChild();
        float btn_w = (ImGui::GetContentRegionAvail().x - 24.0f) / 4.0f;
        if (ImGui::Button("Add file(s)", ImVec2(btn_w, 0)))
        {
            char filename[4096] = "";
            OPENFILENAMEA ofn;
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = NULL;
            ofn.lpstrFilter = "All files\0*.*\0";
            ofn.lpstrFile = filename;
            ofn.nMaxFile = sizeof(filename);
            ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT;
            char initial_dir[MAX_PATH] = {0};
            if (!_config.last_browse_dir.empty())
            {
                strncpy(initial_dir, _config.last_browse_dir.c_str(), sizeof(initial_dir) - 1);
                ofn.lpstrInitialDir = initial_dir;
            }
            if (GetOpenFileNameA(&ofn))
            {
                if (filename[0])
                {
                    _config.last_browse_dir = filename;
                }
                if (filename[strlen(filename) + 1] == '\0')
                {
                    char dir_buf[MAX_PATH] = {0};
                    strncpy(dir_buf, filename, sizeof(dir_buf) - 1);
                    if (PathRemoveFileSpecA(dir_buf))
                        _config.last_browse_dir = dir_buf;
                    cur_list.add_path(filename);
                }
                else
                {
                    char *dir = filename;
                    char *file = filename + strlen(dir) + 1;
                    while (*file)
                    {
                        char full[MAX_PATH];
                        snprintf(full, MAX_PATH, "%s\\%s", dir, file);
                        cur_list.add_path(full);
                        file += strlen(file) + 1;
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Add folder(s)", ImVec2(btn_w, 0)))
        {
            BROWSEINFOA bi = {};
            bi.lpszTitle = "Select a folder to add";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            bi.lpfn = browse_folder_cb;
            const char *init = _config.last_browse_dir.empty() ? nullptr : _config.last_browse_dir.c_str();
            bi.lParam = (LPARAM)init;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
            if (pidl)
            {
                char path[MAX_PATH];
                if (SHGetPathFromIDListA(pidl, path))
                {
                    _config.last_browse_dir = path;
                    cur_list.add_path(path);
                }
                CoTaskMemFree(pidl);
            }
        }
        ImGui::SameLine();
        {
            bool has_selected = false;
            for (const auto &e : cur_list.entries)
            {
                if (e.selected)
                {
                    has_selected = true;
                    break;
                }
            }
            ImGui::BeginDisabled(!has_selected);
            if (ImGui::Button("Remove selected", ImVec2(btn_w, 0)))
            {
                cur_list.rm_selected();
            }
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(cur_list.entries.empty());
        if (ImGui::Button("Clear all", ImVec2(btn_w, 0)))
        {
            cur_list.clear();
        }
        ImGui::EndDisabled();
        uint64_t total = cur_list.total_size();
        ImGui::TextDisabled("%d item(s) - %s total", (int)cur_list.entries.size(), format_size(total));
        ImGui::BeginChild("output", ImVec2(0, 77), true);
        ImGui::TextDisabled("Output");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        if (_app_mode == app_mode::build_mode)
        {
            ImGui::Text("Drive:");
            ImGui::SameLine();
            if (drive_count > 0 && selected_drive_idx < drive_count)
            {
                ImGui::SetNextItemWidth(-30);
                if (ImGui::BeginCombo("##drive_combo", combo_label(selected_drive_idx)))
                {
                    for (int i = 0; i < drive_count; i++)
                    {
                        bool is_selected = (selected_drive_idx == i);
                        if (ImGui::Selectable(combo_label(i), is_selected))
                            selected_drive_idx = i;
                        if (is_selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                if (ImGui::ImageButton("##erase_build", (ImTextureID)tex_clean, ImVec2(16, 16)))
                {
                    on_erase_clicked();
                }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Erase disc");
                _status_line(build_ctx.is_running);
            }
            else
            {
                ImGui::TextDisabled("no reader / writer found");
            }
        }
        else
        {
            ImGui::Text("Output ISO:");
            ImGui::SameLine();
            char path_buf[MAX_PATH];
            strncpy(path_buf, _config.last_read_path.c_str(), sizeof(path_buf) - 1);
            ImGui::SetNextItemWidth(-70);
            if (ImGui::InputText("##out_iso_path", path_buf, sizeof(path_buf), ImGuiInputTextFlags_ReadOnly))
            {
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse...", ImVec2(60, 0)))
            {
                show_save_dialog();
            }
        }
        ImGui::EndChild();
        if (_app_mode == app_mode::build_mode) // c; files to disc
        {
            ImGui::BeginChild("settings", ImVec2(0, 100), true);
            ImGui::TextDisabled("Settings [SCROLLABLE]");
            ImGui::Separator();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Volume label:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            {
                const char *lbl_err = validate_vol_label(volume_label_buf, session_iso_mode);
                if (lbl_err)
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
                ImGui::InputText("##vollabel", volume_label_buf, sizeof(volume_label_buf));
                if (lbl_err)
                {
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                        ImGui::SetTooltip("%s", lbl_err);
                }
            }
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Write speed:");
            ImGui::SameLine();
            bool has_spd = drive_count > 0 && selected_drive_idx >= 0 && selected_drive_idx < drive_count && !drives[selected_drive_idx].supported_write_speeds.empty();
            ImGui::SetNextItemWidth(80);
            {
                char spd_label[16];
                if (session_write_speed == 0)
                    snprintf(spd_label, sizeof(spd_label), "Max");
                else
                    snprintf(spd_label, sizeof(spd_label), "%dx", session_write_speed);
                if (ImGui::BeginCombo("##build_speed", spd_label))
                {
                    if (ImGui::Selectable("Max", session_write_speed == 0))
                        session_write_speed = 0;
                    if (has_spd)
                    {
                        for (int s : drives[selected_drive_idx].supported_write_speeds)
                        {
                            char item[16];
                            snprintf(item, sizeof(item), "%dx", s);
                            if (ImGui::Selectable(item, session_write_speed == s))
                                session_write_speed = s;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::Checkbox("Verify after burn", &session_verify);
            ImGui::Checkbox("Eject tray when done", &session_eject);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("ISO mode:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140);
            if (ImGui::BeginCombo("##build_iso_mode", iso_mode_label(session_iso_mode)))
            {
                if (ImGui::Selectable("ISO9660", session_iso_mode == 0))
                    session_iso_mode = 0;
                if (ImGui::Selectable("Joliet (9660)", session_iso_mode == 1))
                    session_iso_mode = 1;
                if (ImGui::Selectable("UDF", session_iso_mode == 2))
                    session_iso_mode = 2;
                if (ImGui::Selectable("Hybrid", session_iso_mode == 3))
                    session_iso_mode = 3;
                ImGui::EndCombo();
            }
            ImGui::EndChild();
        }
        else
        {
            ImGui::BeginChild("settings", ImVec2(0, 85), true);
            ImGui::TextDisabled("Settings");
            ImGui::Separator();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Volume label:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            {
                const char *lbl_err = validate_vol_label(volume_label_buf, session_iso_mode);
                if (lbl_err)
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
                ImGui::InputText("##iso_vollabel", volume_label_buf, sizeof(volume_label_buf));
                if (lbl_err)
                {
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                        ImGui::SetTooltip("%s", lbl_err);
                }
            }
            ImGui::AlignTextToFramePadding();
            ImGui::Text("ISO mode:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140);
            if (ImGui::BeginCombo("##iso_only_mode", iso_mode_label(session_iso_mode)))
            {
                if (ImGui::Selectable("ISO9660", session_iso_mode == 0))
                    session_iso_mode = 0;
                if (ImGui::Selectable("Joliet (9660)", session_iso_mode == 1))
                    session_iso_mode = 1;
                if (ImGui::Selectable("UDF", session_iso_mode == 2))
                    session_iso_mode = 2;
                if (ImGui::Selectable("Hybrid", session_iso_mode == 3))
                    session_iso_mode = 3;
                ImGui::EndCombo();
            }
            ImGui::EndChild();
        }
        ImGui::SetCursorPos(ImVec2(10.0f, ImGui::GetWindowHeight() - 110.0f));
        if (build_ctx.is_running)
        {
            ImGui::EndDisabled();
            if (button_w_icon("##btn_abort_build", tex_abort, "Abort", ImVec2(130, 45)))
                __cd_abort(true);
            ImGui::BeginDisabled(true);
        }
        else
        {
            if (_app_mode == app_mode::build_mode)
            {
                if (button_w_icon("##btn_build", tex_burn, "Build n' burn", ImVec2(170, 45)))
                {
                    if (validate_vol_label(volume_label_buf, session_iso_mode))
                    {
                        cd_error("ERROR", "Volume label is invalid.\nHover over the label box for details.");
                    }
                    else if (cur_list.entries.empty())
                    {
                        cd_error("ERROR", "No files were added to build.");
                    }
                    else if (drive_count <= 0 || selected_drive_idx >= drive_count)
                    {
                        cd_error("ERROR", "No reader / writer is selected.");
                    }
                    else if (!drives[selected_drive_idx].disc_present)
                    {
                        cd_error("ERROR", "No disc appears to be present in the selected reader / writer.");
                    }
                    else if ((drives[selected_drive_idx].type == drive_type::bdrom || drives[selected_drive_idx].type == drive_type::bdrw) && session_iso_mode != 2)
                    {
                        cd_error("ERROR", "Blu-Ray media requires UDF ISO mode in this app.\nSwitch ISO mode to UDF and retry.");
                    }
                    else
                    {
                        drive_handle test_h = drives_open(drives[selected_drive_idx].path);
                        bool ready = test_h.valid && drives_test_ready(test_h);
                        if (test_h.valid)
                            drives_close(test_h);
                        if (!ready)
                        {
                            cd_error("ERROR", "Perhaps a disc is still loading? Come again!");
                        }
                        else
                        {
                            if (build_drive.valid)
                                drives_close(build_drive);
                            build_drive = drives_open(drives[selected_drive_idx].path);
                            if (!build_drive.valid)
                            {
                                LOG_ERR("Failed to open drive");
                            }
                            else
                            {
                                build_init(build_ctx, &cur_list, &build_drive, std::string(volume_label_buf), session_write_speed, session_verify, session_eject, iso_config(session_iso_mode));
                                build_start(build_ctx);
                            }
                        }
                    }
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                {
                    request_mode_change(app_mode::audio_cd);
                }
            }
            else
            {
                if (button_w_icon("##btn_build_iso", tex_build, "Build ISO", ImVec2(150, 45)))
                {
                    if (validate_vol_label(volume_label_buf, session_iso_mode))
                    {
                        cd_error("ERROR", "Volume label is invalid.\nHover over the label box for details.");
                    }
                    else if (cur_list.entries.empty())
                    {
                        cd_error("ERROR", "No files added to build.");
                    }
                    else if (_config.last_read_path.empty())
                    {
                        cd_error("ERROR", "No output ISO path selected.");
                    }
                    else
                    {
                        build_init_iso(build_ctx, &cur_list, _config.last_read_path, std::string(volume_label_buf), iso_config(session_iso_mode));
                        build_start(build_ctx);
                    }
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
    }
    else if (_app_mode == app_mode::audio_cd) // c; audio cd
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
        ImGui::BeginDisabled(any_engine_running);

        ImGui::TextDisabled("Audio tracks (drag n' drop audio files here)");
        float list_h = ImGui::GetWindowHeight() - 380.0f;
        if (list_h < 60.0f)
            list_h = 60.0f;
        ImGui::BeginChild("audio_track_list", ImVec2(0, list_h), true);

        {
            std::lock_guard<std::mutex> lk(audio_tracks.list_mutex);
            if (ImGui::IsWindowFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A))
            {
                for (auto &e : audio_tracks.entries)
                    e.selected = true;
            }
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
            {
                for (auto &e : audio_tracks.entries)
                    e.selected = false;
            }
            for (int i = 0; i < (int)audio_tracks.entries.size(); ++i)
            {
                audio_track &e = audio_tracks.entries[i];
                char label[512];
                if (e.is_converting)
                    snprintf(label, sizeof(label), "%02d.  %-40s  (converting...)", i + 1, e.display_name.c_str());
                else
                    snprintf(label, sizeof(label), "%02d.  %-40s  %s", i + 1, e.display_name.c_str(), format_size(e.size_bytes));
                if (ImGui::Selectable(label, e.selected))
                {
                    if (!ImGui::GetIO().KeyCtrl)
                    {
                        for (auto &x : audio_tracks.entries)
                            x.selected = false;
                    }
                    e.selected = !e.selected;
                }
            }
            if (audio_tracks.entries.empty())
            {
                ImGui::TextDisabled("\n\n\n\n                  drop audio files here, or use the buttons below");
            }
        }
        ImGui::EndChild();
        float btn_w = (ImGui::GetContentRegionAvail().x - 16.0f) / 3.0f;
        if (ImGui::Button("Add track(s)", ImVec2(btn_w, 0)))
        {
            char filename[4096] = "";
            OPENFILENAMEA ofn;
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = NULL;
            ofn.lpstrFilter = "Audio files (FFmpeg supported)\0*.wav;*.mp3;*.flac;*.ogg;*.opus;*.m4a;*.aac;*.wma;*.ape;*.wv;*.ac3;*.aiff;*.aif;*.mp2\0All files\0*.*\0";
            ofn.lpstrFile = filename;
            ofn.nMaxFile = sizeof(filename);
            ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT;
            char initial_dir[MAX_PATH] = {0};
            if (!_config.last_browse_dir.empty())
            {
                strncpy(initial_dir, _config.last_browse_dir.c_str(), sizeof(initial_dir) - 1);
                ofn.lpstrInitialDir = initial_dir;
            }
            if (GetOpenFileNameA(&ofn))
            {
                if (filename[strlen(filename) + 1] == '\0')
                {
                    char dir_buf[MAX_PATH] = {0};
                    strncpy(dir_buf, filename, sizeof(dir_buf) - 1);
                    if (PathRemoveFileSpecA(dir_buf))
                        _config.last_browse_dir = dir_buf;
                    audio_tracks.add_path(filename);
                }
                else
                {
                    char dir_buf[MAX_PATH] = {0};
                    strncpy(dir_buf, filename, sizeof(dir_buf) - 1);
                    if (PathRemoveFileSpecA(dir_buf))
                        _config.last_browse_dir = dir_buf;
                    char *dir = filename;
                    char *file = filename + strlen(dir) + 1;
                    while (*file)
                    {
                        char full[MAX_PATH];
                        snprintf(full, MAX_PATH, "%s\\%s", dir, file);
                        audio_tracks.add_path(full);
                        file += strlen(file) + 1;
                    }
                }
            }
        }
        ImGui::SameLine();
        {
            std::lock_guard<std::mutex> lk(audio_tracks.list_mutex);
            bool has_selected = false;
            for (const auto &e : audio_tracks.entries)
                if (e.selected)
                {
                    has_selected = true;
                    break;
                }
            ImGui::BeginDisabled(!has_selected);
        }
        if (ImGui::Button("Remove selected", ImVec2(btn_w, 0)))
            audio_tracks.rm_selected();
        ImGui::EndDisabled();
        ImGui::SameLine();
        bool is_empty = false;
        {
            std::lock_guard<std::mutex> lk(audio_tracks.list_mutex);
            is_empty = audio_tracks.entries.empty();
        }
        ImGui::BeginDisabled(is_empty);
        if (ImGui::Button("Clear all", ImVec2(btn_w, 0)))
            audio_tracks.clear();
        ImGui::EndDisabled();
        uint64_t total = audio_tracks.total_size();
        uint32_t total_sec = (uint32_t)(total / (44100 * 2 * 2));
        {
            std::lock_guard<std::mutex> lk(audio_tracks.list_mutex);
            ImGui::TextDisabled("%d track(s) - %02d:%02d total playtime", (int)audio_tracks.entries.size(), total_sec / 60, total_sec % 60);
        }
        ImGui::BeginChild("settings", ImVec2(0, 90), true);
        ImGui::TextDisabled("Settings");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Write speed:");
        ImGui::SameLine();
        bool has_spd = drive_count > 0 && selected_drive_idx >= 0 && selected_drive_idx < drive_count && !drives[selected_drive_idx].supported_write_speeds.empty();
        ImGui::SetNextItemWidth(80);
        {
            char spd_label[16];
            if (session_write_speed == 0)
                snprintf(spd_label, sizeof(spd_label), "Max");
            else
                snprintf(spd_label, sizeof(spd_label), "%dx", session_write_speed);
            if (ImGui::BeginCombo("##audio_speed", spd_label))
            {
                if (ImGui::Selectable("Max", session_write_speed == 0))
                    session_write_speed = 0;
                if (has_spd)
                {
                    for (int s : drives[selected_drive_idx].supported_write_speeds)
                    {
                        char item[16];
                        snprintf(item, sizeof(item), "%dx", s);
                        if (ImGui::Selectable(item, session_write_speed == s))
                            session_write_speed = s;
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::Checkbox("Eject tray when done", &session_eject);
        ImGui::EndChild();
        ImGui::BeginChild("output", ImVec2(0, 85), true);
        ImGui::TextDisabled("Output");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Drive:");
        ImGui::SameLine();
        if (drive_count > 0 && selected_drive_idx < drive_count)
        {
            ImGui::SetNextItemWidth(-30);
            if (ImGui::BeginCombo("##drive_combo", combo_label(selected_drive_idx)))
            {
                for (int i = 0; i < drive_count; i++)
                {
                    bool is_selected = (selected_drive_idx == i);
                    if (ImGui::Selectable(combo_label(i), is_selected))
                        selected_drive_idx = i;
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::ImageButton("##erase_audio", (ImTextureID)tex_clean, ImVec2(16, 16)))
                on_erase_clicked();
            ImGui::PopStyleColor();
            _status_line(audio_ctx.is_running);
        }
        else
        {
            ImGui::TextDisabled("no reader / writer found");
        }
        ImGui::EndChild();

        ImGui::SetCursorPos(ImVec2(10.0f, ImGui::GetWindowHeight() - 110.0f));
        if (audio_ctx.is_running)
        {
            ImGui::EndDisabled();
            if (button_w_icon("##btn_abort_audio", tex_abort, "Abort", ImVec2(130, 45)))
                __cd_abort(true);
            ImGui::BeginDisabled(true);
        }
        else
        {
            bool any_converting = false;
            uint64_t total_sz = 0;
            {
                std::lock_guard<std::mutex> lk(audio_tracks.list_mutex);
                for (const auto &e : audio_tracks.entries)
                {
                    if (e.is_converting)
                        any_converting = true;
                    total_sz += e.size_bytes;
                }
            }
            ImGui::BeginDisabled(any_converting);
            if (button_w_icon("##btn_burn_audio", tex_burn, "Burn audio CD", ImVec2(180, 45)))
            {
                if (any_converting)
                    cd_error("ERROR", "Yo! Some tracks are still converting with FFmpeg.");
                else if (is_empty)
                    cd_error("ERROR", "No audio tracks were added.");
                else if (total_sz > 846720000ULL)
                    cd_error("ERROR", "Total playtime exceeds 80 minutes (disc capacity).");
                else if (drive_count <= 0 || selected_drive_idx >= drive_count)
                    cd_error("ERROR", "No reader / writer is selected.");
                else if (!drives[selected_drive_idx].disc_present)
                    cd_error("ERROR", "No disc appears to be present.");
                else
                {
                    drive_handle test_h = drives_open(drives[selected_drive_idx].path);
                    bool ready = test_h.valid && drives_test_ready(test_h);
                    disc_info d_info = {};
                    if (test_h.valid)
                    {
                        d_info = probe_disc(test_h);
                        drives_close(test_h);
                    }
                    if (!ready)
                        cd_error("ERROR", "Perhaps a disc is still loading? Come again!");
                    else if (d_info.profile_known && d_info.profile != 0x09 && d_info.profile != 0x0A && d_info.profile != 0x08)
                        cd_error("ERROR", "Inserted disc is NOT a CD-R / -RW.");
                    else
                    {
                        if (audio_drive.valid)
                            drives_close(audio_drive);
                        audio_drive = drives_open(drives[selected_drive_idx].path);
                        if (!audio_drive.valid)
                            LOG_ERR("Failed to open drive");
                        else
                        {
                            audio_init(audio_ctx, &audio_drive, &audio_tracks, session_write_speed, session_eject);
                            audio_start(audio_ctx);
                        }
                    }
                }
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                request_mode_change(app_mode::build_mode);
            }
            ImGui::EndDisabled();
        }
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
    }
    else if (_app_mode == app_mode::settings) // c; settings tab
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 6));
        const int cd_speeds[] = {1, 2, 4, 8, 16, 24, 32, 48, 52};
        const int dvd_speeds[] = {1, 2, 4, 6, 8, 12, 16, 20, 24};
        const int n_cd = 9;
        const int n_dvd = 9;
        auto speed_combo = [](const char *id, int *val, const int *speeds, int n) -> bool
        {
            bool changed = false;
            char lbl[16];
            if (*val == 0)
                snprintf(lbl, sizeof(lbl), "Max");
            else
                snprintf(lbl, sizeof(lbl), "%dx", *val);
            ImGui::SetNextItemWidth(80);
            if (ImGui::BeginCombo(id, lbl))
            {
                if (ImGui::Selectable("Max", *val == 0))
                {
                    *val = 0;
                    changed = true;
                }
                for (int i = 0; i < n; ++i)
                {
                    char item[16];
                    snprintf(item, sizeof(item), "%dx", speeds[i]);
                    if (ImGui::Selectable(item, *val == speeds[i]))
                    {
                        *val = speeds[i];
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            return changed;
        };
        ImGui::BeginChild("settings_scroll", ImVec2(0, ImGui::GetWindowHeight() - 130.0f));
        ImGui::TextDisabled("Write speeds");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("CD write speed:");
        ImGui::SameLine();
        settings_dirty |= speed_combo("##cd_ws", &temp_config.cd_write_speed, cd_speeds, n_cd);
        _tt_icon("cd_ws", "tt_cd_ws");
        ImGui::AlignTextToFramePadding();
        ImGui::Text("DVD write speed:");
        ImGui::SameLine();
        settings_dirty |= speed_combo("##dvd_ws", &temp_config.dvd_write_speed, dvd_speeds, n_dvd);
        _tt_icon("dvd_ws", "tt_dvd_ws");
        ImGui::Spacing();
        ImGui::TextDisabled("Read speeds");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("CD read speed:");
        ImGui::SameLine();
        settings_dirty |= speed_combo("##cd_rs", &temp_config.cd_read_speed, cd_speeds, n_cd);
        _tt_icon("cd_rs", "tt_cd_rs");
        ImGui::AlignTextToFramePadding();
        ImGui::Text("DVD read speed:");
        ImGui::SameLine();
        settings_dirty |= speed_combo("##dvd_rs", &temp_config.dvd_read_speed, dvd_speeds, n_dvd);
        _tt_icon("dvd_rs", "tt_dvd_rs");
        ImGui::Spacing(); // c; we won't support this bullshi for BDs since they are encrypted and MakeMKV exists
        ImGui::TextDisabled("Device");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Default drive:");
        ImGui::SameLine();
        char def_drv_label[128];
        if (temp_config.default_drive_path.empty())
            snprintf(def_drv_label, sizeof(def_drv_label), "Whatever I seek (first available)");
        else
            snprintf(def_drv_label, sizeof(def_drv_label), "%s", temp_config.default_drive_path.c_str());
        ImGui::SetNextItemWidth(-1);
        ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - 24);
        if (ImGui::BeginCombo("##def_drv", def_drv_label))
        {
            if (ImGui::Selectable("Whatever I seek (first available)", temp_config.default_drive_path.empty()))
            {
                temp_config.default_drive_path = "";
                settings_dirty = true;
            }
            for (int i = 0; i < drive_count; ++i)
            {
                if (ImGui::Selectable(combo_label(i), temp_config.default_drive_path == drives[i].path))
                {
                    temp_config.default_drive_path = drives[i].path;
                    settings_dirty = true;
                }
            }
            ImGui::EndCombo();
        }
        _tt_icon("def_drv", "tt_def_drv");
        ImGui::Spacing();
        ImGui::TextDisabled("Burn behaviour"); // m; Oh so you DO use British-English? Lovely.
        ImGui::Separator();                    // c; or i could of put "postavke prženja", y'know?
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Write mode:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        if (ImGui::BeginCombo("##wm", temp_config.write_mode == 0 ? "DAO" : "TAO"))
        {
            if (ImGui::Selectable("DAO", temp_config.write_mode == 0))
            {
                temp_config.write_mode = 0;
                settings_dirty = true;
            }
            if (ImGui::Selectable("TAO", temp_config.write_mode == 1))
            {
                temp_config.write_mode = 1;
                settings_dirty = true;
            }
            ImGui::EndCombo();
        }
        _tt_icon("write_mode", "tt_write_mode");
        settings_dirty |= ImGui::Checkbox("Verify after burn", &temp_config.verify_after_burn);
        _tt_icon("verify", "tt_verify");
        settings_dirty |= ImGui::Checkbox("Eject tray when done", &temp_config.eject_when_done);
        _tt_icon("eject", "tt_eject");
        settings_dirty |= ImGui::Checkbox("Use MODE SELECT", &temp_config.use_mode_select);
        _tt_icon("mode_select", "tt_mode_select");
        ImGui::AlignTextToFramePadding();
        ImGui::Text("ISO mode:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        if (ImGui::BeginCombo("##settings_iso_mode", iso_mode_label(temp_config.iso_mode)))
        {
            if (ImGui::Selectable("ISO9660", temp_config.iso_mode == 0))
            {
                temp_config.iso_mode = 0;
                settings_dirty = true;
            }
            if (ImGui::Selectable("Joliet (9660)", temp_config.iso_mode == 1))
            {
                temp_config.iso_mode = 1;
                settings_dirty = true;
            }
            if (ImGui::Selectable("UDF", temp_config.iso_mode == 2))
            {
                temp_config.iso_mode = 2;
                settings_dirty = true;
            }
            if (ImGui::Selectable("Hybrid", temp_config.iso_mode == 3))
            {
                temp_config.iso_mode = 3;
                settings_dirty = true;
            }
            ImGui::EndCombo();
        }
        _tt_icon("iso_mode", "tt_iso_mode");
        ImGui::Spacing();
        ImGui::TextDisabled("Audio CD (FFmpeg)");
        ImGui::Separator();
        settings_dirty |= ImGui::Checkbox("Write FFmpeg decode output to cache folder (.wav)", &temp_config.where_audio_go);
        _tt_icon("ffmpeg_wav", "tt_ffmpeg_wav");
        ImGui::AlignTextToFramePadding();
        ImGui::Text("FFmpeg path (exe):");
        ImGui::SetNextItemWidth(-1);
        ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - 24); // Make room for icon
        if (ImGui::InputText("##ff_ov", ffmpeg_ov_ui, sizeof(ffmpeg_ov_ui)))
            settings_dirty = true;
        _tt_icon("ffmpeg_path", "tt_ffmpeg_path");
        if (ImGui::Button("Browse ffmpeg.exe##ffxb", ImVec2(140, 0)))
            show_ffmpeg_exe_browse();
        ImGui::TextDisabled("Leave empty to auto detect (PATH, next to PABS).\nSaved list below uses current saved config.");
        ImGui::TextDisabled("Detected candidates:");
        {
            std::vector<std::string> ff_c;
            ff_candidates(ff_c);
            if (ff_c.empty())
                ImGui::BulletText("none - no FFmpeg was located in your systems PATH");
            else
                for (const auto &s : ff_c)
                    ImGui::BulletText("%s", s.c_str());
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Sound effects");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("SFX volume:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        if (ImGui::SliderInt("##sfx_vol", &temp_config.sfx_volume, 0, 100))
        {
            settings_dirty = true;
            _config.sfx_volume = temp_config.sfx_volume;
            set_sfx_vol();
        }
        settings_dirty |= ImGui::Checkbox("Mute success sound", &temp_config.mute_success_sfx);
        settings_dirty |= ImGui::Checkbox("Mute error sound", &temp_config.mute_error_sfx);
        settings_dirty |= ImGui::Checkbox("Mute warning sound", &temp_config.mute_warning_sfx);
        ImGui::Spacing();
        ImGui::TextDisabled("Defaults");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Volume label:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputText("##def_vol", temp_label, sizeof(temp_label)))
            settings_dirty = true;
        _tt_icon("vol_label", "tt_vol_label");
        ImGui::Spacing();
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 8.0f);
        if (button_w_icon("##btn_save_settings", tex_save, "Save settings", ImVec2(160, 45)))
        {
            temp_config.volume_label = temp_label;
            temp_config.ffmpeg_override.assign(ffmpeg_ov_ui);
            trim_str(temp_config.ffmpeg_override);
            _config = temp_config;
            config_save(_config);
            set_sfx_vol();
            settings_dirty = false;
            LOG_OK("Settings saved to pabs.json");
            cd_ok("Settings saved", "Your settings have been saved successfully.");
        }
        ImGui::PopStyleVar();
    }
    else if (_app_mode == app_mode::verify_mode) // c; verify disc
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
        ImGui::BeginDisabled(any_engine_running);
        ImGui::TextDisabled("Source");
        ImGui::BeginChild("source_panel", ImVec2(0, 75), true);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Drive:");
        ImGui::SameLine();
        if (ImGui::BeginCombo("##verify_drive", combo_label(selected_drive_idx)))
        {
            for (int i = 0; i < drive_count; ++i)
            {
                bool is_selected = (selected_drive_idx == i);
                if (ImGui::Selectable(combo_label(i), is_selected))
                {
                    selected_drive_idx = i;
                }
                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        _status_line(verify_ctx.is_running);
        static char iso_path[4096] = "";
        if (verify_compare_path[0] != '\0')
        {
            memcpy(iso_path, verify_compare_path, sizeof(iso_path));
            iso_path[sizeof(iso_path) - 1] = '\0';
            verify_compare_path[0] = '\0';
        }
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Image:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 40.0f);
        ImGui::InputText("##verify_img", iso_path, sizeof(iso_path));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Leave blank to just test physical disc readability (no comparison)."); // c; the tooltip could be dti'd[] but that's not MY interest
        ImGui::SameLine();
        if (ImGui::Button("...##verify_br", ImVec2(30, 0)))
        {
            OPENFILENAMEA ofn;
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = NULL;
            ofn.lpstrFilter = "ISO files\0*.iso\0All files\0*.*\0";
            ofn.lpstrFile = iso_path;
            ofn.nMaxFile = sizeof(iso_path);
            ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            char initial_dir[MAX_PATH] = {0};
            if (!_config.last_browse_dir.empty())
            {
                strncpy(initial_dir, _config.last_browse_dir.c_str(), sizeof(initial_dir) - 1);
                ofn.lpstrInitialDir = initial_dir;
            }
            if (GetOpenFileNameA(&ofn))
            {
                char dir_buf[MAX_PATH] = {0};
                strncpy(dir_buf, iso_path, sizeof(dir_buf) - 1);
                if (PathRemoveFileSpecA(dir_buf))
                    _config.last_browse_dir = dir_buf;
            }
        }
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::TextDisabled("Settings");
        ImGui::BeginChild("verify_settings", ImVec2(0, 45), true);
        ImGui::AlignTextToFramePadding();
        ImGui::Checkbox("Eject tray when done", &session_eject);
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::TextDisabled("Sector map (50x20)");          // m; Yo I was gone for TWO hours and you made this shit?!
        ImGui::BeginChild("sec_map", ImVec2(0, 160), true); // >> Like how do you even make this?!
        ImDrawList *draw_list = ImGui::GetWindowDrawList(); // c; raylib
        ImVec2 p = ImGui::GetCursorScreenPos();
        float box_size = 6.0f;
        float spacing = 1.0f;
        int cols = 50;
        int rows = 20;
        for (int i = 0; i < VERIFY_GRID_S; ++i)
        {
            int r = i / cols;
            int c = i % cols;
            ImVec2 p_min = ImVec2(p.x + c * (box_size + spacing), p.y + r * (box_size + spacing));
            ImVec2 p_max = ImVec2(p_min.x + box_size, p_min.y + box_size);
            ImU32 color = IM_COL32(100, 100, 100, 255);
            verify_block_state st = verify_ctx.sector_map[i].load();
            if (st == verify_block_state::good)
                color = IM_COL32(50, 205, 50, 255);
            else if (st == verify_block_state::bad)
                color = IM_COL32(255, 69, 0, 255);
            else if (st == verify_block_state::unrecorded)
                color = IM_COL32(30, 30, 30, 255);
            draw_list->AddRectFilled(p_min, p_max, color);
            if (ImGui::IsMouseHoveringRect(p_min, p_max))
            {
                ImGui::BeginTooltip();
                uint64_t v_bytes = verify_ctx.verify_bytes.load();
                uint64_t start_byte = v_bytes ? (i * v_bytes) / VERIFY_GRID_S : 0;
                uint64_t end_byte = v_bytes ? ((i + 1) * v_bytes) / VERIFY_GRID_S : 0;
                ImGui::Text("Grid index: %d", i);
                ImGui::Text("Range: %llu <-> %llu bytes", (unsigned long long)start_byte, (unsigned long long)end_byte);
                if (st == verify_block_state::bad)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Status: mismatch / read error");
                    ImGui::Text("Error at offset: %llu", (unsigned long long)verify_ctx.sector_mismatch[i].load());
                }
                else if (st == verify_block_state::unrecorded)
                {
                    ImGui::TextDisabled("Status: not part of recorded data");
                }
                else if (st == verify_block_state::good)
                {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Status: OK");
                }
                else
                {
                    ImGui::TextDisabled("Status: unscanned");
                }
                ImGui::EndTooltip();
            }
        }
        ImGui::Dummy(ImVec2(cols * (box_size + spacing), rows * (box_size + spacing)));
        ImGui::EndChild();
        ImGui::Spacing();
        if (verify_ctx.is_running)
        {
            ImGui::EndDisabled();
            if (button_w_icon("##btn_abort_verify", tex_abort, "Abort", ImVec2(120, 45)))
                __cd_abort(false);
            ImGui::BeginDisabled(true);
        }
        else
        {
            if (button_w_icon("##btn_verify", tex_burn, "Verify", ImVec2(120, 45)))
            {
                if (drive_count <= 0 || selected_drive_idx < 0 || selected_drive_idx >= drive_count)
                {
                    cd_error("ERROR", "No reader / writer selected.");
                }
                else if (!drives[selected_drive_idx].disc_present)
                {
                    cd_error("ERROR", "No disc is present in the drive in the first place.");
                }
                else
                {
                    drive_handle test_h = drives_open(drives[selected_drive_idx].path);
                    bool ready = test_h.valid && drives_test_ready(test_h);
                    if (test_h.valid)
                        drives_close(test_h);
                    if (!ready)
                    {
                        cd_error("ERROR", "Perhaps a disc is still loading? Come again!");
                    }
                    else
                    {
                        drive_handle probe_h = drives_open(drives[selected_drive_idx].path);
                        disc_info probe = probe_disc(probe_h);
                        if (probe_h.valid)
                            drives_close(probe_h);
                        if (probe.media_present && probe.disc_info_we_kinda_know && (probe.disc_status & 0x03) == 0)
                        {
                            cd_error("ERROR", "Disc doesn't have any data in the first place;\nwhat are you trying to verify?");
                        }
                        else
                        {
                            verify_drive = drives_open(drives[selected_drive_idx].path);
                            if (!verify_drive.valid)
                            {
                                LOG_ERR("Failed to open drive for verify");
                            }
                            else
                            {
                                verify_options opts;
                                opts.compare_image_path = iso_path;
                                opts.eject_when_done = session_eject;
                                verify_init(verify_ctx, &verify_drive, opts);
                                verify_start(verify_ctx);
                            }
                        }
                    }
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
    }
    else if (_app_mode == app_mode::are_you)
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
        ImGui::BeginDisabled(any_engine_running);
        ImGui::BeginChild("are_u_source", ImVec2(0, 85), true);
        ImGui::TextDisabled("Source");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Drive:");
        ImGui::SameLine();
        if (drive_count > 0 && selected_drive_idx < drive_count)
        {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##are_you_drive", combo_label(selected_drive_idx)))
            {
                for (int i = 0; i < drive_count; i++)
                {
                    bool is_selected = (selected_drive_idx == i);
                    if (ImGui::Selectable(combo_label(i), is_selected))
                        selected_drive_idx = i;
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            _status_line(false);
        }
        else
        {
            ImGui::TextDisabled("no reader / writer selected");
        }
        ImGui::EndChild();
        static char report_buf[16384] = {0};
        static bool prev_present = false;
        static char prev_drive[8] = {0};
        static bool show_unreadable_once = false;
        enum class probe_state
        {
            idle,
            running,
            done
        };
        static std::atomic<probe_state> probe_state = {probe_state::idle};
        static disc_info probe_result = {};
        static std::mutex probe_mutex;
        static char probe_drive_path[8] = {0};
        static char probe_letter_root[8] = {0};
        bool present = (drive_count > 0 && selected_drive_idx >= 0 && selected_drive_idx < drive_count) ? drives[selected_drive_idx].disc_present : false;
        bool drive_changed = false;
        if (drive_count > 0 && selected_drive_idx >= 0 && selected_drive_idx < drive_count)
        {
            drive_changed = (strncmp(prev_drive, drives[selected_drive_idx].path, sizeof(prev_drive) - 1) != 0);
        }
        if ((present != prev_present || drive_changed) && probe_state.load() != probe_state::running)
        {
            prev_present = present;
            memset(report_buf, 0, sizeof(report_buf));
            if (drive_count > 0 && selected_drive_idx >= 0 && selected_drive_idx < drive_count)
            {
                snprintf(prev_drive, sizeof(prev_drive), "%s", drives[selected_drive_idx].path);
                snprintf(probe_drive_path, sizeof(probe_drive_path), "%s", drives[selected_drive_idx].path);
                snprintf(probe_letter_root, sizeof(probe_letter_root), "%c:\\", drives[selected_drive_idx].path[0]);
                if (present)
                {
                    snprintf(report_buf, sizeof(report_buf), "probing disc...");
                    probe_state.store(probe_state::running);
                    drive_info snap = drives[selected_drive_idx];
                    std::thread([snap]()
                                {
                        drive_handle h = drives_open(snap.path);
                        disc_info result = {};
                        if (h.valid)
                        {
                            result = probe_disc(h);
                            drives_close(h);
                        }
                        char vol_name[MAX_PATH] = {0};
                        char fs_name[MAX_PATH] = {0};
                        DWORD serial = 0, max_comp = 0, flags = 0;
                        char letter_root[8];
                        snprintf(letter_root, sizeof(letter_root), "%c:\\", snap.path[0]);
                        bool vol_ok = GetVolumeInformationA(letter_root, vol_name, (DWORD)sizeof(vol_name), &serial, &max_comp, &flags, fs_name, (DWORD)sizeof(fs_name)) == TRUE;
                        char buf[16384] = {0};
                        auto append = [&](const char *fmt, ...)
                        {
                            va_list ap;
                            va_start(ap, fmt);
                            size_t cur = strlen(buf);
                            vsnprintf(buf + cur, sizeof(buf) - cur - 1, fmt, ap);
                            va_end(ap);
                        };
                        auto append_field = [&](const char *label, const char *value)
                        {
                            append("%-21s %s\r\n", label, value);
                        };
                        auto append_field_u32 = [&](const char *label, uint32_t value)
                        {
                            append("%-21s %u\r\n", label, value);
                        };
                        append("prob.\n");
                        append("----------------------------------------\r\n");
                        append_field("Drive:", snap.path);
                        append_field("Vendor:", snap.vendor);
                        append_field("Product:", snap.product);
                        append_field("Revision:", snap.revision);
                        append_field("Drive type:", drives_type_str(snap.type));
                        append_field("Media present:", "yeah (maybe)");
                        if (result.profile_known)
                            append("%-21s %s (0x%04X)\r\n", "Disc type:", disc_profile_str(result.profile), result.profile);
                        else
                            append_field("Disc type:", "unavailable");
                        if (result.disc_info_we_kinda_know)
                        {
                            append_field("Disc status:", disc_status_str(result.disc_status));
                            append_field("Last session state:", last_disc_sess_str(result._state_session));
                            append_field("Burned data:", ((result.disc_status & 0x03) == 0) ? "nah (blank)" : "yeah");
                        }
                        else
                        {
                            append_field("Disc status:", "unavailable");
                            append_field("Last session state:", "unavailable");
                            append_field("Burned data:", "unknown");
                        }
                        if (result.session_info)
                        {
                            append_field_u32("Sessions:", result.sessions);
                            append_field_u32("First track on disc:", result._first_track);
                            append_field_u32("First track in last session:", result.first_track_l_session);
                            append_field_u32("Last track in last session:", result.last_track_l_session);
                        }
                        else
                        {
                            append_field("Sessions:", "unavailable");
                            append_field("First track on disc:", "unavailable");
                            append_field("First track in last session:", "unavailable");
                            append_field("Last track in last session:", "unavailable");
                        }
                        bool rewritable = result.is_eraseable ? result.erasable : burn_is_rw(snap.path);
                        append_field("Rewritable:", rewritable ? "yeah" : "nah");
                        uint32_t free_sectors = 0;
                        bool free_sectors_known = false;
                        if (result.capacity_known)
                        {
                            append_field("Capacity:", format_size(result.capacity_bytes));
                            append_field_u32("Sectors:", result.total_sectors);
                            append("%-21s %u B\r\n", "Sector size:", result.sector_size);
                            free_sectors = result.total_sectors;
                            free_sectors_known = true;
                        }
                        else
                        {
                            append_field("Capacity:", "unavailable - normal on blank discs");
                            append_field("Sectors:", "unavailable - normal on blank discs");
                            append_field("Sector size:", "unavailable - normal on blank discs");
                        }
                        if (result.disc_info_we_kinda_know && (result.disc_status & 0x03) == 0 && result.atip_known)
                        {
                            uint32_t atip_frames = (uint32_t)result.atip_leadout_m * 60U * 75U + (uint32_t)result.atip_leadout_s * 75U + (uint32_t)result.atip_leadout_f;
                            if (atip_frames > 150U)
                            {
                                free_sectors = atip_frames - 150U;
                                free_sectors_known = true;
                            }
                        }
                        if (free_sectors_known)
                        {
                            char sec_buf[32] = {};
                            format_u32(free_sectors, sec_buf, sizeof(sec_buf));
                            uint64_t free_bytes = (uint64_t)free_sectors * 2048ULL;
                            append_field("Free sectors:", sec_buf);
                            append_field("Free space:", format_size(free_bytes));
                            uint32_t frames = free_sectors + 150U;
                            uint32_t mm = frames / (60U * 75U);
                            uint32_t rem = frames % (60U * 75U);
                            uint32_t ss2 = rem / 75U;
                            uint32_t ff = rem % 75U;
                            append("%-21s %02u:%02u:%02u (MM:SS:FF)\r\n", "Free time:", mm, ss2, ff);
                        }
                        else
                        {
                            append_field("Free sectors:", "unavailable");
                            append_field("Free space:", "unavailable");
                            append_field("Free time:", "unavailable");
                        }
                        if (result.nwa_known)
                            append_field_u32("Next writable address:", result.next_writable_lba);
                        else if (result.disc_info_we_kinda_know && (result.disc_status & 0x03) == 0)
                            append_field("Next writable address:", "0");
                        else
                            append_field("Next writable address:", "unavailable");
                        if (result.atip_known)
                        {
                            append("%-21s %02um%02us%02uf\r\n", "ATIP lead in:", result.atip_leadin_m, result.atip_leadin_s, result.atip_leadin_f);
                            append("%-21s %02um%02us%02uf\r\n", "ATIP lead out:", result.atip_leadout_m, result.atip_leadout_s, result.atip_leadout_f);
                        }
                        else
                        {
                            append_field("ATIP lead in:", "unavailable");
                            append_field("ATIP lead out:", "unavailable");
                        }
                        if (!snap.supported_write_speeds.empty())
                        {
                            append("%-21s ", "Supported write speeds:");
                            for (size_t i2 = 0; i2 < snap.supported_write_speeds.size(); ++i2)
                            {
                                append("%dx", snap.supported_write_speeds[i2]);
                                if (i2 + 1 < snap.supported_write_speeds.size())
                                    append(", ");
                            }
                            append("\r\n");
                        }
                        else
                        {
                            append_field("Supported write speeds:", "unavailable");
                        }
                        if (vol_ok)
                        {
                            append_field("Volume label:", vol_name[0] ? vol_name : "none");
                            append_field("Filesystem:", fs_name[0] ? fs_name : "unknown");
                        }
                        else
                        {
                            append_field("Volume label:", "unavailable");
                            append_field("Filesystem:", "unavailable");
                        }
                        bool blank_disc = (result.disc_info_we_kinda_know && (result.disc_status & 0x03) == 0);
                        bool partial_read = !result.first_read_ok;
                        if (blank_disc)
                            append_field("Readable probe:", "not applicable (blank media)");
                        else
                            append_field("Readable probe:", partial_read ? "failed" : "OK");
                        std::lock_guard<std::mutex> lk(probe_mutex);
                        probe_result = result;
                        snprintf(report_buf, sizeof(report_buf), "%s", buf);
                        bool blank = (result.disc_info_we_kinda_know && (result.disc_status & 0x03) == 0);
                        show_unreadable_once = (!blank && partial_read);
                        probe_state.store(probe_state::done); })
                        .detach();
                }
                else
                {
                    snprintf(report_buf, sizeof(report_buf), "\r\ninsert a disc to probe it\r\n");
                    probe_state.store(probe_state::idle);
                    show_unreadable_once = false;
                }
            }
        }
        if (probe_state.load() == probe_state::done)
            probe_state.store(probe_state::idle);
        ImGui::Spacing();
        ImGui::BeginChild("disc_report", ImVec2(0, ImGui::GetWindowHeight() - 210.0f), true);
        ImGui::TextDisabled("Report");
        ImGui::Separator();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextMultiline("##are_you_report", report_buf, sizeof(report_buf), ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_AllowTabInput);
        ImGui::EndChild();
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        if (show_unreadable_once)
        {
            show_unreadable_once = false;
            cd_error("ERROR", "The disc could not be fully read;\nperhaps it contains raw PCM audio data, or is just damaged?\nCheck the sectors via verify disc mode.");
        }
    }
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 60);
    bool is_running = false;
    float raw_progress = 0.0f;
    const char *status = "Idle";
    float speed_mbps = 0.0f;
    uint32_t elapsed = 0;
    char status_buf[64] = {0};
    if (_app_mode == app_mode::write_mode)
    {
        is_running = burn_ctx.is_running;
        raw_progress = burn_ctx.progress_percent;
        {
            std::lock_guard<std::mutex> lk(burn_ctx.status_mutex);
            snprintf(status_buf, sizeof(status_buf), "%s", burn_ctx.status_text);
        }
        status = status_buf;
        speed_mbps = burn_ctx.write_speed_mbps.load();
        elapsed = burn_ctx.elapsed_seconds.load();
    }
    else if (_app_mode == app_mode::read_mode)
    {
        is_running = read_ctx.is_running;
        raw_progress = read_ctx.progress_percent;
        {
            std::lock_guard<std::mutex> lk(read_ctx.status_mutex);
            snprintf(status_buf, sizeof(status_buf), "%s", read_ctx.status_text);
        }
        status = status_buf;
        speed_mbps = read_ctx.read_speed_mbps.load();
        elapsed = read_ctx.elapsed_seconds.load();
    }
    else if (_app_mode == app_mode::build_mode || _app_mode == app_mode::build_iso)
    {
        is_running = build_ctx.is_running;
        raw_progress = build_ctx.progress_percent;
        {
            std::lock_guard<std::mutex> lk(build_ctx.status_mutex);
            snprintf(status_buf, sizeof(status_buf), "%s", build_ctx.status_text);
        }
        status = status_buf;
        speed_mbps = build_ctx.write_speed_mbps.load();
        elapsed = build_ctx.elapsed_seconds.load();
    }
    else if (_app_mode == app_mode::audio_cd)
    {
        is_running = audio_ctx.is_running;
        raw_progress = audio_ctx.progress_percent;
        {
            std::lock_guard<std::mutex> lk(audio_ctx.status_mutex);
            snprintf(status_buf, sizeof(status_buf), "%s", audio_ctx.status_text);
        }
        status = status_buf;
        speed_mbps = audio_ctx.write_speed_mbps.load();
        elapsed = audio_ctx.elapsed_seconds.load();
    }
    else if (_app_mode == app_mode::verify_mode)
    {
        is_running = verify_ctx.is_running;
        uint32_t bad_sectors = 0;
        verify_progress(verify_ctx, raw_progress, speed_mbps, elapsed, bad_sectors);
        {
            std::lock_guard<std::mutex> lk(verify_ctx.status_mutex);
            snprintf(status_buf, sizeof(status_buf), "%s", verify_ctx.status_text);
        }
        status = status_buf;
    }
    if (erase_ctx.is_running)
    {
        is_running = true;
        uint32_t erase_elapsed = 0, erase_eta = 0;
        erase_progress(erase_ctx, erase_elapsed, erase_eta);
        elapsed = erase_elapsed;
        if (erase_eta > 0)
        {
            raw_progress = (float)erase_elapsed / (float)erase_eta * 100.0f;
            if (raw_progress > 100.0f)
                raw_progress = 100.0f;
        }
        else
        {
            raw_progress = 0.0f;
        }
        {
            std::lock_guard<std::mutex> lk(erase_ctx.status_mutex);
            snprintf(status_buf, sizeof(status_buf), "%s", erase_ctx.status_text);
        }
        status = status_buf;
        speed_mbps = 0.0f;
    }
    float progress = raw_progress / 100.0f;
    char prog_text[128];
    if (is_running)
    {
        snprintf(prog_text, sizeof(prog_text), "%s - %.1f%%", status, raw_progress);
    }
    else if (raw_progress >= 100.0f)
    {
        snprintf(prog_text, sizeof(prog_text), "%s", status);
    }
    else
    {
        snprintf(prog_text, sizeof(prog_text), "%s", status);
    }
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.4f, 0.6f, 0.5f, 1.0f));
    ImGui::ProgressBar(progress, ImVec2(-1, 0), prog_text);
    ImGui::PopStyleColor();
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 35);
    ImGui::Separator();
    if (is_running)
    {
        ImGui::TextDisabled("%.1f MB/s", speed_mbps);
    }
    else
    {
        ImGui::TextDisabled("0.0 MB/s");
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 100);
    static double action_start_time = 0;
    static bool was_running = false;
    static uint32_t final_secs = 0;
    static bool prev_read_running = false;
    static bool read_err_shown = false;
    if (!prev_read_running && read_ctx.is_running)
    {
        read_err_shown = false;
    }
    static bool prev_build_running = false;
    static bool build_err_shown = false;
    if (!prev_build_running && build_ctx.is_running)
    {
        build_err_shown = false;
    }
    static bool prev_burn_running = false;
    static bool burn_err_shown = false;
    if (!prev_burn_running && burn_ctx.is_running)
    {
        burn_err_shown = false;
    }
    static bool prev_audio_running = false;
    static bool audio_err_shown = false;
    if (!prev_audio_running && audio_ctx.is_running)
    {
        audio_err_shown = false;
    }
    if (any_engine_running && !was_running)
    {
        action_start_time = ImGui::GetTime();
        was_running = true;
    }
    else if (!any_engine_running && was_running)
    {
        was_running = false;
        final_secs = (uint32_t)(ImGui::GetTime() - action_start_time);
        if (read_ctx.disc_removed.exchange(false))
        {
            cd_error("ERROR", "The disc was removed during the read operation.");
        }
        else if (verify_ctx.disc_removed.exchange(false))
        {
            cd_error("ERROR", "The disc was removed during the verify operation.");
        }
    }
    if (prev_read_running && !read_ctx.is_running && !read_err_shown)
    {
        char st[64] = {0};
        {
            std::lock_guard<std::mutex> lk(read_ctx.status_mutex);
            snprintf(st, sizeof(st), "%s", read_ctx.status_text);
        }
        if (strncmp(st, "[ ERROR ]", 9) == 0)
        {
            cd_error("ERROR", st);
            read_err_shown = true;
        }
    }
    prev_read_running = read_ctx.is_running;
    if (prev_build_running && !build_ctx.is_running && !build_err_shown)
    {
        char st[64] = {0};
        {
            std::lock_guard<std::mutex> lk(build_ctx.status_mutex);
            snprintf(st, sizeof(st), "%s", build_ctx.status_text);
        }
        if (strncmp(st, "[ ERROR ]", 9) == 0)
        {
            const char *title = (_app_mode == app_mode::build_iso) ? "Files to ISO" : "Files to disc";
            show_friendly_build_error_dialog(title, st);
            build_err_shown = true;
        }
        else if (strncmp(st, "Aborted", 7) != 0 && build_ctx.verify_after_burn && !build_ctx.build_to_iso)
        {
            _app_mode = app_mode::verify_mode;
            verify_options opts;
            opts.eject_when_done = build_ctx.eject_when_done;
            if (build_drive.valid)
                drives_close(build_drive);
            verify_drive = drives_open(drives[selected_drive_idx].path);
            verify_init(verify_ctx, &verify_drive, opts);
            verify_start(verify_ctx);
        }
    }
    prev_build_running = build_ctx.is_running;
    if (prev_burn_running && !burn_ctx.is_running && !burn_err_shown)
    {
        char st[64] = {0};
        {
            std::lock_guard<std::mutex> lk(burn_ctx.status_mutex);
            snprintf(st, sizeof(st), "%s", burn_ctx.status_text);
        }
        if (strncmp(st, "[ ERROR ]", 9) == 0)
        {
            if (is_status(st, "[ ERROR ] Disc not blank"))
                cd_error("ERROR", "The inserted disc is not blank, try a different one."); // c; x2
            else
                cd_error("ERROR", "The operation failed; check the logs for details.");
            burn_err_shown = true;
        }
        else if (strncmp(st, "Aborted", 7) != 0 && burn_ctx.options.verify_after_burn)
        {
            _app_mode = app_mode::verify_mode;
            verify_options opts;
            opts.compare_image_path = burn_ctx.image->path;
            opts.eject_when_done = burn_ctx.options.eject_when_done;
            if (burn_drive.valid)
                drives_close(burn_drive);
            verify_drive = drives_open(drives[selected_drive_idx].path);
            verify_init(verify_ctx, &verify_drive, opts);
            verify_start(verify_ctx);
        }
    }
    prev_burn_running = burn_ctx.is_running;
    if (prev_audio_running && !audio_ctx.is_running && !audio_err_shown)
    {
        char st[64] = {0};
        {
            std::lock_guard<std::mutex> lk(audio_ctx.status_mutex);
            snprintf(st, sizeof(st), "%s", audio_ctx.status_text);
        }
        if (strncmp(st, "[ ERROR ]", 9) == 0)
        {
            if (is_status(st, "[ ERROR ] Disc not blank"))
                cd_error("ERROR", "The inserted disc is not blank, try a different one.");
            else
                cd_error("ERROR", "The operation failed; check the logs for details.");
            audio_err_shown = true;
        }
    }
    prev_audio_running = audio_ctx.is_running;
    uint32_t secs = elapsed;
    if (any_engine_running)
    {
        secs = (uint32_t)(ImGui::GetTime() - action_start_time);
    }
    else if (raw_progress >= 100.0f || final_secs > 0)
    {
        secs = final_secs;
    }
    ImGui::TextDisabled("%u:%02u elapsed", secs / 60, secs % 60);
    cd_about();
    cd_keybinds();
    cd_quick_features();
    cd_creators();
    clean_action action = render_dialogs();
    if (action == clean_action::Quick || action == clean_action::Full)
    {
        if (drive_count > 0 && !erase_ctx.is_running)
        {
            erase_drive = drives_open(drives[selected_drive_idx].path);
            if (!erase_drive.valid)
            {
                LOG_ERR("Failed to open drive for erasing");
            }
            else
            {
                erase_options opts;
                opts.full_erase = (action == clean_action::Full);
                opts.eject_when_done = _config.eject_when_done;
                erase_init(erase_ctx, &erase_drive, opts);
                erase_start(erase_ctx);
            }
        }
    }
    abort_action abort_action = cd_abort();
    if (abort_action == abort_action::Confirmed)
    {
        if (burn_ctx.is_running)
            burn_abort(burn_ctx);
        else if (read_ctx.is_running)
            read_abort(read_ctx);
        else if (build_ctx.is_running)
            build_abort(build_ctx);
        else if (verify_ctx.is_running)
            verify_abort(verify_ctx);
        else if (erase_ctx.is_running)
            erase_abort(erase_ctx);
        else if (audio_ctx.is_running)
            audio_abort(audio_ctx);
    }
    ImGui::End();
}
static int count_files_recursive(const std::string &path)
{
    int count = 0;
    DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return 0;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
    {
        std::string pattern = path;
        if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/')
            pattern += "\\";
        pattern += "*";
        WIN32_FIND_DATAA ffd;
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &ffd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (strcmp(ffd.cFileName, ".") == 0 || strcmp(ffd.cFileName, "..") == 0)
                    continue;
                if (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    continue;
                std::string child = path;
                if (!child.empty() && child.back() != '\\' && child.back() != '/')
                    child += "\\";
                child += ffd.cFileName;
                if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    count += count_files_recursive(child);
                else
                    count += 1;
            } while (FindNextFileA(hFind, &ffd));
            FindClose(hFind);
        }
    }
    else
    {
        count = 1;
    }
    return count;
}
void drag_drop_handle(HDROP hDrop)
{
    if (is_cue_open_perhaps())
    {
        UINT count = DragQueryFileA(hDrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i)
        {
            char path[MAX_PATH];
            if (DragQueryFileA(hDrop, i, path, MAX_PATH))
            {
                cue_handle_drop(path);
            }
        }
        DragFinish(hDrop);
        return;
    }

    UINT count = DragQueryFileA(hDrop, 0xFFFFFFFF, nullptr, 0);
    int n_added = 0;
    for (UINT i = 0; i < count; ++i)
    {
        char path[MAX_PATH];
        if (DragQueryFileA(hDrop, i, path, MAX_PATH))
        {
            if (_app_mode == app_mode::write_mode)
            {
                if (is_img_file(path))
                {
                    open_img_path(path);
                }
                else
                {
                    cd_error("ERROR", "Only supported disc image files can be dropped in this mode.\n(.iso .bin .cue .img .ccd .mds .mdf and related; .nrg is not supported yet.)");
                }
            }
            else if (_app_mode == app_mode::audio_cd)
            {
                if (_is_audio(path))
                {
                    audio_tracks.add_path(path);
                    ++n_added;
                }
                else if (const char *dot = strrchr(path, '.'); dot && _stricmp(dot, ".cue") == 0)
                {
                    FILE *f = fopen(path, "r");
                    if (f)
                    {
                        char dir[MAX_PATH];
                        strncpy(dir, path, sizeof(dir) - 1);
                        dir[sizeof(dir) - 1] = '\0';
                        char *cut = strrchr(dir, '\\');
                        if (!cut)
                            cut = strrchr(dir, '/');
                        if (cut)
                            *(cut + 1) = '\0';
                        else
                            dir[0] = '\0';

                        char line[640];
                        while (fgets(line, sizeof(line), f))
                        {
                            char *p = line;
                            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                                ++p;
                            if (_strnicmp(p, "FILE ", 5) != 0)
                                continue;
                            p += 5;
                            while (*p == ' ' || *p == '\t')
                                ++p;
                            if (*p != '"')
                                continue;
                            ++p;
                            char *q = strchr(p, '"');
                            if (!q)
                                continue;
                            *q = '\0';

                            std::string audio_path = p;
                            if (audio_path.empty())
                                continue;
                            if (audio_path[1] != ':' && audio_path[0] != '\\' && audio_path[0] != '/')
                                audio_path = std::string(dir) + "\\" + audio_path;

                            if (_is_audio(audio_path.c_str()))
                            {
                                audio_tracks.add_path(audio_path);
                                ++n_added;
                            }
                            else if (GetFileAttributesA(audio_path.c_str()) != INVALID_FILE_ATTRIBUTES)
                            {
                                audio_tracks.add_path(audio_path);
                                ++n_added;
                            }
                        }
                        fclose(f);
                    }
                }
                else
                {
                    cd_error("ERROR", "Unsupported extension for Audio CD mode.\nTry WAV or a format FFmpeg can decode\n(.mp3, .flac, .aac, .ogg, …);\nor use \"All files\" in Add track(s).");
                }
            }
            else if (_app_mode == app_mode::verify_mode)
            {
                if (is_img_file(path))
                {
                    strncpy(verify_compare_path, path, sizeof(verify_compare_path) - 1);
                }
                else
                {
                    cd_error("ERROR", "Only disc image files can be dropped in verify mode (.iso, .bin, .img).");
                }
            }
            else
            {
                if (_app_mode == app_mode::build_iso)
                    build_files_iso.add_path(path);
                else
                    build_files_disc.add_path(path);
                int c = count_files_recursive(path);
                n_added += (c > 0 ? c : 1);
            }
        }
    }
    DragFinish(hDrop);
    if (n_added > 0 && _app_mode != app_mode::write_mode && _app_mode != app_mode::audio_cd)
        LOG_INFOF("Added %d item(s) for ISO build", n_added);
    if (count > 0 && _app_mode != app_mode::write_mode && _app_mode != app_mode::build_mode && _app_mode != app_mode::build_iso && _app_mode != app_mode::audio_cd && _app_mode != app_mode::verify_mode)
    {
        request_mode_change(app_mode::build_mode); // m; What's the occasion you don't snake_case on this?
                                                   // c; pyside taught me that "dropEvent" stays as that
                                                   // m; I don't know why I asked I don't understand Qt.
                                                   // c; yeah cos you code in electron slop
                                                   // m; Ouch??
    }
}

// end