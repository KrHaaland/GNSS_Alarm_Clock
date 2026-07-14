// Settings.cpp — persistent settings packed into the RV-3028's 43-byte user
// EEPROM. Chosen over the old internal-flash emulation because the RTC EEPROM
// survives firmware reflashes (the flash area lived inside the app image and
// was wiped on every update), takes ~100k writes/byte (4x internal flash),
// and lets the 24LC512 be dropped from the hardware-v2 BOM.
//
// The RAM struct keeps its friendly layout; pack()/unpack() translate to a
// 39-byte image. Space is won by not storing derived data:
//   - tz strings (68 B) -> lat/lon in centidegrees + manual-zone index; the
//     POSIX string is re-derived at boot via tz_lookup()/TZ_TABLE.
//   - alarm tune filename (32 B) -> 16-bit FNV-1a hash, re-matched against
//     the TUNES directory at boot (missing file -> builtin melody fallback).
// Writes go byte-by-byte (~16 ms each) and only for bytes that changed; the
// checksum byte is written LAST so a torn write invalidates the block
// (-> defaults on next boot) instead of going unnoticed.
#include "Settings.h"

#include <string.h>

#include "RtcRV3028.h"
#include "Timezone.h"
#include "TuneStorage.h"
#include "TzTable.h"

// --- packed image layout -----------------------------------------------------
#define PACK_MAGIC0 'G'
#define PACK_MAGIC1 'C'
#define PACK_VERSION 2 // v2: + rampSeconds (v1 blocks migrate in place)
#define PACK_LEN 40           // incl. trailing checksum; <= RTC_USER_EEPROM_SIZE
#define EPOCH2020_DAYS 18262u // 2020-01-01 in unix epoch-days (weekStart base)

enum : uint8_t {
  OFF_MAGIC0 = 0, OFF_MAGIC1, OFF_VERSION, OFF_FLAGS,
  OFF_LAT = 4,    // i16 centideg, LE
  OFF_LON = 6,    // i16 centideg
  OFF_ZONEIDX = 8, // manual TZ_TABLE index, 0xFF = none
  OFF_VOLUME, OFF_SNOOZEMIN, OFF_BUZZAFTER, OFF_BRIGHT,
  OFF_DIMTIMEOUT = 13, // u16 seconds
  OFF_DIMBRIGHT = 15,
  OFF_SNZTOTAL = 16,   // u32
  OFF_SNZWEEK = 20,    // u16
  OFF_SNZWEEKSTART = 22, // u16, local epoch-day - EPOCH2020_DAYS
  OFF_ALARM0 = 24,     // 7 B each: flags,hour,min,mask,melody,hash(u16)
  OFF_ALARM1 = 31,
  OFF_RAMP = 38,       // gentle-wake ramp seconds (added in v2)
  OFF_CHECKSUM = 39,
};
#define V1_LEN 39 // v1 image: no ramp byte, checksum at offset 38
// flags byte: b0 tzAuto, b1 tapSnooze, b2 use24h, b3 havePosition, b4-5 mode
// alarm flags byte: b0 enabled

static Settings s;
static uint8_t lastPacked[PACK_LEN];
static bool haveLast = false;

// 16-bit FNV-1a of a filename (folded 32-bit); 0 is reserved for "no tune".
static uint16_t tune_hash(const char *name) {
  if (!name || !name[0])
    return 0;
  uint32_t h = 2166136261u;
  for (const char *p = name; *p; p++) {
    h ^= (uint8_t)*p;
    h *= 16777619u;
  }
  uint16_t f = (uint16_t)((h >> 16) ^ h);
  return f ? f : 1;
}

static void put16(uint8_t *b, uint16_t v) { b[0] = v & 0xFF; b[1] = v >> 8; }
static void put32(uint8_t *b, uint32_t v) {
  b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF;
  b[2] = (v >> 16) & 0xFF; b[3] = v >> 24;
}
static uint16_t get16(const uint8_t *b) {
  return (uint16_t)(b[0] | (b[1] << 8));
}
static uint32_t get32(const uint8_t *b) {
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
}

static int16_t centideg(float v) {
  float c = v * 100.0f;
  if (c > 32767.0f) c = 32767.0f;
  if (c < -32768.0f) c = -32768.0f;
  return (int16_t)(c >= 0 ? c + 0.5f : c - 0.5f);
}

static uint8_t checksum(const uint8_t *b, uint8_t csOff) {
  uint8_t sum = 0;
  for (uint8_t i = 0; i < csOff; i++)
    sum = (uint8_t)(sum + b[i]);
  return (uint8_t)(0xFF - sum); // so full-block sum == 0xFF when intact
}

