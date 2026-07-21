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
#ifndef GNSS_SIM
  // RMC+GGA at 1 Hz, GSV (satellites in view, for the sky screen) every 5th
  // fix — full GSV bursts every second would crowd the 9600-baud link.
  Serial1.print("$PMTK314,0,1,0,1,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0*2D\r\n");
  Serial1.print("$PMTK220,1000*1F\r\n");
#endif
}

// --- GSV (satellites in view) ---------------------------------------------
// Small standalone NMEA line parser fed the same characters as TinyGPS++.
// $GPGSV (GPS) and $GLGSV (GLONASS) groups arrive as 1..4 sentences with up
// to 4 satellites each; a group is staged and committed atomically on its
// last sentence so gnss_get_sats() never sees a half-updated constellation.
#define GSV_MAX_PER_SYS 16
static GnssSatInfo s_gsv[2][GSV_MAX_PER_SYS]; // [0]=GPS, [1]=GLONASS
static uint8_t s_gsvCount[2];
static GnssSatInfo s_gsvStage[2][GSV_MAX_PER_SYS];
static uint8_t s_gsvStageN[2];
static uint32_t s_gsvMs; // millis() of last committed group

static char s_nmeaLine[96];
static uint8_t s_nmeaLen;

static int gsv_field_int(const char *&p) { // parses up to next ',' or '*'
  int v = 0;
  bool any = false;
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p++ - '0');
    any = true;
  }
  if (*p == ',')
    p++;
  return any ? v : -1; // empty field -> -1
}

static void gsv_parse_line(const char *l, uint8_t len) {
  // "$GPGSV," / "$GLGSV," + verified checksum required.
  if (len < 10 || l[0] != '$' || l[3] != 'G' || l[4] != 'S' || l[5] != 'V')
    return;
  uint8_t sys;
  if (l[1] == 'G' && l[2] == 'P')
    sys = 0;
  else if (l[1] == 'G' && l[2] == 'L')
    sys = 1;
  else
    return;
  uint8_t sum = 0, i = 1;
  for (; i < len && l[i] != '*'; i++)
    sum ^= (uint8_t)l[i];
  if (i + 2 >= len)
    return;
  char hex[] = "0123456789ABCDEF";
  if (l[i + 1] != hex[sum >> 4] || l[i + 2] != hex[sum & 0xF])
    return;

  const char *p = l + 7; // past "$GPGSV,"
  int total = gsv_field_int(p);
  int msg = gsv_field_int(p);
  gsv_field_int(p); // numSV (unused; we count what we store)
  if (total < 1 || msg < 1 || msg > total)
    return;
  if (msg == 1)
    s_gsvStageN[sys] = 0;

  for (uint8_t s = 0; s < 4 && *p && *p != '*'; s++) {
    int prn = gsv_field_int(p);
    int elev = gsv_field_int(p);
    int azim = gsv_field_int(p);
    int snr = gsv_field_int(p); // empty = in view, not tracked
    if (prn <= 0 || s_gsvStageN[sys] >= GSV_MAX_PER_SYS)
      continue;
    GnssSatInfo &o = s_gsvStage[sys][s_gsvStageN[sys]++];
    o.prn = (uint8_t)prn;
    o.elevDeg = (uint8_t)((elev < 0) ? 0 : (elev > 90) ? 90 : elev);
    o.azimDeg = (uint16_t)((azim < 0) ? 0 : azim % 360);
    o.snrDb = (uint8_t)((snr < 0) ? 0 : (snr > 99) ? 99 : snr);
    o.system = sys ? 'R' : 'G';
  }

  if (msg == total) { // group complete: commit
    memcpy((void *)s_gsv[sys], s_gsvStage[sys],
           s_gsvStageN[sys] * sizeof(GnssSatInfo));
    s_gsvCount[sys] = s_gsvStageN[sys];
    s_gsvMs = millis();
  }
}

static void gsv_feed(char c) {
  if (c == '$')
    s_nmeaLen = 0;
  if (c == '\r' || c == '\n') {
    if (s_nmeaLen >= 6)
      gsv_parse_line(s_nmeaLine, s_nmeaLen);
    s_nmeaLen = 0;
    return;
  }
  if (s_nmeaLen < sizeof(s_nmeaLine) - 1)
    s_nmeaLine[s_nmeaLen++] = c;
}

uint8_t gnss_get_sats(GnssSatInfo *out, uint8_t maxN) {
  // GSV comes every ~5 s (every 5th fix); stale after 12 s means the burst
  // stream died (no antenna signal at all, or a sim build).
  if (s_gsvMs == 0 || (uint32_t)(millis() - s_gsvMs) > 12000ul)
    return 0;
  uint8_t n = 0;
  for (uint8_t sys = 0; sys < 2 && n < maxN; sys++)
    for (uint8_t i = 0; i < s_gsvCount[sys] && n < maxN; i++)
      out[n++] = s_gsv[sys][i];
  return n;
}

void gnss_task() {
  while (Serial1.available() > 0) {
    char c = (char)Serial1.read();
    gps.encode(c);
    gsv_feed(c);
  }

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

#ifdef GNSS_SIM
// Simulated GNSS: state is driven by the SimConsole, not the L86. gnss_task()
// still runs (Serial1 is idle), but the fix/pos/utc come from these setters.
static bool s_simFix = false;
static float s_simKmph = 0.0f, s_simAltM = 0.0f;
void gnss_sim_set_fix(float lat, float lon) {
  s_lat = lat;
  s_lon = lon;
  s_havePos = true;
  s_simFix = true;
}
void gnss_sim_clear_fix() { s_simFix = false; }
void gnss_sim_set_utc(time_t utc) {
  s_utc = utc;
  s_utcMs = millis(); // fresh -> ClockKeeper anchors on the next 1 Hz task
  s_haveUtc = true;
}
void gnss_sim_set_motion(float kmph, float altM) {
  s_simKmph = kmph;
  s_simAltM = altM;
}
bool gnss_has_fix() { return s_simFix; }
#else
bool gnss_has_fix() {
  return gps.location.isValid() && gps.location.age() < 10000ul;
}
#endif

bool gnss_get_position(float &lat, float &lon) {
  if (!s_havePos)
    return false;
  lat = s_lat;
  lon = s_lon;
  return true;
}

#ifdef GNSS_SIM
bool gnss_get_speed_kmph(float &kmph) {
  if (!s_simFix)
    return false;
  kmph = s_simKmph;
  return true;
}

bool gnss_get_altitude_m(float &meters) {
  if (!s_simFix)
    return false;
  meters = s_simAltM;
  return true;
}
#else
bool gnss_get_speed_kmph(float &kmph) {
  if (!gnss_has_fix() || !gps.speed.isValid())
    return false;
  kmph = (float)gps.speed.kmph();
  return true;
}

bool gnss_get_altitude_m(float &meters) {
  if (!gnss_has_fix() || !gps.altitude.isValid())
    return false;
  meters = (float)gps.altitude.meters();
  return true;
}
#endif

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
