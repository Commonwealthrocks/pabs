// sfx.cpp
// last updated: 04/06/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "sfx.hpp"
#include "config.hpp"
#include <windows.h>
#include <mmsystem.h>
#include <string>
namespace sfx
{
    static std::string get_sfx_path(const char *filename)
    {
        char exe_dir[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, exe_dir, MAX_PATH);
        char *sep = strrchr(exe_dir, '\\');
        if (sep)
            *(sep + 1) = '\0';
        return std::string(exe_dir) + "assets\\sfx\\" + filename;
    }
    void error_sfx()
    {
        if (!_config.mute_error_sfx)
            PlaySoundA("C:\\Windows\\Media\\Windows Critical Stop.wav", NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    }
    void success_sfx()
    {
        if (!_config.mute_success_sfx)
            PlaySoundA(get_sfx_path("success.wav").c_str(), NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    }
    void info_sfx()
    {
        if (!_config.mute_info_sfx)
            PlaySoundA("C:\\Windows\\Media\\Speech On.wav", NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    }
}

// end