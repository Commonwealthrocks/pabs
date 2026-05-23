// where_how_why_ffmpeg.cpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "where_how_why_ffmpeg.hpp"
#include "config.hpp"
#include "logger.hpp"
#include <windows.h>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
void trim_str(std::string &s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    if (i > 0)
        s.erase(0, i);
}
static bool file_ok(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}
static bool dir_ok(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
static void push_uni(std::vector<std::string> &v, const std::string &x)
{
    if (x.empty())
        return;
    for (const auto &e : v)
        if (_stricmp(e.c_str(), x.c_str()) == 0)
            return;
    v.push_back(x);
}
std::string cache_audio_dir()
{
    char exe_dir[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe_dir, MAX_PATH);
    char *sep = strrchr(exe_dir, '\\');
    if (sep)
        *(sep + 1) = '\0';
    return std::string(exe_dir) + "assets\\__cache";
}
void cache_audio_sw()
{
    std::string d = cache_audio_dir();
    CreateDirectoryA(d.c_str(), nullptr);
    std::string pat = d + "\\*.wav";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            std::string fp = d + "\\" + fd.cFileName;
            if (!DeleteFileA(fp.c_str()))
                LOG_WARNF("FFmpeg cache: could not delete %s", fp.c_str());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
static bool try_override(std::string &out)
{
    std::string o = _config.ffmpeg_override;
    trim_str(o);
    if (o.empty())
        return false;
    if (file_ok(o.c_str()))
    {
        out = o;
        return true;
    }
    if (dir_ok(o.c_str()))
    {
        std::string try_exe = o;
        if (try_exe.back() != '\\' && try_exe.back() != '/')
            try_exe += '\\';
        try_exe += "ffmpeg.exe";
        if (file_ok(try_exe.c_str()))
        {
            out = try_exe;
            return true;
        }
    }
    LOG_WARNF("FFmpeg says: override not usable -> %s", o.c_str());
    return false;
}
bool ff_exe(std::string &out)
{
    out.clear();
    if (try_override(out))
    {
        LOG_INFOF("FFmpeg says: settings -> %s", out.c_str());
        return true;
    }
    char buf[MAX_PATH * 4] = {0};
    DWORD n = SearchPathA(nullptr, "ffmpeg.exe", nullptr, sizeof(buf), buf, nullptr);
    if (n > 0 && n < sizeof(buf) && file_ok(buf))
    {
        out.assign(buf);
        return true;
    }
    char exe_dir[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe_dir, MAX_PATH);
    char *sep = strrchr(exe_dir, '\\');
    if (sep)
        *(sep + 1) = '\0';
    std::string local = std::string(exe_dir) + "ffmpeg.exe";
    if (file_ok(local.c_str()))
    {
        out = local;
        return true;
    }
    LOG_ERR("FFmpeg: none (set path in settings or install FFmpeg)"); // c; mike what the FUCK is this?!??!?!?!?! also coolder says hi
                                                                      // m; Who?
                                                                      // c; the indian tech scammer
                                                                      // m; What are on Earth are you saying?
                                                                      // c; ¯\_(ツ)_/¯
    return false;
}
void ff_candidates(std::vector<std::string> &out)
{
    out.clear();
    std::string t;
    if (try_override(t))
        push_uni(out, t);
    char buf[MAX_PATH * 4] = {0};
    DWORD n = SearchPathA(nullptr, "ffmpeg.exe", nullptr, sizeof(buf), buf, nullptr);
    if (n > 0 && n < sizeof(buf) && file_ok(buf))
        push_uni(out, std::string(buf));
    char exe_dir[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe_dir, MAX_PATH);
    char *sep = strrchr(exe_dir, '\\');
    if (sep)
        *(sep + 1) = '\0';
    std::string local = std::string(exe_dir) + "ffmpeg.exe";
    if (file_ok(local.c_str()))
        push_uni(out, local);
}
static int run_process(const std::string &cmd)
{
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.hStdOutput = INVALID_HANDLE_VALUE;
    si.hStdError = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi = {};
    std::string mutable_cmd = cmd;
    BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);
    if (!ok)
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;
}
bool ff2wav(const std::string &in_f, const std::string &out_wav)
{
    std::string exe;
    if (!ff_exe(exe))
        return false;
    std::string cmd = "\"" + exe + "\" -nostdin -hide_banner -loglevel error -y -i \"" + in_f + "\" -f wav -ar 44100 -ac 2 -c:a pcm_s16le \"" + out_wav + "\"";
    int rc = run_process(cmd);
    if (rc != 0)
    {
        LOG_ERRF("ffmpeg says: exit %d src=\"%s\"", rc, in_f.c_str());
        return false;
    }
    return true;
}
bool ff2wav_in_ram(const std::string &in_f, std::vector<uint8_t> &out_bytes)
{
    out_bytes.clear();
    std::string exe;
    if (!ff_exe(exe))
        return false;
    std::string cmd = "\"" + exe + "\" -nostdin -hide_banner -loglevel error -i \"" + in_f + "\" -f wav -ar 44100 -ac 2 -c:a pcm_s16le -";
    HANDLE h_read = INVALID_HANDLE_VALUE, h_write = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&h_read, &h_write, &sa, 0))
    {
        LOG_ERR("FFmpeg says: CreatePipe failed");
        return false;
    }
    SetHandleInformation(h_read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = INVALID_HANDLE_VALUE;
    si.hStdOutput = h_write;
    si.hStdError = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi = {};
    std::string mutable_cmd = cmd;
    BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);
    CloseHandle(h_write);
    if (!ok)
    {
        CloseHandle(h_read);
        LOG_ERR("FFmpeg says: CreateProcess failed");
        return false;
    }
    uint8_t chunk[65536];
    DWORD n_read = 0;
    while (ReadFile(h_read, chunk, sizeof(chunk), &n_read, nullptr) && n_read > 0)
        out_bytes.insert(out_bytes.end(), chunk, chunk + n_read);
    CloseHandle(h_read);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exit_code != 0)
    {
        LOG_ERRF("FFmpeg says: RAM decode exit %d src=\"%s\"", (int)exit_code, in_f.c_str());
        out_bytes.clear();
        return false;
    }
    out_bytes.shrink_to_fit();
    return !out_bytes.empty();
}

// end