static void pack(const Settings &in, uint8_t out[PACK_LEN]) {
  memset(out, 0, PACK_LEN);
  out[OFF_MAGIC0] = PACK_MAGIC0;
  out[OFF_MAGIC1] = PACK_MAGIC1;
  out[OFF_VERSION] = PACK_VERSION;
  out[OFF_FLAGS] = (in.tzAuto ? 1 : 0) | (in.tapSnooze ? 2 : 0) |
                   (in.use24h ? 4 : 0) | (in.havePosition ? 8 : 0) |
                   ((in.mode & 3) << 4);
  put16(&out[OFF_LAT], (uint16_t)centideg(in.lastLat));
  put16(&out[OFF_LON], (uint16_t)centideg(in.lastLon));
  int zi = tztable_index_of_posix(in.tzPosix);
  out[OFF_ZONEIDX] = (zi >= 0 && zi < 0xFF) ? (uint8_t)zi : 0xFF;
  out[OFF_VOLUME] = in.volume;
  out[OFF_SNOOZEMIN] = in.snoozeMinutes;
  out[OFF_BUZZAFTER] = in.buzzerAfterMin;
  out[OFF_BRIGHT] = in.brightness;
  put16(&out[OFF_DIMTIMEOUT], in.dimTimeoutS);
  out[OFF_DIMBRIGHT] = in.dimBrightness;
  put32(&out[OFF_SNZTOTAL], in.snoozeTotal);
  put16(&out[OFF_SNZWEEK], in.snoozeWeek);
  uint32_t ws = in.snoozeWeekStart;
  ws = (ws > EPOCH2020_DAYS) ? ws - EPOCH2020_DAYS : 0;
  put16(&out[OFF_SNZWEEKSTART], (uint16_t)(ws > 0xFFFF ? 0xFFFF : ws));
  for (uint8_t i = 0; i < NUM_ALARMS; i++) {
    uint8_t *a = &out[i == 0 ? OFF_ALARM0 : OFF_ALARM1];
    const AlarmConfig &ac = in.alarms[i];
    a[0] = ac.enabled ? 1 : 0;
    a[1] = ac.hour;
    a[2] = ac.minute;
    a[3] = ac.daysMask;
    a[4] = ac.melodyId;
    put16(&a[5], tune_hash(ac.tune));
  }
  out[OFF_RAMP] = in.rampSeconds;
  out[OFF_CHECKSUM] = checksum(out, OFF_CHECKSUM);
}

// Tune hashes seen at unpack; resolved against the TUNES directory once
// storage is up (settings_begin runs after storage_begin — see main.cpp).
static uint16_t s_pendingTuneHash[NUM_ALARMS];

static void resolve_tunes() {
  char names[12][32];
  uint8_t n = storage_list_tunes(names, 12);
  for (uint8_t i = 0; i < NUM_ALARMS; i++) {
    s.alarms[i].tune[0] = '\0';
    uint16_t want = s_pendingTuneHash[i];
    if (!want)
      continue;
    for (uint8_t k = 0; k < n; k++)
      if (tune_hash(names[k]) == want) {
        strncpy(s.alarms[i].tune, names[k], TUNE_NAME_LEN - 1);
        s.alarms[i].tune[TUNE_NAME_LEN - 1] = '\0';
        break;
      }
    // no match -> stays "", alarm falls back to the builtin melody
  }
}

static void unpack(const uint8_t in[PACK_LEN], Settings &out) {
  settings_defaults(); // sane base; overwrite with stored values
  uint8_t f = in[OFF_FLAGS];
  out.tzAuto = f & 1;
  out.tapSnooze = f & 2;
  out.use24h = f & 4;
  out.havePosition = f & 8;
  out.mode = (uint8_t)((f >> 4) & 3) % MODE_COUNT;
  out.lastLat = (int16_t)get16(&in[OFF_LAT]) / 100.0f;
  out.lastLon = (int16_t)get16(&in[OFF_LON]) / 100.0f;
  out.volume = in[OFF_VOLUME] <= 10 ? in[OFF_VOLUME] : 10;
  out.snoozeMinutes = in[OFF_SNOOZEMIN] ? in[OFF_SNOOZEMIN] : 9;
  out.buzzerAfterMin = in[OFF_BUZZAFTER];
  out.brightness = in[OFF_BRIGHT];
  out.dimTimeoutS = get16(&in[OFF_DIMTIMEOUT]);
  out.dimBrightness = in[OFF_DIMBRIGHT];
  out.snoozeTotal = get32(&in[OFF_SNZTOTAL]);
  out.snoozeWeek = get16(&in[OFF_SNZWEEK]);
  out.snoozeWeekStart = EPOCH2020_DAYS + get16(&in[OFF_SNZWEEKSTART]);
  out.rampSeconds = in[OFF_RAMP] <= 60 ? in[OFF_RAMP] : 30;
  for (uint8_t i = 0; i < NUM_ALARMS; i++) {
    const uint8_t *a = &in[i == 0 ? OFF_ALARM0 : OFF_ALARM1];
    AlarmConfig &ac = out.alarms[i];
    ac.enabled = a[0] & 1;
    ac.hour = a[1] <= 23 ? a[1] : 7;
    ac.minute = a[2] <= 59 ? a[2] : 0;
    ac.daysMask = a[3] & 0x7F;
    ac.melodyId = a[4];
    ac.tune[0] = '\0';
    s_pendingTuneHash[i] = get16(&a[5]);
  }
  // Re-derive the timezone strings the pack dropped: manual selection from
  // the GMT ladder, else from the stored position (identical result to what
  // was displayed when it was saved).
  uint8_t zi = in[OFF_ZONEIDX];
  if (!out.tzAuto && zi < TZ_COUNT) {
    strncpy(out.tzPosix, TZ_TABLE[zi].posix, TZ_POSIX_LEN - 1);
    out.tzPosix[TZ_POSIX_LEN - 1] = '\0';
    strncpy(out.tzName, TZ_TABLE[zi].name, TZ_NAME_LEN - 1);
    out.tzName[TZ_NAME_LEN - 1] = '\0';
  } else if (out.havePosition) {
    TzResult r;
    tz_lookup(out.lastLat, out.lastLon, r);
    strncpy(out.tzPosix, r.posix, TZ_POSIX_LEN - 1);
    out.tzPosix[TZ_POSIX_LEN - 1] = '\0';
    strncpy(out.tzName, r.name, TZ_NAME_LEN - 1);
    out.tzName[TZ_NAME_LEN - 1] = '\0';
  } // else: defaults (Europe/Oslo) until the first fix
}

