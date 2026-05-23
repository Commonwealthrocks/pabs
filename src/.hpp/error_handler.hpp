// error_handler.hpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
#include <windows.h>
#include <cstdint>
// m; 0 means that a functon was returned successfully.
// >> A positive value is deemed a "work-with-it" value while a negative one is a must halt in any operation.
enum class pabs_error : int32_t
{
    ok = 0,                 // c; ok
    no_disc = 1,            // c; recoverable
    disc_not_ready = 2,     // c; recoverable
    write_protected = 3,    // c; recoverable
    buffer_underrun = 4,    // c; recoverable
    medium_error = 5,       // c; recoverable
    invalid_handle = -1,    // c; fatal
    drive_open_failed = -2, // c; fatal
    ioctl_failed = -3,      // c; fatal
    command_failed = -4,    // c; fatal
    unsupported_drive = -5, // c; fatal
    unsupported_media = -6, // c; fatal
    burn_failed = -7,       // c; fatal
    invalid_argument = -8,  // c; fatal
    out_of_memory = -9,     // c; fatal
    internal_error = -10,   // c; fatal
};
struct sense_data
{
    uint8_t response_code;
    uint8_t sense_key;
    uint8_t asc;
    uint8_t ascq;
    bool valid;
};
struct pabs_error_info
{
    pabs_error code;
    sense_data sense;
    DWORD win_error;
    char message[128];
    char location[64];
};
void err_init();
void err_set(pabs_error code, sense_data sense, DWORD win_err, const char *message, const char *location);
pabs_error_info err_last();
void err_clear();
bool err_is_fatal();
bool err_is_recoverable();
const char *err_code_str(pabs_error code);
const char *err_sense_key_str(uint8_t sense_key);
const char *err_asc_str(uint8_t asc, uint8_t ascq); // m; How the fuck do you just know this?
void err_dump();                                    // c; mdlt came in clutch for most of this i won't lie...
#define PABS_LOC (__func__)
#define ERR_SET(code, msg) \
    err_set((code), {0, 0, 0, 0, false}, GetLastError(), (msg), PABS_LOC)
#define ERR_SET_SCSI(code, sense_buf, msg) \
    err_set((code), err_parse_sense(sense_buf), 0, (msg), PABS_LOC)
#define ERR_RETURN(code, msg)   \
    do                          \
    {                           \
        ERR_SET((code), (msg)); \
        return false;           \
    } while (0)
sense_data err_parse_sense(const uint8_t *sense_buf);

// end