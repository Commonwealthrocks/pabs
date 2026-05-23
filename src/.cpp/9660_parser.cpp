// 9660_parser.cpp
/// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "9660_parser.hpp"
#include "logger.hpp"
#include <cstring>
bool iso9660_parse_pvd(image_context &ctx, iso9660_info &out_info)
{
    out_info.valid = false;
    out_info.volume_id = "";
    out_info.volume_space_size = 0;
    if (!ctx.is_valid)
    {
        LOG_ERR("iso9660_parse_pvd: invalid image context");
        return false;
    }
    uint8_t sector_buf[2048];
    if (!image_read_blocks(ctx, 16, 1, sector_buf))
    {
        LOG_ERR("iso9660_parse_pvd: failed to read sector 16");
        return false;
    }
    if (sector_buf[0] != 1)
    {
        LOG_ERRF("iso9660_parse_pvd: expected VDT 1 at sector 16, got %d", sector_buf[0]);
        return false;
    }
    if (memcmp(&sector_buf[1], "CD001", 5) != 0)
    {
        LOG_ERR("iso9660_parse_pvd: missing 'CD001' standard identifier");
        return false;
    }
    char vol_id[33];
    memcpy(vol_id, &sector_buf[40], 32);
    vol_id[32] = '\0';
    for (int i = 31; i >= 0; i--)
    {
        if (vol_id[i] == ' ')
            vol_id[i] = '\0';
        else
            break;
    }
    out_info.volume_id = vol_id;
    uint32_t size_le;
    memcpy(&size_le, &sector_buf[80], 4); // m; OK you were right that OS project does come in hand.
    out_info.volume_space_size = size_le; // c; don't doubt me butcher
    out_info.valid = true;
    LOG_INFOF("iso9660_parse_pvd: valid PVD found; volume '%s', size %u sectors", out_info.volume_id.c_str(), out_info.volume_space_size);
    return true;
}

// end