// config.hpp
// last updated: 04/06/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
#include <string>
struct pabs_config
{
    int cd_write_speed;
    int dvd_write_speed;
    int cd_read_speed;
    int dvd_read_speed;
    bool verify_after_burn;
    bool eject_when_done;
    int write_mode;
    bool use_mode_select;
    int sfx_volume;
    bool mute_success_sfx;
    bool mute_error_sfx;
    bool mute_info_sfx;
    int iso_mode;
    bool where_audio_go;
    std::string ffmpeg_override;
    std::string volume_label;
    std::string default_drive_path;
    std::string last_image_path;
    std::string last_read_path;
    std::string last_browse_dir;
};
void config_load(pabs_config &out_cfg);
void config_save(const pabs_config &cfg);
extern pabs_config _config;
void set_sfx_vol(); // m; Is this meant to be a seperate function?
                    // c; think so
                    // m; Thanks a lot...

// end