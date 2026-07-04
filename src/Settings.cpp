// Settings.cpp — persistent settings via FlashStorage_SAMD emulated EEPROM.
#include "Settings.h"
#include <string.h>

// FlashStorage_SAMD.h contains implementations and may be included in exactly
// one translation unit. Size/debug must be defined before the include.
#define FLASH_DEBUG 0
#define EEPROM_EMULATION_SIZE 4096
#include <FlashStorage_SAMD.h>

static Settings s;
static Settings lastSaved;
static bool haveLastSaved = false;

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
  s.snoozeMinutes = 9;
  s.buzzerAfterMin = 5;
  s.tapSnooze = true;

  s.use24h = true;
  s.brightness = 0x90;
  s.dimTimeoutS = 30;
  s.dimBrightness = 0x10;
}

void settings_begin() {
  EEPROM.setCommitASAP(false); // batch writes; commit explicitly in save()
  EEPROM.get(0, s);
  if (s.magic != SETTINGS_MAGIC || s.version != SETTINGS_VERSION) {
    settings_defaults();
    settings_save();
  } else {
    memcpy(&lastSaved, &s, sizeof(Settings));
    haveLastSaved = true;
  }
}

Settings &settings() { return s; }

void settings_save() {
  if (haveLastSaved && memcmp(&lastSaved, &s, sizeof(Settings)) == 0)
    return; // avoid needless flash erase cycles
  EEPROM.put(0, s);
  EEPROM.commit();
  memcpy(&lastSaved, &s, sizeof(Settings));
  haveLastSaved = true;
}
