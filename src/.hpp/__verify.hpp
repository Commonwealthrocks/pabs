// __verify.hpp
// last updated: 27/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
#include "__drives.hpp"
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#define VERIFY_GRID_S 1000 // m; What is this measured in?
                           // c; DTTM²
                           // m; What?
                           // c; idk i just made it up
enum class verify_block_state : uint8_t
{
    unscanned = 0,
    good = 1,
    bad = 2,
    unrecorded = 3
};
struct verify_options
{
    std::string compare_image_path;
    bool eject_when_done = false;
};
struct verify_context
{
    drive_handle *drive = nullptr;
    verify_options options;
    std::atomic<bool> is_running{false};
    std::atomic<bool> abort_requested{false};
    std::atomic<bool> disc_removed{false};
    std::atomic<float> progress_percent{0.0f};
    std::atomic<float> read_speed_mbps{0.0f};
    std::atomic<uint32_t> elapsed_seconds{0};
    std::atomic<uint32_t> tot_bad_sectors{0};
    std::atomic<verify_block_state> sector_map[VERIFY_GRID_S];
    std::atomic<uint64_t> sector_mismatch[VERIFY_GRID_S];
    std::atomic<uint64_t> verify_bytes{0};
    std::mutex status_mutex;
    char status_text[256] = {};
    std::thread *worker_thread = nullptr;
};
void verify_init(verify_context &ctx, drive_handle *drive, const verify_options &options);
bool verify_start(verify_context &ctx);
void verify_abort(verify_context &ctx);
void verify_progress(const verify_context &ctx, float &out_percent, float &out_mbps, uint32_t &out_elapsed_sec, uint32_t &out_bad_sectors);

// end