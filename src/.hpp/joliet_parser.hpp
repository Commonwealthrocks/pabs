// joliet_parser.hpp
// last updated: 23/05/2026
// cmake -G "Ninja" ..
// ninja
#pragma once
#include "__image.hpp"
#include <cstdint>
#include <string>
struct joliet_info
{
    bool valid;
    std::string volume_id;
    uint32_t volume_space_size;
    uint32_t svd_sector;
};
bool joliet_parse_svd(image_context &ctx, joliet_info &out_info);

// end