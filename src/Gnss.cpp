// Gnss.cpp — Quectel L86-M33 on Serial1 (9600 NMEA), parsed with TinyGPS++.
#include "Gnss.h"
#include "pins.h"
#include "Timezone.h"
#include <TinyGPS++.h>

static TinyGPSPlus gps;

static time_t s_utc = 0;      // epoch of last decoded whole UTC second
static uint32_t s_utcMs = 0;  // millis() at that whole-second instant
static bool s_haveUtc = false;

static float s_lat = 0.0f, s_lon = 0.0f; // last valid fix
static bool s_havePos = false;

void gnss_begin() {
  // Both L86 control pins have module-internal pull-ups: leave hi-Z.
  portb_hiz(GNSS_RESET_PORTPIN);
  portb_hiz(GNSS_FON_PORTPIN);

  Serial1.begin(9600);
  // RMC+GGA only, 1 Hz. Checksums verified (XOR of payload).
  Serial1.print("$PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0*28\r\n");
  Serial1.print("$PMTK220,1000*1F\r\n");
}

void gnss_task() {
  while (Serial1.available() > 0)
    gps.encode((char)Serial1.read());

  // Require date.isUpdated() too so we only anchor on RMC commits (date and
  // time from the same sentence); a GGA just after midnight would otherwise
  // pair the new time with the previous day's date.
  //
  // CRITICAL: require an actual position fix. TinyGPS++ commits date/time for
  // any checksum-valid RMC even with status 'V' (no fix). On a cold start the
  // L86 emits its free-running clock with the GPS-epoch date, which TinyGPS++
  // maps to year 2080 (passes >= 2024). Without the fix gate that garbage
  // would be adopted and written into the supercap-backed RTC, persisting
  // across reboots. Only trust GNSS time when we truly have a fix.
  if (gps.time.isUpdated() && gps.date.isUpdated() && gps.time.isValid() &&
      gps.date.isValid() && gps.date.year() >= 2024 && gps.date.year() < 2100 &&
      gnss_has_fix()) {
    time_t t =
        tm_to_epoch(gps.date.year(), gps.date.month(), gps.date.day(),
                    gps.time.hour(), gps.time.minute(), gps.time.second());
    // Sentence carries hh:mm:ss.cc; the whole second was cc*10 ms ago.
    s_utcMs = millis() - (uint32_t)gps.time.centisecond() * 10u;
    s_utc = t;
    s_haveUtc = true;
  }

  if (gps.location.isValid() && gps.location.isUpdated()) {
    s_lat = (float)gps.location.lat();
    s_lon = (float)gps.location.lng();
    s_havePos = true;
  }
}

bool gnss_time_valid() {
  return s_haveUtc && (uint32_t)(millis() - s_utcMs) < 10000ul;
}

bool gnss_get_utc(time_t &utc, uint32_t &ageMs) {
  if (!s_haveUtc)
    return false;
  utc = s_utc;
  ageMs = (uint32_t)(millis() - s_utcMs);
  return true;
}

bool gnss_has_fix() {
  return gps.location.isValid() && gps.location.age() < 10000ul;
}

bool gnss_get_position(float &lat, float &lon) {
  if (!s_havePos)
    return false;
  lat = s_lat;
  lon = s_lon;
  return true;
}

uint8_t gnss_num_sats() {
  if (!gps.satellites.isValid())
    return 0;
  uint32_t n = gps.satellites.value();
  return (n > 255u) ? 255u : (uint8_t)n;
}

uint16_t gnss_hdop_x10() {
  if (!gps.hdop.isValid())
    return 0xFFFF;
  int32_t v = gps.hdop.value() / 10; // value() is HDOP*100
  if (v < 0 || v > 0xFFFE)
    return 0xFFFF;
  return (uint16_t)v;
}

uint32_t gnss_chars_seen() { return gps.charsProcessed(); }

void gnss_hw_reset() {
  portb_output(GNSS_RESET_PORTPIN, false); // RESET_N is active low
  delay(100);
  portb_hiz(GNSS_RESET_PORTPIN); // module's internal pull-up releases reset
  delay(10);
}
