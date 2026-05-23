// logger.cpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "logger.hpp"
#include <windows.h>
#include <cstdio>
#include "config.hpp"
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <mmsystem.h>
#include <io.h>
#include <mutex>
static FILE *log_file = nullptr;
static bool initialized = false;
static log_entry ring[LOG_R_SIZE] = {};
static int ring_head = 0;
static int ring_count = 0;
static std::mutex log_mutex;
std::mutex &log_get_mutex()
{
    return log_mutex;
}
static const char *level_tag(log_level level) // m; We'll define the logger by 5-ish levels
{
    switch (level)
    {
    case log_level::ok:
        return "[ OK ]            ";
    case log_level::info:
        return "[ INFO ]          ";
    case log_level::error:
        return "[ ERROR ]         ";
    case log_level::incoming_fatal:
        return "[ INCOMING FATAL ]"; // m; The hell is INCOMING FATAL bro
    case log_level::fatal:           // c; printf("figure");
        return "[ FATAL ]         "; // m; ???
    case log_level::wat:
        return "[ ??? ]           ";
    default:
        return "[ ??? ]           ";
    }
}
static void timestamp(char *buf, size_t len)
{
    time_t now = time(nullptr);
    struct tm t = {};
    if (localtime_s(&t, &now) == 0)
        strftime(buf, len, "%d/%m/%Y %H:%M:%S", &t);
    else
        strncpy(buf, "00/00/0000 00:00:00", len);
}
bool log_init(const char *log_path)
{
    ring_head = 0;
    ring_count = 0;
    memset(ring, 0, sizeof(ring));
    if (log_path)
    {
        log_file = fopen(log_path, "a");
        if (!log_file)
        {
            fprintf(stderr, "%s failed to open log file: %s\n", level_tag(log_level::error), log_path); // c; yo PEP8 really hates these lines i'm making
            return false;                                                                               // m; Dawg it's C++ what are you saying
        }
        char ts[24] = {};
        timestamp(ts, sizeof(ts));
        fprintf(log_file,
                "\n---------------------------------------\n"
                "  PABS session started -> %s\n"
                "----------------------------------------\n",
                ts);
        fflush(log_file);
    }
    initialized = true;
    return true;
}
void log_shutdown()
{
    if (log_file)
    {
        char ts[24] = {};
        timestamp(ts, sizeof(ts));
        fprintf(log_file,
                "---------------------------------------\n"
                "  PABS session ended -> %s\n"
                "---------------------------------------\n",
                ts);
        fflush(log_file);
        fclose(log_file);
        log_file = nullptr;
    }
    initialized = false;
}
static void ring_push(log_level level, const char *func, const char *message)
{
    log_entry &e = ring[ring_head];
    e.level = level;
    strncpy(e.func, func, sizeof(e.func) - 1);
    strncpy(e.message, message, sizeof(e.message) - 1);
    e.func[sizeof(e.func) - 1] = '\0';
    e.message[sizeof(e.message) - 1] = '\0';
    ring_head = (ring_head + 1) % LOG_R_SIZE;
    if (ring_count < LOG_R_SIZE)
        ++ring_count;
}
void log_write(log_level level, const char *func, const char *message)
{
    std::lock_guard<std::mutex> lock(log_mutex);
    const char *tag = level_tag(level);
    fprintf(stderr, "%s  %s  -  %s\n", tag, func, message);
    if (log_file)
    {
        char ts[24] = {};
        timestamp(ts, sizeof(ts));
        fprintf(log_file, "%s  %s  %s  -  %s\n", ts, tag, func, message);
        fflush(log_file);
    }
    ring_push(level, func, message);
    if (level == log_level::error || level == log_level::incoming_fatal || level == log_level::fatal)
    {
        if (!_config.mute_error_sfx)
        {
            PlaySoundA("SystemHand", NULL, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
        }
    }
}
void log_writef(log_level level, const char *func, const char *fmt, ...)
{
    char buf[512] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    log_write(level, func, buf);
}
const log_entry *log_ring_entries()
{
    return ring;
}
int log_ring_head()
{
    return ring_head;
}
int log_ring_count()
{
    return ring_count;
}
void _log_clear()
{
    std::lock_guard<std::mutex> lock(log_mutex);
    ring_head = 0;
    ring_count = 0;
    memset(ring, 0, sizeof(ring));
}
void log_wipe()
{
    std::lock_guard<std::mutex> lock(log_mutex);
    ring_head = 0;
    ring_count = 0;
    memset(ring, 0, sizeof(ring));
    if (log_file)
    {
        fflush(log_file);
        fseek(log_file, 0, SEEK_SET);
#ifdef _WIN32
        SetEndOfFile((HANDLE)_get_osfhandle(_fileno(log_file)));
#else
        ftruncate(fileno(log_file), 0);
#endif
        char ts[24] = {};
        timestamp(ts, sizeof(ts));
        fprintf(log_file,
                "\n---------------------------------------\n"
                "  PABS log wiped -> %s\n"
                "----------------------------------------\n",
                ts);
        fflush(log_file);
    }
}

// end