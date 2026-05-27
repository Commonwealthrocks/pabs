// __audio_h.hpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
#include "__drives.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
struct audio_track
{
    std::string original_path;
    std::string pcm_path;
    std::vector<uint8_t> pcm_ram;
    std::string display_name;
    uint64_t size_bytes;
    bool selected;
    bool is_temp;
    bool is_converting;
};
struct audio_list
{
    std::vector<audio_track> entries;
    std::atomic<int> track_id{0};
    std::atomic<int> pending_converts{0};
    std::atomic<int> ok_converts{0};
    void add_path(const std::string &path);
    void rm_selected();
    void clear();
    uint64_t total_size() const;
    std::mutex list_mutex;
};
struct audio_context
{
    drive_handle *drive;
    audio_list *tracks;
    int speed_multiplier;
    bool eject_when_done;
    std::atomic<bool> is_running;
    std::atomic<bool> abort_requested;
    std::atomic<float> progress_percent;
    std::atomic<float> write_speed_mbps;
    std::atomic<uint32_t> elapsed_seconds;
    char status_text[64];
    std::thread *worker_thread;
    std::mutex status_mutex;
};
void audio_init(audio_context &ctx, drive_handle *drive, audio_list *tracks, int speed_mult, bool eject);
bool audio_start(audio_context &ctx);
void audio_abort(audio_context &ctx);
void audio_progress(const audio_context &ctx, float &out_percent, float &out_mbps, uint32_t &out_elapsed_sec);

// end