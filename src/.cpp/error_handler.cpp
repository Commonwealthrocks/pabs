// error_handler.cpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#include "error_handler.hpp"
#include "logger.hpp"
#include <cstdio>
#include <cstring>
static pabs_error_info last_error = {};
static bool initialized = false;
void err_init()
{
    memset(&last_error, 0, sizeof(last_error));
    last_error.code = pabs_error::ok;
    initialized = true;
}
void err_set(pabs_error code, sense_data sense, DWORD win_err, const char *message, const char *location)
{
    last_error.code = code;
    last_error.sense = sense;
    last_error.win_error = win_err;
    strncpy(last_error.message, message, sizeof(last_error.message) - 1);
    strncpy(last_error.location, location, sizeof(last_error.location) - 1);
    last_error.message[sizeof(last_error.message) - 1] = '\0';
    last_error.location[sizeof(last_error.location) - 1] = '\0';
}
pabs_error_info err_last()
{
    return last_error;
}
void err_clear()
{
    memset(&last_error, 0, sizeof(last_error));
    last_error.code = pabs_error::ok;
}
bool err_is_fatal()
{
    return (int32_t)last_error.code < 0;
}
bool err_is_recoverable()
{
    return (int32_t)last_error.code > 0;
}
const char *err_code_str(pabs_error code) // m; I'll write the return values here, you work on the sense keys
                                          // >> or whatever the fuck you said.
                                          // c; ok
{
    switch (code)
    {
    case pabs_error::ok:
        return "ok";
    case pabs_error::no_disc:
        return "no_disc";
    case pabs_error::disc_not_ready:
        return "disc_not_ready";
    case pabs_error::write_protected:
        return "write_protected";
    case pabs_error::buffer_underrun:
        return "buffer_underrun";
    case pabs_error::medium_error:
        return "medium_error";
    case pabs_error::invalid_handle:
        return "invalid_handle";
    case pabs_error::drive_open_failed:
        return "drive_open_failed";
    case pabs_error::ioctl_failed:
        return "ioctl_failed";
    case pabs_error::command_failed:
        return "command_failed";
    case pabs_error::unsupported_drive:
        return "unsupported_drive";
    case pabs_error::unsupported_media:
        return "unsupported_media";
    case pabs_error::burn_failed:
        return "burn_failed";
    case pabs_error::invalid_argument:
        return "invalid_argument";
    case pabs_error::out_of_memory:
        return "out_of_memory";
    case pabs_error::internal_error:
        return "internal_error";
    default:
        return "???";
    }
}
const char *err_sense_key_str(uint8_t sense_key) // c; increment of 1; won't hurt as much
{
    switch (sense_key & 0x0F)
    {
    case 0x0:
        return "no sense";
    case 0x1:
        return "recovered error";
    case 0x2:
        return "not ready";
    case 0x3:
        return "medium error";
    case 0x4:
        return "hardware error";
    case 0x5:
        return "illegal request";
    case 0x6:
        return "unit attention";
    case 0x7:
        return "data protect";
    case 0x8:
        return "blank check";
    case 0x9:
        return "vendor specific";
    case 0xA:
        return "copy aborted";
    case 0xB:
        return "aborted command";
    case 0xC:
        return "equal";
    case 0xD:
        return "volume overflow";
    case 0xE:
        return "miscompare";
    default:
        return "reserved";
    }
}
const char *err_asc_str(uint8_t asc, uint8_t ascq)
{
    uint16_t key = ((uint16_t)asc << 8) | ascq;
    switch (key)
    {
    case 0x0400:
        return "logical unit not ready, cause not reportable";
    case 0x0401:
        return "logical unit is in process of becoming ready";
    case 0x0402:
        return "logical unit not ready, initializing command required";
    case 0x0403:
        return "logical unit not ready, manual intervention required";
    case 0x0408:
        return "logical unit not ready, long write in progress";
    case 0x0409:
        return "logical unit not ready, self test in progress";
    case 0x1700:
        return "recovered data with no error correction applied";
    case 0x1800:
        return "recovered data with error correction applied";
        // c; can you fucking help out a bit?
        // m; I'm going to be honest I got no idea where to continue from your work...
        // c; of course...
        // c; https://www.t10.org/lists/asc-alph.txt, make work
    case 0x3000:
        return "incompatible medium installed";
    case 0x3001:
        return "cannot read medium; unknown format";
    case 0x3002:
        return "cannot read medium; incompatible format";
    case 0x3004:
        return "cannot write medium; incompatible format";
    case 0x3005:
        return "cannot write medium; write protected";
    case 0x3006:
        return "cannot format medium; incompatible medium";
    case 0x3100:
        return "medium format corrupted";
    case 0x3200:
        return "no defect spare location available";
    case 0x0800:
        return "logical unit communication failure";
    case 0x0900:
        return "track following error";
    case 0x0C00:
        return "write error";
    case 0x0C01:
        return "write error; recovered with auto reallocation";
    case 0x0C02:
        return "write error; no auto reallocation";
    case 0x0C03:
        return "write error; recommend reassignment";
    case 0x0C07:
        return "write error; recovery needed";
    case 0x0C08:
        return "write error; recovery failed";
    case 0x0C09:
        return "write error; loss of streaming (buffer underrun)";
    case 0x0C0A:
        return "write error; padding blocks added";
    case 0x2800:
        return "not ready to ready change, medium may have changed";
    case 0x2900:
        return "power on, reset, or bus device reset occurred";
    case 0x3A00:
        return "medium not present";
    case 0x3A01:
        return "medium not present; tray closed";
    case 0x3A02:
        return "medium not present; tray open";
    case 0x3A03:
        return "medium not present; loadable";
    case 0x3A04:
        return "medium not present; medium auxiliary memory accessible";
    case 0x2000:
        return "invalid command operation code";
    case 0x2100:
        return "logical block address out of range";
    case 0x2400:
        return "invalid field in CDB";
    case 0x2600:
        return "invalid field in parameter list";
    case 0x2C00:
        return "command sequence error";
    case 0x5300:
        return "media load or eject failed";
    case 0x5302:
        return "medium removal prevented";
    case 0x0E00:
        return "logical unit communication time[out]";
    case 0x5100:
        return "erase failure";
    case 0x5200:
        return "cartridge fault";
    case 0x7200:
        return "session fixation error";
    case 0x7201:
        return "session fixation error writing lead in";
    case 0x7202:
        return "session fixation error writing lead out";
    case 0x7300:
        return "CD control error";
    case 0x7302:
        return "power calibration area almost full";
    case 0x7303:
        return "power calibration area full";
    case 0x7304:
        return "program memory area update failure";
    case 0x7305:
        return "program memory area is full";
    default:
        break;
    }
    thread_local char fallback[64];
    snprintf(fallback, sizeof(fallback), "Unknown (ASC=0x%02X ASCQ=0x%02X)", asc, ascq);
    return fallback;
}
sense_data err_parse_sense(const uint8_t *buf)
{
    sense_data s = {};
    if (!buf)
        return s;
    uint8_t response_code = buf[0] & 0x7F;
    if (response_code != 0x70 && response_code != 0x71)
        return s;
    s.response_code = response_code;
    s.sense_key = buf[2] & 0x0F;
    s.asc = buf[12];
    s.ascq = buf[13];
    s.valid = true;
    return s;
}
void err_dump()
{
    pabs_error_info &e = last_error;
    log_writef(log_level::error, "err_dump", ">> ERRORS");
    log_writef(log_level::error, "err_dump", "  code     : %s (%d)", err_code_str(e.code), (int32_t)e.code);
    log_writef(log_level::error, "err_dump", "  message  : %s", e.message);
    log_writef(log_level::error, "err_dump", "  location : %s", e.location);
    if (e.win_error != 0)
    {
        char win_msg[256] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e.win_error, 0, win_msg, sizeof(win_msg), nullptr);
        size_t len = strlen(win_msg);
        if (len > 0 && win_msg[len - 1] == '\n')
            win_msg[len - 1] = '\0';
        log_writef(log_level::error, "err_dump", "  win32    : 0x%08lX - %s", e.win_error, win_msg);
    }
    if (e.sense.valid)
    {
        log_writef(log_level::error, "err_dump", "  sense    : key=0x%02X (%s)",
                e.sense.sense_key,
                err_sense_key_str(e.sense.sense_key));
        log_writef(log_level::error, "err_dump", "  asc/ascq : 0x%02X/0x%02X — %s",
                e.sense.asc, e.sense.ascq,
                err_asc_str(e.sense.asc, e.sense.ascq));
    }
    log_writef(log_level::error, "err_dump", "---------------------");
}

// end