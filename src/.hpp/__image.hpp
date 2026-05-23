// _image.hpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
#include <string>
#include <cstdint>
#include <windows.h>
struct image_context
{
    std::string path;
    bool is_valid;
    uint64_t total_bytes;
    uint32_t total_sectors;
    HANDLE win_handle;
    std::string volume_id;
    bool has_udf;
    bool has_joliet;
    bool has_iso9660;
    uint32_t joliet_svd_sector;
};
bool image_open(image_context &ctx, const char *path);
void image_close(image_context &ctx);
bool image_validate(image_context &ctx);
bool image_read_blocks(image_context &ctx, uint64_t start_sector, uint32_t sector_count, void *buffer);

// end