// __read.cpp
// last updated: 28/05/2026
// cmake -G "Ninja" ..
// ninja
#include "__read.hpp"
#include "logger.hpp"
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <chrono>
#include <vector>
#pragma pack(push, 1)
struct wav_header // m; We should account for ripping raw PCM audio,
                  // >> in that case add a WAV / RIFF header to play it normally.
{
    char riff_tag[4] = {'R', 'I', 'F', 'F'};
    uint32_t riff_len;
    char wave_tag[4] = {'W', 'A', 'V', 'E'};
    char fmt_tag[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_len = 16;
    uint16_t audio_format = 1;
    uint16_t num_channels = 2;
    uint32_t sample_rate = 44100;
    uint32_t byte_rate = 44100 * 2 * 2;
    uint16_t block_align = 4;
    uint16_t bits_per_sample = 16;
    char data_tag[4] = {'d', 'a', 't', 'a'};
    uint32_t data_len;
};
#pragma pack(pop)
#define SET_STATUS(ctx, text)                                                 \
    do                                                                        \
    {                                                                         \
        std::lock_guard<std::mutex> _lk((ctx)->status_mutex);                 \
        snprintf((ctx)->status_text, sizeof((ctx)->status_text), "%s", text); \
    } while (0)
static void read_thread_func(read_context *ctx)
{
    ctx->is_running = true;
    LOG_INFO("Read engine starting");
    SET_STATUS(ctx, "Initializing...");
    HANDLE hDrive = INVALID_HANDLE_VALUE;
    HANDLE hOut = INVALID_HANDLE_VALUE;
    GET_LENGTH_INFORMATION lenInfo;
    ZeroMemory(&lenInfo, sizeof(lenInfo));
    DWORD bytesReturned = 0;
    uint64_t total_bytes = 0;
    if (!ctx->drive || ctx->output_path.empty())
    {
        LOG_ERR("Invalid drive or output path");
        SET_STATUS(ctx, "[ ERROR ] Setup failed");
        ctx->is_running = false;
        return;
    }
    hDrive = ctx->drive->win_handle;
    if (hDrive == INVALID_HANDLE_VALUE || !hDrive)
    {
        LOG_ERR("Invalid drive handle");
        SET_STATUS(ctx, "[ ERROR ] Invalid drive");
        goto cleanup;
    }
    if (ctx->is_audio_disc)
    {
        SET_STATUS(ctx, "Ripping audio CD...");
        CDROM_TOC toc;
        ZeroMemory(&toc, sizeof(toc));
        DWORD toc_ret = 0;
        if (!DeviceIoControl(hDrive, IOCTL_CDROM_READ_TOC, nullptr, 0, &toc, sizeof(toc), &toc_ret, nullptr))
        {
            LOG_ERR("Failed to read TOC for audio CD ripping");
            SET_STATUS(ctx, "[ ERROR ] TOC failed");
            goto cleanup;
        }
        int first = toc.FirstTrack;
        int last = toc.LastTrack;
        uint64_t total_sectors_to_rip = 0;
        uint64_t sectors_ripped = 0;
        struct track_info
        {
            int track_num;
            uint32_t start_lba;
            uint32_t end_lba;
        };
        std::vector<track_info> tracks;
        for (int t = first; t <= last; ++t)
        {
            int idx = t - first;
            int next_idx = idx + 1;
            uint32_t lba1 = (toc.TrackData[idx].Address[1] * 60 * 75) + (toc.TrackData[idx].Address[2] * 75) + toc.TrackData[idx].Address[3];
            uint32_t lba2 = (toc.TrackData[next_idx].Address[1] * 60 * 75) + (toc.TrackData[next_idx].Address[2] * 75) + toc.TrackData[next_idx].Address[3];
            if (lba1 > 150)
                lba1 -= 150;
            else
                lba1 = 0;
            if (lba2 > 150)
                lba2 -= 150;
            else
                lba2 = 0;

            tracks.push_back({t, lba1, lba2});
            total_sectors_to_rip += (lba2 - lba1);
        }
        const DWORD SECTOR_SIZE = 2352;
        const DWORD CHUNK_SECTORS = 16;
        BYTE *buffer = (BYTE *)VirtualAlloc(nullptr, CHUNK_SECTORS * SECTOR_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        auto start_time = std::chrono::steady_clock::now();
        auto last_update_time = start_time;
        uint64_t last_bytes = 0;
        for (const auto &trk : tracks)
        {
            if (ctx->abort_requested)
                break;
            char t_status[64];
            snprintf(t_status, sizeof(t_status), "Ripping track %02d...", trk.track_num);
            SET_STATUS(ctx, t_status);
            char out_path[MAX_PATH];
            snprintf(out_path, sizeof(out_path), "%s\\Track %02d.wav", ctx->output_path.c_str(), trk.track_num);
            HANDLE hOutTrack = CreateFileA(out_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hOutTrack == INVALID_HANDLE_VALUE)
                continue;
            uint32_t track_sectors = trk.end_lba - trk.start_lba;
            uint32_t track_bytes = track_sectors * SECTOR_SIZE;
            wav_header wh;
            wh.data_len = track_bytes;
            wh.riff_len = 36 + track_bytes;
            DWORD w;
            WriteFile(hOutTrack, &wh, sizeof(wh), &w, nullptr);
            uint32_t current_lba = trk.start_lba;
            while (current_lba < trk.end_lba && !ctx->abort_requested)
            {
                uint32_t s_to_read = CHUNK_SECTORS;
                if (current_lba + s_to_read > trk.end_lba)
                    s_to_read = trk.end_lba - current_lba;
                RAW_READ_INFO rri;
                rri.DiskOffset.QuadPart = (uint64_t)current_lba * 2048;
                rri.SectorCount = s_to_read;
                rri.TrackMode = CDDA;
                DWORD raw_read_ret = 0;
                BOOL ok = DeviceIoControl(hDrive, IOCTL_CDROM_RAW_READ, &rri, sizeof(rri), buffer, s_to_read * SECTOR_SIZE, &raw_read_ret, nullptr);
                if (!ok || raw_read_ret == 0)
                {
                    ZeroMemory(buffer, s_to_read * SECTOR_SIZE);
                    LOG_WARN("Raw read error. Padding with silence.");
                }
                WriteFile(hOutTrack, buffer, s_to_read * SECTOR_SIZE, &w, nullptr);
                current_lba += s_to_read;
                sectors_ripped += s_to_read;
                ctx->progress_percent = (float)sectors_ripped / (float)total_sectors_to_rip * 100.0f;
                auto now = std::chrono::steady_clock::now();
                auto delta_sec = std::chrono::duration<float>(now - last_update_time).count();
                if (delta_sec >= 0.5f)
                {
                    float speed_bps = ((sectors_ripped * SECTOR_SIZE) - last_bytes) / delta_sec;
                    ctx->read_speed_mbps = speed_bps / (1024.0f * 1024.0f);
                    last_update_time = now;
                    last_bytes = sectors_ripped * SECTOR_SIZE;
                }
                ctx->elapsed_seconds = (uint32_t)std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            }
            CloseHandle(hOutTrack);
        }
        VirtualFree(buffer, 0, MEM_RELEASE);
        if (ctx->abort_requested)
        {
            LOG_WARN("Audio Rip aborted by user.");
            SET_STATUS(ctx, "Aborted");
        }
        else
        {
            LOG_OK("Audio Rip finished successfully.");
            SET_STATUS(ctx, "Finished");
            ctx->progress_percent = 100.0f;
            ctx->read_speed_mbps = 0.0f;
        }
        goto cleanup;
    }
    {
        DWORD dwBytes;
        DeviceIoControl(hDrive, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &dwBytes, nullptr);
    }
    SET_STATUS(ctx, "Getting disc space...");
    if (DeviceIoControl(hDrive, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &lenInfo, sizeof(lenInfo), &bytesReturned, nullptr))
    {
        total_bytes = lenInfo.Length.QuadPart;
    }
    else
    {
        LARGE_INTEGER size;
        if (GetFileSizeEx(hDrive, &size))
        {
            total_bytes = size.QuadPart;
        }
        else
        {
            LOG_ERR("Failed to determine disc space");
            SET_STATUS(ctx, "[ ERROR ] Space unknown");
            goto cleanup;
        }
    }
    if (total_bytes == 0)
    {
        LOG_ERR("Disc appears to be empty");     // m; What about edge cases?
        SET_STATUS(ctx, "[ ERROR ] Disc empty"); // c; they're edge cases.
        goto cleanup;                            // m; Fair point.
    }
    LOG_INFOF("Disc capacity: %llu bytes", (unsigned long long)total_bytes);
    {
        CDROM_TOC toc;
        ZeroMemory(&toc, sizeof(toc));
        DWORD toc_ret = 0;
        BOOL toc_ok = DeviceIoControl(hDrive, IOCTL_CDROM_READ_TOC, nullptr, 0, &toc, sizeof(toc), &toc_ret, nullptr);
        if (toc_ok)
        {
            bool has_data = false;
            for (int t = toc.FirstTrack; t <= toc.LastTrack; ++t)
            {
                int idx = t - toc.FirstTrack;
                if (idx >= 0 && idx < MAXIMUM_NUMBER_TRACKS)
                {
                    if (toc.TrackData[idx].Control & 0x04)
                    {
                        has_data = true;
                        break;
                    }
                }
            }
            if (!has_data)
            {
                LOG_ERR("Raw PCM data is not supported in this mode; no descriptor (9660) was found at sector 16 of the selected disc");
                SET_STATUS(ctx, "[ ERROR ] Audio disc; not supported");
                goto cleanup;
            }
        }
    }
    {
        BYTE *pvd = (BYTE *)VirtualAlloc(nullptr, 2048, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD pvd_read = 0;
        LARGE_INTEGER seek16;
        seek16.QuadPart = 16LL * 2048;
        SetFilePointerEx(hDrive, seek16, nullptr, FILE_BEGIN);
        if (ReadFile(hDrive, pvd, 2048, &pvd_read, nullptr) && pvd_read == 2048)
        {
            if (pvd[0] == 1 && memcmp(&pvd[1], "CD001", 5) == 0)
            {
                uint32_t vol_sectors = 0;
                memcpy(&vol_sectors, &pvd[80], 4);
                uint64_t vol_bytes = (uint64_t)vol_sectors * 2048;
                if (vol_bytes > 0 && vol_bytes < total_bytes)
                {
                    LOG_INFOF("Filesystem is smaller than physical disc (%llu vs %llu bytes); capping ISO read to avoid unreadable run out blocks.", (unsigned long long)vol_bytes, (unsigned long long)total_bytes);
                    total_bytes = vol_bytes;
                }
            }
        }
        VirtualFree(pvd, 0, MEM_RELEASE);
        LARGE_INTEGER rewind;
        rewind.QuadPart = 0;
        SetFilePointerEx(hDrive, rewind, nullptr, FILE_BEGIN);
    }
    SET_STATUS(ctx, "Creating ISO file...");
    hOut = CreateFileA(ctx->output_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE)
    {
        LOG_ERR("Failed to create output ISO file");
        SET_STATUS(ctx, "[ ERROR ] Create file failed");
        goto cleanup;
    }
    SET_STATUS(ctx, "Reading...");
    LOG_INFO("Starting read loop");
    {
        const DWORD SECTOR_SIZE = 2048;
        const DWORD BUFFER_SIZE = 32 * 2048;
        BYTE *buffer = (BYTE *)VirtualAlloc(nullptr, BUFFER_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        uint64_t bytes_read_total = 0;
        auto start_time = std::chrono::steady_clock::now();
        auto last_update_time = start_time;
        auto last_disc_check_time = start_time;
        uint64_t last_bytes = 0;
        while (bytes_read_total < total_bytes && !ctx->abort_requested)
        {
            auto now_check = std::chrono::steady_clock::now();
            if (std::chrono::duration<float>(now_check - last_disc_check_time).count() >= 2.0f)
            {
                last_disc_check_time = now_check;
                if (!drives_has_media(ctx->drive->info.path))
                {
                    LOG_ERR("Disc removed during read");       // m; Do people really do this?
                    SET_STATUS(ctx, "[ ERROR ] Disc removed"); // c; why else would i add it?
                    ctx->disc_removed = true;
                    break;
                }
            }
            DWORD to_read = BUFFER_SIZE;
            if (total_bytes - bytes_read_total < BUFFER_SIZE)
            {
                to_read = (DWORD)(total_bytes - bytes_read_total);
            }
            DWORD aligned_read = to_read;
            if (aligned_read % SECTOR_SIZE != 0)
            {
                aligned_read = ((aligned_read / SECTOR_SIZE) + 1) * SECTOR_SIZE;
            }
            DWORD read = 0;
            if (!ReadFile(hDrive, buffer, aligned_read, &read, nullptr) || read == 0)
            {
                LOG_ERRF("Read error at offset %llu", (unsigned long long)bytes_read_total);
                break;
            }
            DWORD to_write = (read > to_read) ? to_read : read;
            DWORD written = 0;
            if (!WriteFile(hOut, buffer, to_write, &written, nullptr) || written != to_write)
            {
                LOG_ERR("Write error to ISO file");
                break;
            }
            bytes_read_total += to_write;
            ctx->progress_percent = (float)bytes_read_total / (float)total_bytes * 100.0f;
            auto now = std::chrono::steady_clock::now();
            auto delta_sec = std::chrono::duration<float>(now - last_update_time).count();
            if (delta_sec >= 0.5f)
            {
                float speed_bps = (bytes_read_total - last_bytes) / delta_sec;
                ctx->read_speed_mbps = speed_bps / (1024.0f * 1024.0f);
                last_update_time = now;
                last_bytes = bytes_read_total;
            }
            ctx->elapsed_seconds = (uint32_t)std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        }
        VirtualFree(buffer, 0, MEM_RELEASE);
        CloseHandle(hOut);
        if (ctx->abort_requested)
        {
            LOG_WARN("Read aborted by user, deleting partial ISO");
            SET_STATUS(ctx, "Aborted");
            DeleteFileA(ctx->output_path.c_str());
        }
        else if (bytes_read_total >= total_bytes)
        {
            LOG_OK("ISO creation finished successfully");
            SET_STATUS(ctx, "Finished");
            ctx->progress_percent = 100.0f;
            ctx->read_speed_mbps = 0.0f;
        }
        else
        {
            LOG_ERR("ISO creation failed (incomplete read / write).");
            SET_STATUS(ctx, "[ ERROR ] Read failed");
        }
    }
cleanup:
    if (ctx->drive && ctx->drive->valid)
    {
        drives_close(*ctx->drive);
    }
    LOG_INFO("Read engine shutting down");
    ctx->is_running = false;
}
void read_init(read_context &ctx, drive_handle *drive, const std::string &output_path, bool is_audio)
{
    if (ctx.worker_thread)
    {
        if (ctx.worker_thread->joinable())
            ctx.worker_thread->join();
        delete ctx.worker_thread;
        ctx.worker_thread = nullptr;
    }
    ctx.drive = drive;
    ctx.output_path = output_path;
    ctx.is_audio_disc = is_audio;
    ctx.is_running = false;
    ctx.abort_requested = false;
    ctx.disc_removed = false;
    ctx.progress_percent = 0.0f;
    ctx.read_speed_mbps = 0.0f; // m; Flot.
    ctx.elapsed_seconds = 0;
    {
        std::lock_guard<std::mutex> lk(ctx.status_mutex);
        snprintf(ctx.status_text, sizeof(ctx.status_text), "Idle");
    }
    ctx.worker_thread = nullptr;
}
bool read_start(read_context &ctx)
{
    if (ctx.is_running)
        return false;
    ctx.abort_requested = false;
    ctx.disc_removed = false;
    ctx.progress_percent = 0.0f;
    ctx.read_speed_mbps = 0.0f;
    ctx.elapsed_seconds = 0;
    {
        std::lock_guard<std::mutex> lk(ctx.status_mutex);
        snprintf(ctx.status_text, sizeof(ctx.status_text), "Starting...");
    }
    if (ctx.worker_thread && ctx.worker_thread->joinable())
    {
        ctx.worker_thread->join();
        delete ctx.worker_thread;
    }
    ctx.worker_thread = new std::thread(read_thread_func, &ctx);
    return true;
}
void read_abort(read_context &ctx)
{
    if (ctx.is_running)
    {
        ctx.abort_requested = true;
        SET_STATUS(&ctx, "Aborting...");
    }
}

// end