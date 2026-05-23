// 9660_parser.hpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
#include "__image.hpp"
#include <cstdint>
#include <string>
struct iso9660_info
{
    bool valid;
    std::string volume_id;
    uint32_t volume_space_size;
};
bool iso9660_parse_pvd(image_context &ctx, iso9660_info &out_info);

// end