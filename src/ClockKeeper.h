// ClockKeeper.h — central timekeeper.
//
// Sources, in order of trust: GNSS (when valid) > RTC (battery/supercap
// backed) > none. Keeps a millis()-anchored UTC epoch, disciplines the RTC
// from GNSS, derives local time via the active POSIX TZ (Settings), and
// re-derives the timezone from GNSS coordinates when tzAuto is enabled —
// persisting it to flash so local time is right after reboot indoors.
#pragma once
#include <Arduino.h>
#include <time.h>

enum class TimeSource : uint8_t { None, Rtc, Gnss };

void clock_begin(); // seed from RTC if plausible
void clock_task();  // GNSS sync + tz auto-update; call every loop

bool clock_valid();          // do we know the time at all?
time_t clock_now_utc();      // 0 if not valid
time_t clock_now_local();    // UTC + tz offset at now
int32_t clock_tz_offset();   // current offset in seconds (east positive)
bool clock_is_dst();
TimeSource clock_source();
uint32_t clock_last_gnss_sync_age_s(); // UINT32_MAX if never
// Force a GNSS->RTC sync at the next valid GNSS time (menu action).
void clock_request_sync();
