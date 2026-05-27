// __drives.cpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "__drives.hpp"
#include <ntddscsi.h>
#include <devioctl.h>
#include <ntddstor.h>
#include <cstring>
#include <cctype>
#include <cstdio>
void sptd_init(sptd_packet &pkt, BYTE *data_buf, DWORD data_len, BYTE direction)
{
    memset(&pkt, 0, sizeof(pkt));
    pkt.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    pkt.sptd.SenseInfoLength = PABS_SENSE_LEN;
    pkt.sptd.DataIn = direction;
    pkt.sptd.DataBuffer = data_buf;
    pkt.sptd.DataTransferLength = data_len;
    pkt.sptd.TimeOutValue = 5;
    pkt.sptd.SenseInfoOffset = offsetof(sptd_packet, sense);
}
bool sptd_send(HANDLE h, sptd_packet &pkt)
{
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, &pkt, sizeof(pkt), &pkt, sizeof(pkt), &returned, nullptr);
    return ok == TRUE;
}
static void trim_scsi_str(char *dst, const BYTE *src, int len)
{
    memcpy(dst, src, len);
    dst[len] = '\0';
    for (int i = len - 1; i >= 0 && dst[i] == ' '; --i)
        dst[i] = '\0';
}
static drive_type guess_drive(const char *product)
{
    char lower[17] = {};
    for (int i = 0; product[i] && i < 16; ++i)
        lower[i] = (char)tolower((unsigned char)product[i]);
    if (strstr(lower, "bd") || strstr(lower, "blu"))
    {
        if (strstr(lower, "rom"))
            return drive_type::bdrom;
        return drive_type::bdrw;
    }
    if (strstr(lower, "dvd") && strstr(lower, "rw"))
        return drive_type::dvdrw;
    if (strstr(lower, "dvd") && strstr(lower, "r"))
        return drive_type::dvdrw;
    if (strstr(lower, "dvd"))
        return drive_type::dvdrom;
    if (strstr(lower, "cd-rw") || strstr(lower, "cdrw"))
        return drive_type::cdrom;
    if (strstr(lower, "cd"))
        return drive_type::cdrom;
    return drive_type::unknown; // c; forgot to make the "U" lowercase
}
int drives_enum(drive_info out_drives[PABS_MAX_DRIVES])
{
    int found = 0;
    for (char letter = 'A'; letter <= 'Z' && found < PABS_MAX_DRIVES; ++letter)
    {
        char root[4] = {letter, ':', '\\', '\0'};
        if (GetDriveTypeA(root) != DRIVE_CDROM)
            continue;
        char path[3] = {letter, ':', '\0'};
        drive_handle h = drives_open(path);
        if (!h.valid)
        {
            drives_close(h);
            continue;
        }
        out_drives[found] = h.info;
        out_drives[found].disc_present = drives_test_ready(h);
        switch (out_drives[found].type)
        {
        case drive_type::bdrw:
            out_drives[found].supported_write_speeds = {2, 4, 6, 8, 12}; // m; This seems trusty! http://www.hughsnews.ca/faqs/authoritative-blu-ray-disc-bd-faq/8-recording-and-reading-speed
            break;
        case drive_type::dvdrw:
            out_drives[found].supported_write_speeds = {1, 2, 4, 8, 16};
            break;
        case drive_type::cdrom:
            out_drives[found].supported_write_speeds = {8, 16, 24, 32, 48};
            break;
        default:
            out_drives[found].supported_write_speeds = {4, 8};
            break;
        }
        drives_close(h);
        ++found;
    }
    return found;
}
drive_handle drives_open(const char *drive_path)
{
    drive_handle handle = {};
    handle.valid = false;
    char full_path[32] = {};
    snprintf(full_path, sizeof(full_path), "\\\\.\\%s", drive_path);
    handle.win_handle = CreateFileA(full_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle.win_handle == INVALID_HANDLE_VALUE)
    {
        handle.win_handle = CreateFileA(full_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (handle.win_handle == INVALID_HANDLE_VALUE)
    {
        handle.win_handle = nullptr;
        return handle;
    }
    strncpy(handle.info.path, drive_path, sizeof(handle.info.path) - 1);
    handle.valid = true;
    drives_inquiry(handle);
    return handle;
}
void drives_close(drive_handle &handle)
{
    if (handle.win_handle && handle.win_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(handle.win_handle);
        handle.win_handle = nullptr;
    }
    handle.valid = false;
}
bool drives_test_ready(drive_handle &handle)
{
    if (!handle.valid)
        return false;
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(
        handle.win_handle,
        IOCTL_STORAGE_CHECK_VERIFY2,
        nullptr, 0, nullptr, 0,
        &bytes, nullptr);
    return ok == TRUE;
}
bool drives_has_media(const char *drive_path)
{
    char full_path[32] = {};
    snprintf(full_path, sizeof(full_path), "\\\\.\\%s", drive_path);
    HANDLE h = CreateFileA(full_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY2, nullptr, 0, nullptr, 0, &bytes, nullptr);
    CloseHandle(h);
    return ok == TRUE;
}
bool drives_inquiry(drive_handle &handle)
{
    if (!handle.valid)
        return false;
    BYTE buf[36] = {};
    sptd_packet pkt;
    sptd_init(pkt, buf, sizeof(buf), SCSI_IOCTL_DATA_IN);
    pkt.sptd.CdbLength = 6;
    pkt.sptd.Cdb[0] = 0x12;
    pkt.sptd.Cdb[1] = 0x00;
    pkt.sptd.Cdb[2] = 0x00;
    pkt.sptd.Cdb[3] = 0x00;
    pkt.sptd.Cdb[4] = sizeof(buf);
    pkt.sptd.Cdb[5] = 0x00;
    if (!sptd_send(handle.win_handle, pkt))
        return false;
    trim_scsi_str(handle.info.vendor, buf + 8, 8);
    trim_scsi_str(handle.info.product, buf + 16, 16);
    trim_scsi_str(handle.info.revision, buf + 32, 4);
    handle.info.type = guess_drive(handle.info.product);
    return true;
}
const char *drives_type_str(drive_type type)
{
    switch (type)
    {
    case drive_type::cdrom:
        return "CD-ROM / RW";
    case drive_type::dvdrom:
        return "DVD-ROM";
    case drive_type::dvdrw:
        return "DVD-RW / R";
    case drive_type::bdrom:
        return "BD-ROM";
    case drive_type::bdrw:
        return "BD-RE / R";
    default:
        return "unknown";
    }
}

// end