void settings_defaults() {
  memset(&s, 0, sizeof(s)); // zero padding too, keeps memcmp guard meaningful
  s.magic = SETTINGS_MAGIC;
  s.version = SETTINGS_VERSION;

  strncpy(s.tzPosix, "CET-1CEST,M3.5.0,M10.5.0/3", TZ_POSIX_LEN - 1);
  strncpy(s.tzName, "Europe/Oslo", TZ_NAME_LEN - 1);
  s.tzAuto = true;

  s.lastLat = 0.0f;
  s.lastLon = 0.0f;
  s.havePosition = false;

  s.alarms[0].enabled = false;
  s.alarms[0].hour = 7;
  s.alarms[0].minute = 0;
  s.alarms[0].daysMask = 0x3E; // Mon..Fri
  s.alarms[0].tune[0] = '\0';
  s.alarms[0].melodyId = 0;

  s.alarms[1].enabled = false;
  s.alarms[1].hour = 9;
  s.alarms[1].minute = 0;
  s.alarms[1].daysMask = 0x41; // Sat+Sun
  s.alarms[1].tune[0] = '\0';
  s.alarms[1].melodyId = 2;

  s.volume = 7;
  s.rampSeconds = 30;
  s.snoozeMinutes = 9;
  s.buzzerAfterMin = 5;
  s.tapSnooze = true;

  s.use24h = true;
  s.mode = MODE_CLOCK;
  s.brightness = 0x90;
  s.dimTimeoutS = 30;
  s.dimBrightness = 0x10;

  s.snoozeTotal = 0;
  s.snoozeWeek = 0;
  s.snoozeWeekStart = 0;
}

void settings_begin() {
  settings_defaults();
  memset(s_pendingTuneHash, 0, sizeof(s_pendingTuneHash));

  uint8_t img[PACK_LEN];
  if (!rtc_eeprom_read(0, img, PACK_LEN)) {
    // RTC absent/unreachable: run on defaults, RAM-only (no persistence).
    haveLast = false;
    return;
  }
  bool magicOk = img[OFF_MAGIC0] == PACK_MAGIC0 && img[OFF_MAGIC1] == PACK_MAGIC1;
  if (magicOk && img[OFF_VERSION] == PACK_VERSION &&
      img[OFF_CHECKSUM] == checksum(img, OFF_CHECKSUM)) {
    unpack(img, s);
    resolve_tunes();
    memcpy(lastPacked, img, PACK_LEN);
    haveLast = true;
  } else if (magicOk && img[OFF_VERSION] == 1 &&
             img[V1_LEN - 1] == checksum(img, V1_LEN - 1)) {
    // v1 -> v2: the ramp byte was appended before the checksum. Take the v1
    // fields as-is, default the ramp, persist as v2.
    img[OFF_RAMP] = 0xFF; // force default in unpack's range check
    unpack(img, s);
    s.rampSeconds = 30;
    resolve_tunes();
    haveLast = false;
    settings_save();
  } else {
    // Blank chip / torn write / unknown format: persist the defaults.
    haveLast = false;
    settings_save();
  }
}

Settings &settings() { return s; }

void settings_save() {
  uint8_t img[PACK_LEN];
  pack(s, img);
  if (haveLast && memcmp(lastPacked, img, PACK_LEN) == 0)
    return; // nothing changed
  bool ok = true;
  if (!haveLast) {
    ok = rtc_eeprom_write(0, img, PACK_LEN);
  } else {
    // Write only changed data bytes (16 ms each), checksum last so an
    // interrupted save is detected at next boot instead of read back wrong.
    for (uint8_t i = 0; i < OFF_CHECKSUM && ok; i++)
      if (img[i] != lastPacked[i])
        ok = rtc_eeprom_write(i, &img[i], 1);
    if (ok && img[OFF_CHECKSUM] != lastPacked[OFF_CHECKSUM])
      ok = rtc_eeprom_write(OFF_CHECKSUM, &img[OFF_CHECKSUM], 1);
  }
  if (ok) {
    memcpy(lastPacked, img, PACK_LEN);
    haveLast = true;
  } else {
    haveLast = false; // force a full rewrite on the next save
  }
}
