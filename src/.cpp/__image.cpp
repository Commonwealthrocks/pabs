// __image.cpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "__image.hpp"
#include "logger.hpp"
#include "udf_parser.hpp"
#include "joliet_parser.hpp"
#include "9660_parser.hpp"
#include <string>
static bool cue_first_bin(const char *cue_path, std::string &bin_out)
{
    FILE *f = fopen(cue_path, "r");
    if (!f)
        return false;
    char dir[MAX_PATH];
    strncpy(dir, cue_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *s1 = strrchr(dir, '\\');
    char *s2 = strrchr(dir, '/');
    char *cut = (s1 && s2) ? (s1 > s2 ? s1 : s2) : (s1 ? s1 : s2);
    if (cut)
        *(cut + 1) = '\0';
    else
        dir[0] = '\0';
    char line[640];
    bool ok = false;
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
            break;
        ++p;
        char *q = strchr(p, '"');
        if (!q)
            break;
        *q = '\0';
        bin_out = std::string(dir) + p;
        ok = true;
        break;
    }
    fclose(f);
    return ok;
}
bool image_open(image_context &ctx, const char *path)
{
    ctx.path = path;
    ctx.is_valid = false;
    ctx.total_bytes = 0;
    ctx.total_sectors = 0;
    const char *dot = strrchr(path, '.');
    if (dot && _stricmp(dot, ".nrg") == 0)
    {
        LOG_ERR("NRG images are not supported yet (need a reader); use ISO or BIN / CUE.");
        ctx.win_handle = INVALID_HANDLE_VALUE;
        return false;
    }
    std::string open_path = path;
    if (dot && _stricmp(dot, ".cue") == 0)
    {
        if (!cue_first_bin(path, open_path))
        {
            LOG_ERR("Could not parse .cue (missing FILE line?)");
            ctx.win_handle = INVALID_HANDLE_VALUE;
            return false;
        }
        LOG_INFOF("cue: using data file %s", open_path.c_str());
        ctx.path = open_path;
    }
    else if (dot && _stricmp(dot, ".ccd") == 0)
    {
        std::string p(path);
        size_t d = p.find_last_of('.');
        if (d != std::string::npos)
            p.resize(d);
        p += ".img";
        if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            LOG_ERRF("CCD: expected paired image missing: %s", p.c_str());
            ctx.win_handle = INVALID_HANDLE_VALUE;
            return false;
        }
        LOG_INFOF("CCD: using %s", p.c_str());
        ctx.path = p;
    }
    else if (dot && _stricmp(dot, ".mds") == 0)
    {
        std::string p(path);
        size_t d = p.find_last_of('.');
        if (d != std::string::npos)
            p.resize(d);
        p += ".mdf";
        if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            LOG_ERRF("MDS: expected paired MDF missing: %s", p.c_str());
            ctx.win_handle = INVALID_HANDLE_VALUE;
            return false;
        }
        LOG_INFOF("MDS: using %s", p.c_str());
        ctx.path = p;
    }
    ctx.win_handle = CreateFileA(ctx.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (ctx.win_handle == INVALID_HANDLE_VALUE)
    {
        LOG_ERRF("Failed to open %s", ctx.path.c_str());
        return false;
    }
    LOG_INFOF("Opened %s", ctx.path.c_str());
    return true;
}
void image_close(image_context &ctx)
{
    if (ctx.win_handle && ctx.win_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(ctx.win_handle);
        ctx.win_handle = INVALID_HANDLE_VALUE;
        LOG_INFO("Closed image");
    }
    ctx.is_valid = false;
}
bool image_validate(image_context &ctx) // m; Sometimes the volume label might not apply
                                        // >> this should allow us to work easier with it.
{
    if (ctx.win_handle == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(ctx.win_handle, &size))
    {
        LOG_ERR("GetFileSizeEx failed");
        return false;
    }
    ctx.total_bytes = size.QuadPart;
    ctx.total_sectors = (uint32_t)(ctx.total_bytes / 2048);
    if (ctx.total_bytes % 2048 != 0)
    {
        LOG_WARNF("File size is not a multiple of 2048 bytes (size: %llu)", ctx.total_bytes);
    }
    ctx.is_valid = (ctx.total_sectors > 0);
    ctx.volume_id = "";
    ctx.has_udf = false;
    ctx.has_joliet = false;
    ctx.has_iso9660 = false;
    ctx.joliet_svd_sector = 0;
    if (ctx.is_valid)
    {
        LOG_INFOF("Valid image: %u sectors", ctx.total_sectors);
        udf_info ui;
        joliet_info ji;
        iso9660_info ii;
        if (udf_parse_descriptors(ctx, ui) && ui.valid)
        {
            ctx.has_udf = true;
        }
        if (joliet_parse_svd(ctx, ji) && ji.valid)
        {
            ctx.has_joliet = true;
            ctx.joliet_svd_sector = ji.svd_sector;
        }
        if (iso9660_parse_pvd(ctx, ii) && ii.valid)
        {
            ctx.has_iso9660 = true;
        }

        if (ctx.has_udf)
            ctx.volume_id = ui.volume_id;
        else if (ctx.has_joliet)
            ctx.volume_id = ji.volume_id;
        else if (ctx.has_iso9660)
            ctx.volume_id = ii.volume_id;
    }
    return ctx.is_valid;
}
bool image_read_blocks(image_context &ctx, uint64_t start_sector, uint32_t sector_count, void *buffer)
{
    if (!ctx.is_valid || ctx.win_handle == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER offset;
    offset.QuadPart = start_sector * 2048;
    if (!SetFilePointerEx(ctx.win_handle, offset, nullptr, FILE_BEGIN))
    {
        return false;
    }
    DWORD bytes_to_read = sector_count * 2048;
    DWORD bytes_read = 0;
    if (!ReadFile(ctx.win_handle, buffer, bytes_to_read, &bytes_read, nullptr))
    {
        return false;
    }
    return bytes_read == bytes_to_read;
}
// c; not much to comment in this file

// end