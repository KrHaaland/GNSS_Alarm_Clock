// Settings.h — persistent device settings, packed into the RV-3028 RTC's
// 43-byte user EEPROM (see Settings.cpp for the packed format). Survives
// reboot, power loss AND firmware reflashes; ~100k writes/byte endurance.
// This RAM struct keeps the friendly layout the rest of the firmware uses.
#pragma once
#include <Arduino.h>

#define SETTINGS_MAGIC 0x474E4143u // "GNAC" (RAM-struct identity only)
#define SETTINGS_VERSION 3

// Main-screen mode: what the home screen shows / how inputs behave. The alarm
// engine keeps running regardless of mode. MODE_GAME turns the LIS3DH into a
// USB HID gamepad (tilt = stick, B2..B4 = buttons; B1 keeps its menu role).
enum : uint8_t {
  MODE_CLOCK = 0, MODE_SPEED = 1, MODE_ALT = 2, MODE_GAME = 3, MODE_COUNT = 4
};
#define NUM_ALARMS 2
#define TUNE_NAME_LEN 32
#define TZ_POSIX_LEN 48
#define TZ_NAME_LEN 20

struct AlarmConfig {
  bool enabled;
  uint8_t hour;     // 0..23, local time
  uint8_t minute;   // 0..59
  uint8_t daysMask; // bit0=Sun .. bit6=Sat; 0x7F = every day
  char tune[TUNE_NAME_LEN]; // WAV filename in flash, or "" = builtin melody
  uint8_t melodyId;         // builtin melody when tune[0] == 0
  uint8_t rampSeconds;      // gentle-wake fade-in 0/15/30/60 s (0 = off)
  uint8_t jitterMinutes;    // random trigger offset 0/1/5/9 min (0 = exact)
};

struct Settings {
  uint32_t magic;
  uint16_t version;

  // Timezone. posix e.g. "CET-1CEST,M3.5.0,M10.5.0/3". When tzAuto is set the
  // zone is (re)derived from GNSS coordinates and saved here so the clock
  // shows correct local time after reboot even before a new fix (indoors).
  char tzPosix[TZ_POSIX_LEN];
  char tzName[TZ_NAME_LEN];
  bool tzAuto;

  // Last known position (for tz lookup diagnostics / faster re-lookup)
  float lastLat;
  float lastLon;
  bool havePosition;

  AlarmConfig alarms[NUM_ALARMS];

  uint8_t volume;         // 0..10 master alarm volume
  uint8_t snoozeMinutes;  // default 9
  uint8_t buzzerAfterMin; // escalate to power buzzer after N min ringing, 0=off
  bool tapSnooze;         // LIS3DH tap = snooze while ringing

  bool use24h;
  bool starryNight;        // stars behind the clock between 22:00 and 06:00
  uint8_t mode;            // main-screen mode (MODE_CLOCK/SPEED/ALT)
  uint8_t brightness;      // display contrast 0..255
  uint16_t dimTimeoutS;    // dim display after N s idle, 0 = never
  uint8_t dimBrightness;   // contrast when dimmed

  // Snooze shame counter (appended in v3 — settings_begin() migrates old
  // blocks in place, so nothing else is reset).
  uint32_t snoozeTotal;     // all-time snooze count (stored as u24, caps 16.7M)
  uint16_t snoozeWeek;      // snoozes in the week starting snoozeWeekStart
  uint32_t snoozeWeekStart; // local epoch-day of that week's Monday
};

// Loads settings from flash; installs sane defaults (and saves them) when the
// stored block is missing or from a different version.
void settings_begin();
Settings &settings();      // live, mutable settings instance
void settings_save();      // write-through to flash (call after mutation)
void settings_defaults();  // reset the live instance to defaults (not saved)
