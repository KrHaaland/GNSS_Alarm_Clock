// ClockKeeper.cpp — millis()-anchored UTC; GNSS disciplines the RTC and,
// with tzAuto, re-derives the POSIX timezone from coordinates.
#include "ClockKeeper.h"
#include "Gnss.h"
#include "RtcRV3028.h"
#include "Settings.h"
#include "Timezone.h"
#include <math.h>
#include <string.h>

static time_t s_anchorUtc = 0;
static uint32_t s_anchorMs = 0;
static bool s_valid = false;
static TimeSource s_source = TimeSource::None;

static bool s_everSynced = false;
static uint32_t s_lastGnssSyncMs = 0;
static bool s_syncRequested = false;

static uint32_t s_lastTaskMs = 0;
static bool s_everTzLookup = false;
static uint32_t s_lastTzLookupMs = 0;
static bool s_everTzSave = false;
static uint32_t s_lastTzSaveMs = 0;

static const uint32_t REANCHOR_MS = 3600000ul;  // keep far from 49-day wrap
static const uint32_t SYNC_INTERVAL_MS = 3600000ul;
static const uint32_t TZ_LOOKUP_INTERVAL_MS = 600000ul;
static const uint32_t TZ_SAVE_MIN_INTERVAL_MS = 3600000ul; // flash wear

void clock_begin() {
  time_t t;
  if (rtc_get_utc(t)) {
    s_anchorUtc = t;
    s_anchorMs = millis();
    s_valid = true;
    s_source = TimeSource::Rtc;
  }
}

bool clock_valid() { return s_valid; }

time_t clock_now_utc() {
  if (!s_valid)
    return 0;
  return s_anchorUtc + (time_t)((uint32_t)(millis() - s_anchorMs) / 1000ul);
}

// Slide the anchor forward by whole seconds so millis deltas stay small.
static void reanchor() {
  if (!s_valid)
    return;
  uint32_t delta = millis() - s_anchorMs;
  if (delta > REANCHOR_MS) {
    uint32_t secs = delta / 1000ul;
    s_anchorUtc += (time_t)secs;
    s_anchorMs += secs * 1000ul;
  }
}

static void gnss_sync_check(uint32_t nowMs) {
  time_t gnssEpoch;
  uint32_t ageMs;
  if (!gnss_get_utc(gnssEpoch, ageMs) || ageMs >= 1500ul)
    return;

  time_t gnssNow = gnssEpoch + (time_t)(ageMs / 1000ul);
  bool doSync = !s_everSynced || s_syncRequested;
  if (!doSync) {
    int32_t diff = (int32_t)(gnssNow - clock_now_utc());
    if (diff >= 2 || diff <= -2)
      doSync = true;
    else if ((uint32_t)(nowMs - s_lastGnssSyncMs) > SYNC_INTERVAL_MS)
      doSync = true;
  }
  if (!doSync)
    return;

  s_anchorUtc = gnssEpoch;
  s_anchorMs = millis() - ageMs; // anchor at the whole-second instant
  s_valid = true;
  s_source = TimeSource::Gnss;
  s_everSynced = true;
  s_lastGnssSyncMs = nowMs;
  s_syncRequested = false;
  rtc_set_utc(gnssNow);
}

static void tz_auto_check(uint32_t nowMs) {
  Settings &s = settings();
  if (!s.tzAuto || !gnss_has_fix())
    return;
  if (s_everTzLookup &&
      (uint32_t)(nowMs - s_lastTzLookupMs) < TZ_LOOKUP_INTERVAL_MS)
    return;

  float lat, lon;
  if (!gnss_get_position(lat, lon))
    return;
  s_everTzLookup = true;
  s_lastTzLookupMs = nowMs;

  TzResult r;
  tz_lookup(lat, lon, r);

  bool moved = !s.havePosition || fabsf(lat - s.lastLat) > 0.05f ||
               fabsf(lon - s.lastLon) > 0.05f;

  // Compare the name too: neighbouring countries often share identical rules
  // (Oslo/Stockholm are both CET), and the polygon lookup distinguishes them —
  // comparing only the POSIX string would leave the old name on the display.
  if (strcmp(r.posix, s.tzPosix) != 0 || strcmp(r.name, s.tzName) != 0) {
    strncpy(s.tzPosix, r.posix, TZ_POSIX_LEN - 1);
    s.tzPosix[TZ_POSIX_LEN - 1] = '\0';
    strncpy(s.tzName, r.name, TZ_NAME_LEN - 1);
    s.tzName[TZ_NAME_LEN - 1] = '\0';
    s.lastLat = lat;
    s.lastLon = lon;
    s.havePosition = true;
    settings_save(); // zone change always persists
    s_everTzSave = true;
    s_lastTzSaveMs = nowMs;
  } else if (moved) {
    s.lastLat = lat;
    s.lastLon = lon;
    s.havePosition = true;
    // Position-only updates persist at most once per hour.
    if (!s_everTzSave ||
        (uint32_t)(nowMs - s_lastTzSaveMs) >= TZ_SAVE_MIN_INTERVAL_MS) {
      settings_save();
      s_everTzSave = true;
      s_lastTzSaveMs = nowMs;
    }
  }
}

void clock_task() {
  uint32_t nowMs = millis();
  if ((uint32_t)(nowMs - s_lastTaskMs) < 1000ul)
    return;
  s_lastTaskMs = nowMs;

  reanchor();
  gnss_sync_check(nowMs);
  tz_auto_check(nowMs);
}

time_t clock_now_local() {
  time_t now = clock_now_utc();
  if (!s_valid)
    return 0;
  return now + (time_t)tz_offset_at(settings().tzPosix, now);
}

int32_t clock_tz_offset() {
  return tz_offset_at(settings().tzPosix, clock_now_utc());
}

bool clock_is_dst() {
  bool dst = false;
  tz_offset_at(settings().tzPosix, clock_now_utc(), &dst);
  return dst;
}

TimeSource clock_source() { return s_source; }

uint32_t clock_last_gnss_sync_age_s() {
  if (!s_everSynced)
    return UINT32_MAX;
  return (uint32_t)(millis() - s_lastGnssSyncMs) / 1000ul;
}

void clock_request_sync() { s_syncRequested = true; }
