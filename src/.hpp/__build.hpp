// __build.hpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
#include "__drives.hpp"
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>
enum class iso_mode : int
{
    iso_9660 = 0,
    joliet = 1,
    udf = 2,
    hybrid = 3
};
struct build_entry
{
    std::string path;
    std::string display_name;
    bool is_directory;
    uint64_t size_bytes;
    bool selected;
};
struct _build_list
{
    std::vector<build_entry> entries;
    void add_path(const std::string &path);
    void rm_selected();
    void clear();
    uint64_t total_size() const;
};
struct build_context
{
    _build_list *file_list;
    drive_handle *drive;
    std::string volume_label;
    int speed_multiplier;
    bool verify_after_burn;
    bool eject_when_done;
    bool build_to_iso;
    std::string iso_output_path;
    iso_mode filesystem_mode;
    std::atomic<bool> is_running;
    std::atomic<bool> abort_requested;
    std::atomic<float> progress_percent;
    std::atomic<float> write_speed_mbps;
    std::atomic<uint32_t> elapsed_seconds;
    char status_text[64];
    std::mutex status_mutex;
    std::thread *worker_thread;
};
void build_init(build_context &ctx, _build_list *files, drive_handle *drive, const std::string &volume_label, int speed, bool verify_mode, bool eject, iso_mode mode);
void build_init_iso(build_context &ctx, _build_list *files, const std::string &output_path, const std::string &volume_label, iso_mode mode);
bool build_start(build_context &ctx);
void build_abort(build_context &ctx);

// end