// logger.hpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
#include <cstdint>
enum class log_level : uint8_t
{
    ok = 0,
    info = 1,
    Info = 1,
    error = 2,
    Error = 2,          // c; what is "Error" doing here when we have error?
    incoming_fatal = 3, // m; I'm not sure
    fatal = 4,          // c; YOU wrote the fucking code??
    wat = 5,
};
bool log_init(const char *log_path);
void log_shutdown();
void log_write(log_level level, const char *func, const char *message);
#define LOG_OK(msg) log_write(log_level::ok, __func__, (msg))
#define LOG_INFO(msg) log_write(log_level::info, __func__, (msg))
#define LOG_ERR(msg) log_write(log_level::error, __func__, (msg))
#define LOG_WARN(msg) log_write(log_level::incoming_fatal, __func__, (msg))
#define LOG_FATAL(msg) log_write(log_level::fatal, __func__, (msg))
#define LOG_WAT(msg) log_write(log_level::wat, __func__, (msg))
#define LOG_OKF(fmt, ...) log_writef(log_level::ok, __func__, (fmt), ##__VA_ARGS__)
#define LOG_INFOF(fmt, ...) log_writef(log_level::info, __func__, (fmt), ##__VA_ARGS__)
#define LOG_ERRF(fmt, ...) log_writef(log_level::error, __func__, (fmt), ##__VA_ARGS__)
#define LOG_WARNF(fmt, ...) log_writef(log_level::incoming_fatal, __func__, (fmt), ##__VA_ARGS__)
#define LOG_FATALF(fmt, ...) log_writef(log_level::fatal, __func__, (fmt), ##__VA_ARGS__)
#define LOG_WATF(fmt, ...) log_writef(log_level::wat, __func__, (fmt), ##__VA_ARGS__)
void log_writef(log_level level, const char *func, const char *fmt, ...);
#define LOG_R_SIZE 256
struct log_entry
{
    log_level level;
    char func[48];
    char message[192]; // c; get the message that my mind can understand
};
const log_entry *log_ring_entries();
int log_ring_head();
int log_ring_count();
void _log_clear();
void log_wipe();
#include <mutex>
std::mutex &log_get_mutex();

// end