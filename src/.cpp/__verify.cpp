// __verify.cpp
// last updated: 27/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "__verify.hpp"
#include "__imapi_com.hpp"
#include "logger.hpp"
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <shlobj.h>
#include <chrono>
#define SET_STATUS(ctx, text)                                                 \
    do                                                                        \
    {                                                                         \
        std::lock_guard<std::mutex> _lk((ctx)->status_mutex);                 \
        snprintf((ctx)->status_text, sizeof((ctx)->status_text), "%s", text); \
    } while (0)
static void verify_thread_func(verify_context *ctx)
{
    ctx->is_running = true;
    LOG_INFO("Verify engine starting");
    SET_STATUS(ctx, "Initializing...");
    HANDLE hDrive = INVALID_HANDLE_VALUE;
    HANDLE hIso = INVALID_HANDLE_VALUE;
    GET_LENGTH_INFORMATION lenInfo;
    ZeroMemory(&lenInfo, sizeof(lenInfo));
    DWORD bytesReturned = 0;
    uint64_t drive_bytes = 0;
    uint64_t verify_bytes = 0;
    bool compare_mode = !ctx->options.compare_image_path.empty();
    if (!ctx->drive)
    {
        LOG_ERR("Invalid drive context for verify");
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
    SET_STATUS(ctx, "Checking disc...");
    if (!drives_has_media(ctx->drive->info.path))
    {
        LOG_ERR("Verify requested but no disc present");
        SET_STATUS(ctx, "[ ERROR ] No disc");
        ctx->disc_removed = false;
        goto cleanup;
    }
    {
        DWORD dwBytes;
        DeviceIoControl(hDrive, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &dwBytes, nullptr);
    }
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
                LOG_ERR("Raw PCM data is not supported in this mode; no descriptor (9660) was found at sector 16 of the selected disc"); // c; oddly specific but this really ticked me off...
                SET_STATUS(ctx, "[ ERROR ] Audio disc; not supported");
                goto cleanup;
            }
            int lead_out_idx = toc.LastTrack - toc.FirstTrack + 1;
            if (lead_out_idx >= 0 && lead_out_idx < MAXIMUM_NUMBER_TRACKS)
            {
                TRACK_DATA &lo = toc.TrackData[lead_out_idx];
                uint32_t m = lo.Address[1];
                uint32_t s = lo.Address[2];
                uint32_t f = lo.Address[3];
                uint32_t lba = (m * 60 * 75) + (s * 75) + f;
                if (lba > 150)
                    lba -= 150;
                uint64_t toc_bytes = (uint64_t)lba * 2048;
                if (toc_bytes > 0)
                {
                    LOG_INFOF("TOC: %u -> %llu bytes", lba, (unsigned long long)toc_bytes);
                    drive_bytes = toc_bytes;
                }
            }
        }
    }
    SET_STATUS(ctx, "Getting disc capacity...");
    if (drive_bytes == 0)
    {
        if (DeviceIoControl(hDrive, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &lenInfo, sizeof(lenInfo), &bytesReturned, nullptr)) // m; I think I'll have better odds winning the loto
                                                                                                                                 // >> than figuring this out.
                                                                                                                                 // c; is this just straight up from the microsoft docs?
                                                                                                                                 // m; Yeah.
                                                                                                                                 // c; i'm annoyed that it works cause i can't make fun of you now
        {
            drive_bytes = lenInfo.Length.QuadPart;
        }
        else
        {
            LARGE_INTEGER size;
            if (GetFileSizeEx(hDrive, &size))
            {
                drive_bytes = size.QuadPart;
            }
            else
            {
                LOG_ERR("Failed to determine disc capacity");
                SET_STATUS(ctx, "[ ERROR ] Capacity unknown");
                goto cleanup;
            }
        }
    }
    if (drive_bytes == 0)
    {
        LOG_ERR("Disc appears to be empty");
        SET_STATUS(ctx, "[ ERROR ] Disc empty");
        goto cleanup;
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
                if (vol_bytes > 0 && vol_bytes < drive_bytes)
                {
                    LOG_INFOF("Filesystem is smaller than physical disc (%llu vs %llu bytes); capping readability check to avoid unreadable run out blocks", (unsigned long long)vol_bytes, (unsigned long long)drive_bytes);
                    drive_bytes = vol_bytes;
                }
            }
        }
        VirtualFree(pvd, 0, MEM_RELEASE);
        LARGE_INTEGER rewind;
        rewind.QuadPart = 0;
        SetFilePointerEx(hDrive, rewind, nullptr, FILE_BEGIN);
    }
    if (compare_mode)
    {
        SET_STATUS(ctx, "Opening ISO for comparison...");
        hIso = CreateFileA(ctx->options.compare_image_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hIso == INVALID_HANDLE_VALUE)
        {
            LOG_ERR("Failed to open comparison ISO file");
            SET_STATUS(ctx, "[ ERROR ] Open ISO failed");
            goto cleanup;
        }
        LARGE_INTEGER iso_size;
        if (GetFileSizeEx(hIso, &iso_size))
        {
            uint64_t iso_bytes = iso_size.QuadPart;
            uint64_t iso_aligned = (iso_bytes / 2048) * 2048;
            if (iso_bytes > drive_bytes)
            {
                LOG_WARN("ISO is larger than physical disc. Verify will only check up to disc capacity");
                verify_bytes = (drive_bytes / 2048) * 2048;
            }
            else
            {
                if (iso_bytes != iso_aligned)
                    LOG_INFOF("ISO is smaller than disc (%llu vs %llu); verifying up to sector boundary (%llu bytes)", (unsigned long long)iso_bytes, (unsigned long long)drive_bytes, (unsigned long long)iso_aligned);
                verify_bytes = iso_aligned;
                if (verify_bytes > drive_bytes)
                    verify_bytes = (drive_bytes / 2048) * 2048;
            }
        }
    }
    else
    {
        verify_bytes = (drive_bytes / 2048) * 2048;
    }
    LOG_INFOF("Verifying %llu bytes", (unsigned long long)verify_bytes);
    ctx->verify_bytes.store(verify_bytes);
    SET_STATUS(ctx, "Verifying...");
    LOG_INFO("Starting verify loop");
    {
        const DWORD SECTOR_SIZE = 2048;
        const DWORD BUFFER_SIZE = 32 * 2048;
        BYTE *buffer_drive = (BYTE *)VirtualAlloc(nullptr, BUFFER_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        BYTE *buffer_iso = nullptr;
        if (compare_mode)
        {
            buffer_iso = (BYTE *)VirtualAlloc(nullptr, BUFFER_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        uint64_t bytes_total = 0;
        auto start_time = std::chrono::steady_clock::now();
        auto last_update_time = start_time;
        auto last_disc_check_time = start_time;
        uint64_t last_bytes = 0;
        while (bytes_total < verify_bytes && !ctx->abort_requested)
        {
            auto now_check = std::chrono::steady_clock::now();
            if (std::chrono::duration<float>(now_check - last_disc_check_time).count() >= 2.0f)
            {
                last_disc_check_time = now_check;
                if (!drives_has_media(ctx->drive->info.path))
                {
                    LOG_ERR("Disc removed during verify");
                    SET_STATUS(ctx, "[ ERROR ] Disc removed");
                    ctx->disc_removed = true;
                    break;
                }
            }
            DWORD to_read = BUFFER_SIZE;
            if (verify_bytes - bytes_total < BUFFER_SIZE)
            {
                to_read = (DWORD)(verify_bytes - bytes_total);
            }
            DWORD aligned_read = to_read;
            if (aligned_read % SECTOR_SIZE != 0)
            {
                aligned_read = ((aligned_read / SECTOR_SIZE) + 1) * SECTOR_SIZE;
            }
            uint32_t start_grid_idx = (uint32_t)(((double)bytes_total / (double)verify_bytes) * VERIFY_GRID_S);
            uint32_t end_grid_idx = (uint32_t)(((double)(bytes_total + to_read) / (double)verify_bytes) * VERIFY_GRID_S);
            if (end_grid_idx >= VERIFY_GRID_S)
                end_grid_idx = VERIFY_GRID_S - 1;
            bool chunk_bad = false;
            bool chunk_unrecorded = false;
            uint64_t exact_mismatch = bytes_total;
            DWORD read_drive = 0;
            if (!ReadFile(hDrive, buffer_drive, aligned_read, &read_drive, nullptr) || read_drive == 0)
            {
                bool is_final_chunk = (bytes_total + aligned_read) >= verify_bytes;
                if (!is_final_chunk || read_drive == 0)
                {
                    if (is_final_chunk)
                    {
                        LOG_WARN("Drive read error at end of disc (unrecorded / run-out block)");
                        chunk_unrecorded = true;
                    }
                    else
                    {
                        LOG_ERRF("Drive read error at offset %llu", (unsigned long long)bytes_total);
                        chunk_bad = true;
                        ctx->tot_bad_sectors += (to_read / SECTOR_SIZE);
                    }
                    LARGE_INTEGER li;
                    li.QuadPart = bytes_total + aligned_read;
                    SetFilePointerEx(hDrive, li, nullptr, FILE_BEGIN);
                    if (compare_mode)
                        SetFilePointerEx(hIso, li, nullptr, FILE_BEGIN);
                }
            }
            else
            {
                if (compare_mode)
                {
                    DWORD read_iso = 0;
                    bool iso_ok = ReadFile(hIso, buffer_iso, aligned_read, &read_iso, nullptr);
                    if (!iso_ok && read_iso == 0)
                    {
                        LOG_ERRF("ISO read error at offset %llu", (unsigned long long)bytes_total);
                        chunk_bad = true;
                    }
                    else
                    {
                        DWORD compare_len = read_drive < read_iso ? read_drive : read_iso;
                        if (compare_len > to_read)
                            compare_len = to_read;

                        // PABS live-patches the volume label into the PVD and SVD during burn.
                        // To prevent a false mismatch, sync the volume label in the comparison buffer.
                        if (bytes_total <= 16 * 2048 && bytes_total + compare_len >= 16 * 2048 + 72)
                        {
                            uint64_t rel_offset = (16 * 2048) - bytes_total;
                            if (memcmp(&buffer_drive[rel_offset + 1], "CD001", 5) == 0 &&
                                memcmp(&buffer_iso[rel_offset + 1], "CD001", 5) == 0)
                            {
                                memcpy(&buffer_iso[rel_offset + 40], &buffer_drive[rel_offset + 40], 32);
                            }
                        }
                        if (bytes_total <= 17 * 2048 && bytes_total + compare_len >= 17 * 2048 + 72)
                        {
                            uint64_t rel_offset = (17 * 2048) - bytes_total;
                            if (memcmp(&buffer_drive[rel_offset + 1], "CD001", 5) == 0 &&
                                memcmp(&buffer_iso[rel_offset + 1], "CD001", 5) == 0)
                            {
                                memcpy(&buffer_iso[rel_offset + 40], &buffer_drive[rel_offset + 40], 32);
                            }
                        }

                        if (memcmp(buffer_drive, buffer_iso, compare_len) != 0)
                        {
                            for (DWORD k = 0; k < compare_len; ++k)
                            {
                                if (buffer_drive[k] != buffer_iso[k])
                                {
                                    exact_mismatch = bytes_total + k;
                                    break;
                                }
                            }
                            LOG_ERRF("Mismatch found at offset %llu (exact: %llu)", (unsigned long long)bytes_total, (unsigned long long)exact_mismatch);
                            chunk_bad = true;
                            ctx->tot_bad_sectors++;
                        }

                        if (read_drive != compare_len)
                        {
                            LARGE_INTEGER li;
                            li.QuadPart = bytes_total + compare_len;
                            SetFilePointerEx(hDrive, li, nullptr, FILE_BEGIN);
                        }
                        if (read_iso != compare_len)
                        {
                            LARGE_INTEGER li;
                            li.QuadPart = bytes_total + compare_len;
                            SetFilePointerEx(hIso, li, nullptr, FILE_BEGIN);
                        }
                        aligned_read = compare_len;
                    }
                }
                else
                {
                    if (read_drive < aligned_read)
                    {
                        aligned_read = read_drive;
                    }
                }
            }
            for (uint32_t i = start_grid_idx; i <= end_grid_idx; ++i)
            {
                if (chunk_bad)
                {
                    ctx->sector_map[i].store(verify_block_state::bad);
                    ctx->sector_mismatch[i].store(exact_mismatch);
                }
                else if (chunk_unrecorded)
                {
                    ctx->sector_map[i].store(verify_block_state::unrecorded);
                }
                else
                {
                    if (ctx->sector_map[i].load() == verify_block_state::unscanned)
                    {
                        ctx->sector_map[i].store(verify_block_state::good);
                    }
                }
            }
            bytes_total += aligned_read;
            if (bytes_total > verify_bytes)
                bytes_total = verify_bytes;
            ctx->progress_percent = (float)bytes_total / (float)verify_bytes * 100.0f;
            auto now = std::chrono::steady_clock::now();
            auto delta_sec = std::chrono::duration<float>(now - last_update_time).count();
            if (delta_sec >= 0.5f)
            {
                float speed_bps = (bytes_total - last_bytes) / delta_sec;
                ctx->read_speed_mbps = speed_bps / (1024.0f * 1024.0f);
                last_update_time = now;
                last_bytes = bytes_total;
            }
            ctx->elapsed_seconds = (uint32_t)std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        }
        VirtualFree(buffer_drive, 0, MEM_RELEASE);
        if (buffer_iso)
            VirtualFree(buffer_iso, 0, MEM_RELEASE);
        if (hIso != INVALID_HANDLE_VALUE)
        {
            CloseHandle(hIso);
        }
        if (ctx->abort_requested)
        {
            LOG_WARN("Verify aborted by user");
            SET_STATUS(ctx, "Aborted");
        }
        else
        {
            if (ctx->tot_bad_sectors.load() > 0)
            {
                LOG_WARN("Verify finished with errors");
                SET_STATUS(ctx, "Finished (errors found)"); // c; errors are not marked and pointed out, we are keeping them unvalued
            }
            else
            {
                if (compare_mode)
                {
                    LOG_OK("Verify finished successfully; the data matches");
                    SET_STATUS(ctx, "Match [f]");
                }
                else
                {
                    LOG_OK("Readability test finished successfully; all sectors are readable");
                    SET_STATUS(ctx, "Readable [f]");
                }
            }
            if (ctx->options.eject_when_done)
            {
                LOG_INFO("Ejecting disc");
                DWORD returned = 0;
                DeviceIoControl(hDrive, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &returned, nullptr);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::wstring wDriveStr = imapi_u8_to_w(ctx->drive->info.path);
                SHChangeNotify(SHCNE_MEDIAREMOVED, SHCNF_PATHW, wDriveStr.c_str(), NULL);
            }
            ctx->progress_percent = 100.0f;
            ctx->read_speed_mbps = 0.0f;
        }
    }
cleanup:
    if (ctx->drive && ctx->drive->valid)
    {
        drives_close(*ctx->drive);
    }
    LOG_INFO("Verify engine shutting down");
    ctx->is_running = false;
}
void verify_init(verify_context &ctx, drive_handle *drive, const verify_options &options)
{
    if (ctx.worker_thread)
    {
        if (ctx.worker_thread->joinable())
            ctx.worker_thread->join();
        delete ctx.worker_thread;
        ctx.worker_thread = nullptr;
    }
    ctx.drive = drive;
    ctx.options = options;
    ctx.is_running = false;
    ctx.abort_requested = false;
    ctx.disc_removed = false;
    ctx.progress_percent = 0.0f;
    ctx.read_speed_mbps = 0.0f;
    ctx.elapsed_seconds = 0;
    ctx.tot_bad_sectors = 0;
    for (int i = 0; i < VERIFY_GRID_S; ++i)
    {
        ctx.sector_map[i].store(verify_block_state::unscanned);
        ctx.sector_mismatch[i].store(0);
    }
    ctx.verify_bytes.store(0);
    {
        std::lock_guard<std::mutex> lk(ctx.status_mutex);
        snprintf(ctx.status_text, sizeof(ctx.status_text), "Idle");
    }
    ctx.worker_thread = nullptr;
}
bool verify_start(verify_context &ctx)
{
    if (ctx.is_running)
        return false;
    ctx.abort_requested = false;
    ctx.disc_removed = false;
    ctx.progress_percent = 0.0f;
    ctx.read_speed_mbps = 0.0f;
    ctx.elapsed_seconds = 0;
    ctx.tot_bad_sectors = 0;
    for (int i = 0; i < VERIFY_GRID_S; ++i)
    {
        ctx.sector_map[i].store(verify_block_state::unscanned);
        ctx.sector_mismatch[i].store(0);
    }
    ctx.verify_bytes.store(0);
    {
        std::lock_guard<std::mutex> lk(ctx.status_mutex);
        snprintf(ctx.status_text, sizeof(ctx.status_text), "Starting...");
    }
    if (ctx.worker_thread && ctx.worker_thread->joinable())
    {
        ctx.worker_thread->join();
        delete ctx.worker_thread;
    }
    ctx.worker_thread = new std::thread(verify_thread_func, &ctx);
    return true;
}
void verify_abort(verify_context &ctx)
{
    if (ctx.is_running)
    {
        ctx.abort_requested = true;
        SET_STATUS(&ctx, "Aborting...");
    }
}
void verify_progress(const verify_context &ctx, float &out_percent, float &out_mbps, uint32_t &out_elapsed_sec, uint32_t &out_bad_sectors)
{
    out_percent = ctx.progress_percent.load();
    out_mbps = ctx.read_speed_mbps.load();
    out_elapsed_sec = ctx.elapsed_seconds.load();
    out_bad_sectors = ctx.tot_bad_sectors.load();
}

// end