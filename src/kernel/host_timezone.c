/*
 * host_timezone.c -- the host's time zone, for the EEPROM user settings.
 *
 * Kept apart from kernel_xbox.c because it needs <windows.h>, whose types
 * clash with the Xbox ones that file uses. The fields are the Xbox EEPROM
 * user section's (XBOX_USER_SETTINGS): biases in minutes with the Windows
 * sign (UTC = local + bias), dates as month, week of the month (5 = last),
 * day of the week, hour -- the same encoding as TIME_ZONE_INFORMATION, so
 * the host's values carry over unchanged.
 */
#include <stdint.h>
#include <string.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

void xbox_host_timezone(int32_t *bias, char std_name[4], char dlt_name[4],
                        uint8_t std_date[4], uint8_t dlt_date[4],
                        int32_t *std_bias, int32_t *dlt_bias, int *use_dst)
{
    /* Central European Time: the default, and what a host without the API
     * gets. */
    int32_t b = -60, sb = 0, db = -60;
    uint8_t sd[4] = { 10, 5, 0, 3 }, dd[4] = { 3, 5, 0, 2 };
    int dst = 1;
#if defined(_WIN32)
    TIME_ZONE_INFORMATION tz;
    DWORD r = GetTimeZoneInformation(&tz);
    if (r != TIME_ZONE_ID_INVALID) {
        b = (int32_t)tz.Bias;
        sb = (int32_t)tz.StandardBias;
        db = (int32_t)tz.DaylightBias;
        dst = tz.StandardDate.wMonth != 0 && tz.DaylightDate.wMonth != 0;
        /* A year-specific date (wYear != 0) has no EEPROM form; keep the
         * zone's bias and drop daylight time rather than guess. */
        if (tz.StandardDate.wYear || tz.DaylightDate.wYear)
            dst = 0;
        if (dst) {
            sd[0] = (uint8_t)tz.StandardDate.wMonth; sd[1] = (uint8_t)tz.StandardDate.wDay;
            sd[2] = (uint8_t)tz.StandardDate.wDayOfWeek; sd[3] = (uint8_t)tz.StandardDate.wHour;
            dd[0] = (uint8_t)tz.DaylightDate.wMonth; dd[1] = (uint8_t)tz.DaylightDate.wDay;
            dd[2] = (uint8_t)tz.DaylightDate.wDayOfWeek; dd[3] = (uint8_t)tz.DaylightDate.wHour;
        }
    }
#endif
    if (!dst) {
        memset(sd, 0, 4);
        memset(dd, 0, 4);
        db = 0;
    }
    *bias = b; *std_bias = sb; *dlt_bias = db; *use_dst = dst;
    memcpy(std_date, sd, 4);
    memcpy(dlt_date, dd, 4);
    /* Four-character names, as the dashboard shows them. Windows' own names
     * are long ("Central Europe Standard Time"), so CET's are spelled out and
     * other zones get neutral ones. */
    if (b == -60 && dst) {
        memcpy(std_name, "CET\0", 4);
        memcpy(dlt_name, "CEST", 4);
    } else {
        memcpy(std_name, "STD\0", 4);
        memcpy(dlt_name, dst ? "DST\0" : "STD\0", 4);
    }
}
