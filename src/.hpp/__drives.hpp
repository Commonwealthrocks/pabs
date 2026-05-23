// __drives.hpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
#include <windows.h>
#include <ntddscsi.h>
#include <string>
#include <vector>
#define PABS_MAX_DRIVES 16
#define PABS_SENSE_LEN 18
enum class drive_type
{
    unknown,
    cdrom,
    dvdrom,
    dvdrw,
    bdrom,
    bdrw,
};
struct drive_info
{
    char path[8];
    char vendor[9];
    char product[17];
    char revision[5];
    drive_type type;
    bool tray_open;
    bool disc_present;
    std::vector<int> supported_write_speeds; // c; some discs support 52x, i really hope the average person isn't retarded enough to use that
};
struct drive_handle
{
    HANDLE win_handle;
    drive_info info;
    bool valid;
};
struct sptd_packet
{
    SCSI_PASS_THROUGH_DIRECT sptd;
    BYTE sense[PABS_SENSE_LEN];
};
void sptd_init(sptd_packet &pkt, BYTE *data_buf, DWORD data_len, BYTE direction);
bool sptd_send(HANDLE h, sptd_packet &pkt);
int drives_enum(drive_info out_drives[PABS_MAX_DRIVES]);
drive_handle drives_open(const char *drive_path);
void drives_close(drive_handle &handle);
bool drives_test_ready(drive_handle &handle);
bool drives_has_media(const char *drive_path);
bool drives_inquiry(drive_handle &handle);
const char *drives_type_str(drive_type type);

// end