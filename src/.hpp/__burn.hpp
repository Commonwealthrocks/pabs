// __burn.hpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
#include "__drives.hpp"
#include "__image.hpp"
#include <atomic>
#include <thread>
#include <mutex>
struct burn_options
{
    int speed_multiplier;
    bool verify_after_burn;
    bool eject_when_done;
    int write_mode;
    bool use_mode_select;
    std::string volume_label;
};
struct burn_context
{
    drive_handle *drive;
    image_context *image;
    burn_options options;
    std::atomic<bool> is_running;
    std::atomic<bool> abort_requested;
    std::atomic<float> progress_percent;
    std::atomic<float> write_speed_mbps;
    std::atomic<uint32_t> elapsed_seconds;
    char status_text[64];
    std::thread *worker_thread;
    std::mutex status_mutex;
};
void burn_init(burn_context &ctx, drive_handle *drive, image_context *image, const burn_options &options);
bool burn_start(burn_context &ctx);
void burn_abort(burn_context &ctx);
void burn_progress(const burn_context &ctx, float &out_percent, float &out_mbps, uint32_t &out_elapsed_sec);
bool burn_is_rw(const char *drive_path);

// end