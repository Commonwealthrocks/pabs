// joliet_parser.cpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "joliet_parser.hpp"
#include "logger.hpp"
#include <cstring> // c; we could account for this in the 9660 parser but we'll seperate it
bool joliet_parse_svd(image_context &ctx, joliet_info &out_info)
{
    out_info.valid = false;
    out_info.volume_id = "";
    out_info.volume_space_size = 0;
    if (!ctx.is_valid)
    {
        LOG_ERR("joliet_parse_svd: invalid image context");
        return false;
    }
    uint8_t sector_buf[2048];
    bool found = false;
    for (uint32_t sector = 17; sector < 128; ++sector)
    {
        if (!image_read_blocks(ctx, sector, 1, sector_buf))
            return false;
        if (sector_buf[0] == 255 && memcmp(&sector_buf[1], "CD001", 5) == 0)
            break;
        if (sector_buf[0] != 2 || memcmp(&sector_buf[1], "CD001", 5) != 0)
            continue;
        if (!((sector_buf[88] == 0x25 && sector_buf[89] == 0x2F && sector_buf[90] == 0x40) ||
              (sector_buf[88] == 0x25 && sector_buf[89] == 0x2F && sector_buf[90] == 0x43) ||
              (sector_buf[88] == 0x25 && sector_buf[89] == 0x2F && sector_buf[90] == 0x45)))
            continue;
        char vol_id[33];
        memcpy(vol_id, &sector_buf[40], 32);
        vol_id[32] = '\0';
        for (int i = 31; i >= 0; --i)
        {
            if (vol_id[i] == ' ')
                vol_id[i] = '\0';
            else
                break;
        }
        uint32_t size_le = 0;
        memcpy(&size_le, &sector_buf[80], 4);
        out_info.volume_id = vol_id;
        out_info.volume_space_size = size_le;
        out_info.svd_sector = sector;
        out_info.valid = true;
        found = true;
        break;
    }
    if (!found)
    {
        LOG_INFO("joliet_parse_svd: no Joliet supplementary descriptor");
        return false;
    }
    LOG_INFOF("joliet_parse_svd: valid Joliet SVD; volume '%s', size %u sectors", out_info.volume_id.c_str(), out_info.volume_space_size);
    return true;
}

// end