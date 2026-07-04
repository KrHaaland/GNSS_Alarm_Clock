// RtcRV3028.cpp — RV-3028-C7 on I2C 0x52. UTC in the calendar registers.
// Wire.begin() is done by main setup() before rtc_begin().
#include "RtcRV3028.h"
#include "Timezone.h"
#include <Wire.h>
#include <RV-3028-C7.h>

static RV3028 rtc;
static bool s_present = false;
static bool s_lostPower = false; // PORF latched at begin

static bool rtc_probe() {
  Wire.beginTransmission((uint8_t)RV3028_ADDR);
  return Wire.endTransmission() == 0;
}

// Raw register read; usable before rtc.begin() (which sets the lib's port).
static bool read_reg_raw(uint8_t reg, uint8_t &val) {
  Wire.beginTransmission((uint8_t)RV3028_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom((uint8_t)RV3028_ADDR, (size_t)1) != 1)
    return false;
  val = (uint8_t)Wire.read();
  return true;
}

bool rtc_begin() {
  s_present = rtc_probe();
  if (!s_present)
    return false;

  // rtc.begin() ends with STATUS := 0x00, wiping PORF — latch it first.
  uint8_t st;
  if (read_reg_raw(RV3028_STATUS, st))
    s_lostPower = (st & (1u << STATUS_PORF)) != 0;

  // 24h mode, trickle charger off, Level Switching backup Mode (EEPROM-backed
  // config; writes are skipped by the lib when values already match).
  bool ok = rtc.begin(Wire, true, true, true);
  return ok && s_present;
}

bool rtc_present() { return s_present; }

bool rtc_get_utc(time_t &utc) {
  if (!s_present)
    return false;
  if (!rtc.updateTime())
    return false;
  int y = rtc.getYear(); // lib returns 2000-based full year
  int mo = rtc.getMonth();
  int d = rtc.getDate();
  int h = rtc.getHours();
  int mi = rtc.getMinutes();
  int s = rtc.getSeconds();
  if (y < 2024 || y > 2098 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
      h > 23 || mi > 59 || s > 59)
    return false;
  utc = tm_to_epoch(y, mo, d, h, mi, s);
  return true;
}

bool rtc_set_utc(time_t utc) {
  if (!s_present)
    return false;
  int y, mo, d, h, mi, s, wd;
  epoch_to_tm(utc, y, mo, d, h, mi, s, wd);
  // Weekday register convention: 0 = Sunday (matches epoch_to_tm).
  return rtc.setTime((uint8_t)s, (uint8_t)mi, (uint8_t)h, (uint8_t)wd,
                     (uint8_t)d, (uint8_t)mo, (uint16_t)y);
}

bool rtc_lost_power() {
  bool r = s_lostPower;
  s_lostPower = false;
  // Also catch a PORF raised after begin (backup drained while running).
  if (s_present && rtc.readBit(RV3028_STATUS, STATUS_PORF)) {
    r = true;
    rtc.clearBit(RV3028_STATUS, STATUS_PORF);
  }
  return r;
}
