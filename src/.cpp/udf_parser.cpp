// udf_parser.cpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "udf_parser.hpp" // c; mike you happen to have an BDs?
#include "logger.hpp"     // m; Yeah, but my drive to read / write them doesn't work.
#include <cstring>        // c; fuck.
static bool udf_find_revision(image_context &ctx, std::string &out_rev)
{
    uint8_t buf[2048];
    for (uint32_t sector = 16; sector < 512; ++sector)
    {
        if (!image_read_blocks(ctx, sector, 1, buf))
            return false;
        if (memcmp(&buf[1], "NSR02", 5) == 0)
        {
            out_rev = "2.00";
            return true;
        }
        if (memcmp(&buf[1], "NSR03", 5) == 0)
        {
            out_rev = "2.01+";
            return true;
        }
        if (memcmp(&buf[1], "TEA01", 5) == 0)
            break;
    }
    return false;
}
bool udf_parse_descriptors(image_context &ctx, udf_info &out_info)
{
    out_info.valid = false;
    out_info.volume_id = "";
    out_info.udf_revision = "";
    out_info.volume_space_size = 0;
    if (!ctx.is_valid)
    {
        LOG_ERR("udf_parse_descriptors: invalid image context");
        return false;
    }
    if (!udf_find_revision(ctx, out_info.udf_revision))
    {
        LOG_INFO("udf_parse_descriptors: no UDF VRS descriptor");
        return false;
    }
    uint8_t buf[2048];
    bool found_pvd = false;
    for (uint32_t sector = 256; sector < 8192; ++sector)
    {
        if (!image_read_blocks(ctx, sector, 1, buf))
            return false;
        uint16_t tag_id = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        if (tag_id == 1)
        {
            char vol_id[33];
            memcpy(vol_id, &buf[24], 32);
            vol_id[32] = '\0';
            for (int i = 31; i >= 0; --i)
            {
                if (vol_id[i] == ' ')
                    vol_id[i] = '\0';
                else
                    break;
            }
            out_info.volume_id = vol_id;
            found_pvd = true;
        }
        else if (tag_id == 6)
        {
            uint32_t l = (uint32_t)buf[212] | ((uint32_t)buf[213] << 8) | ((uint32_t)buf[214] << 16) | ((uint32_t)buf[215] << 24);
            out_info.volume_space_size = l;
        }
        else if (tag_id == 8)
        {
            break;
        }
    }
    if (!found_pvd)
    {
        LOG_INFO("udf_parse_descriptors: no UDF primary volume descriptor");
        return false;
    }
    out_info.valid = true;
    LOG_INFOF("udf_parse_descriptors: valid UDF %s; volume '%s', size %u sectors", out_info.udf_revision.c_str(), out_info.volume_id.c_str(), out_info.volume_space_size);
    return true;
}

// end