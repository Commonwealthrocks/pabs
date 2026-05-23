// udf_parser.hpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
// c; this and joliet could of been a single file...
// m; OUR UI FILE IS 3K+ LINES LET'S NOT
#pragma once
#include "__image.hpp"
#include <cstdint>
#include <string>
struct udf_info
{
    bool valid;
    std::string volume_id;
    std::string udf_revision;
    uint32_t volume_space_size;
};
bool udf_parse_descriptors(image_context &ctx, udf_info &out_info);

// end