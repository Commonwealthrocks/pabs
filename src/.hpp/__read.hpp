// __read.hpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
#include "__drives.hpp"
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
struct read_context
{
    drive_handle *drive;
    std::string output_path;
    std::atomic<bool> is_running;
    std::atomic<bool> abort_requested;
    std::atomic<bool> disc_removed; // m; This single line caused me too much trouble.
    std::atomic<float> progress_percent;
    std::atomic<float> read_speed_mbps;
    std::atomic<uint32_t> elapsed_seconds;
    char status_text[64];
    std::thread *worker_thread;
    std::mutex status_mutex;
};
void read_init(read_context &ctx, drive_handle *drive, const std::string &output_path);
bool read_start(read_context &ctx);
void read_abort(read_context &ctx);

// end