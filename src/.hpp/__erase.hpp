// _erase.hpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
#include "__drives.hpp"
#include <atomic>
#include <thread>
#include <mutex>
struct erase_options
{
    bool full_erase = false;
    bool eject_when_done = false;
};
struct erase_context
{
    drive_handle *drive = nullptr;
    erase_options options;
    std::atomic<bool> is_running{false};
    std::atomic<bool> abort_requested{false};
    std::atomic<uint32_t> elapsed_seconds{0};
    std::atomic<uint32_t> estimated_total_seconds{0};
    std::mutex status_mutex;
    char status_text[256] = {};
    std::thread *worker_thread = nullptr;
};
void erase_init(erase_context &ctx, drive_handle *drive, const erase_options &options);
bool erase_start(erase_context &ctx);
void erase_abort(erase_context &ctx);
void erase_progress(const erase_context &ctx, uint32_t &out_elapsed_sec, uint32_t &out_estimated_total_sec);

// end