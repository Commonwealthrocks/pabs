// config.cpp
// last updated: 04/06/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "config.hpp"
#include "sfx.hpp"
#include "logger.hpp"
#include "json.hpp"  // m; Oh so we are using N's JSON?
#include <windows.h> // c; yeah why the fuck would i else curl it
#include <fstream>
#include <mmsystem.h>
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif
using json = nlohmann::json;
pabs_config _config;
void set_sfx_vol()
{
    DWORD v = (DWORD)((_config.sfx_volume / 100.0f) * 0xFFFF);
    DWORD v_both = v | (v << 16);
    waveOutSetVolume(0, v_both);
}
static char json_path[MAX_PATH] = {0};
static const char *get_json_path()
{
    if (json_path[0] == '\0')
    {
        char appdata[MAX_PATH] = {0};
        DWORD len = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            GetModuleFileNameA(nullptr, appdata, MAX_PATH);
        snprintf(json_path, MAX_PATH, "%s\\PABS", appdata);
        CreateDirectoryA(json_path, nullptr);
        snprintf(json_path, MAX_PATH, "%s\\PABS\\pabs.json", appdata);
    }
    return json_path;
}
void config_load(pabs_config &out_cfg)
{
    out_cfg.cd_write_speed = 0;
    out_cfg.dvd_write_speed = 0;
    out_cfg.cd_read_speed = 0;
    out_cfg.dvd_read_speed = 0;
    out_cfg.verify_after_burn = false;
    out_cfg.eject_when_done = true;
    out_cfg.write_mode = 0;
    out_cfg.use_mode_select = false;
    out_cfg.volume_label = "DISC";
    out_cfg.default_drive_path = "";
    out_cfg.last_image_path = "";
    out_cfg.last_read_path = "";
    out_cfg.last_browse_dir = "";
    out_cfg.sfx_volume = 100;
    out_cfg.mute_success_sfx = false;
    out_cfg.mute_error_sfx = false;
    out_cfg.mute_info_sfx = false; // c; should of been info, not "warning" from the start
    out_cfg.iso_mode = 1;
    out_cfg.where_audio_go = true;
    out_cfg.ffmpeg_override = "";
    std::ifstream f(get_json_path());
    if (f.is_open())
    {
        try
        {
            json j = json::parse(f);
            out_cfg.cd_write_speed = j.value("cd_write_speed", out_cfg.cd_write_speed);
            out_cfg.dvd_write_speed = j.value("dvd_write_speed", out_cfg.dvd_write_speed);
            out_cfg.cd_read_speed = j.value("cd_read_speed", out_cfg.cd_read_speed);
            out_cfg.dvd_read_speed = j.value("dvd_read_speed", out_cfg.dvd_read_speed);
            out_cfg.verify_after_burn = j.value("verify_after_burn", out_cfg.verify_after_burn);
            out_cfg.eject_when_done = j.value("eject_when_done", out_cfg.eject_when_done);
            out_cfg.write_mode = j.value("write_mode", out_cfg.write_mode);
            out_cfg.use_mode_select = j.value("use_mode_select", out_cfg.use_mode_select);
            out_cfg.volume_label = j.value("volume_label", out_cfg.volume_label);
            out_cfg.default_drive_path = j.value("default_drive_path", out_cfg.default_drive_path);
            out_cfg.sfx_volume = j.value("sfx_volume", out_cfg.sfx_volume);
            out_cfg.mute_success_sfx = j.value("mute_success_sfx", out_cfg.mute_success_sfx);
            out_cfg.mute_error_sfx = j.value("mute_error_sfx", out_cfg.mute_error_sfx);
            out_cfg.mute_info_sfx = j.value("mute_info_sfx", out_cfg.mute_info_sfx);
            out_cfg.iso_mode = j.value("iso_mode", out_cfg.iso_mode);
            if (out_cfg.iso_mode < 0 || out_cfg.iso_mode > 3)
                out_cfg.iso_mode = 1;
            out_cfg.where_audio_go = j.value("where_audio_go", out_cfg.where_audio_go);    // m; ?
            out_cfg.ffmpeg_override = j.value("ffmpeg_override", out_cfg.ffmpeg_override); // c; ikn, yeah?
        }
        catch (...)
        {
            LOG_ERR("Failed to parse pabs.json, using defaults");
        }
    }
    LOG_INFO("Loaded settings from pabs.json");
}
void config_save(const pabs_config &cfg)
{
    json j;
    j["cd_write_speed"] = cfg.cd_write_speed;   // m; The snake_case... It hurts to see...
    j["dvd_write_speed"] = cfg.dvd_write_speed; // c; it's MY project 😒
    j["cd_read_speed"] = cfg.cd_read_speed;
    j["dvd_read_speed"] = cfg.dvd_read_speed;
    j["verify_after_burn"] = cfg.verify_after_burn;
    j["eject_when_done"] = cfg.eject_when_done;
    j["write_mode"] = cfg.write_mode;
    j["use_mode_select"] = cfg.use_mode_select;
    j["volume_label"] = cfg.volume_label;
    j["default_drive_path"] = cfg.default_drive_path;
    j["sfx_volume"] = cfg.sfx_volume;
    j["mute_success_sfx"] = cfg.mute_success_sfx;
    j["mute_error_sfx"] = cfg.mute_error_sfx;
    j["mute_info_sfx"] = cfg.mute_info_sfx;
    j["iso_mode"] = cfg.iso_mode;
    j["where_audio_go"] = cfg.where_audio_go;
    j["ffmpeg_override"] = cfg.ffmpeg_override;
    std::ofstream f(get_json_path());
    if (f.is_open())
    {
        f << j.dump(4);
        LOG_INFO("Saved settings to pabs.json");
        sfx::success_sfx();
    }
    else
    {
        LOG_ERR("Failed to open pabs.json for writing");
    }
}

// end