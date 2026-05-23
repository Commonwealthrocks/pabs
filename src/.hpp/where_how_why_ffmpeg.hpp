// where_how_why_ffmpeg.hpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
#include <string>
#include <vector>
#include <cstdint>
void trim_str(std::string &s);
std::string cache_audio_dir();
void cache_audio_sw();
bool ff_exe(std::string &out);
void ff_candidates(std::vector<std::string> &out);
bool ff2wav(const std::string &in_f, const std::string &out_wav);
bool ff2wav_in_ram(const std::string &in_f, std::vector<uint8_t> &out_bytes);

// end