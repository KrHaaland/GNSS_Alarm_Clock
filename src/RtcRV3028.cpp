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

// Raw register write; usable before rtc.begin().
static bool write_reg_raw(uint8_t reg, uint8_t val) {
  Wire.beginTransmission((uint8_t)RV3028_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// Poll STATUS.EEBUSY clear; EEPROM byte-write takes ~16 ms worst case.
static bool eeprom_wait_ready() {
  uint32_t deadline = millis() + 100;
  uint8_t st;
  while ((int32_t)(deadline - millis()) > 0) {
    if (!read_reg_raw(RV3028_STATUS, st))
      return false;
    if (!(st & (1u << STATUS_EEBUSY)))
      return true;
  }
  return false;
}

// Set/clear CTRL1.EERD (auto-refresh must be off during user-EEPROM access).
static bool eeprom_set_eerd(bool on) {
  uint8_t c1;
  if (!read_reg_raw(RV3028_CTRL1, c1))
    return false;
  if (on)
    c1 |= (1u << CTRL1_EERD);
  else
    c1 &= ~(1u << CTRL1_EERD);
  return write_reg_raw(RV3028_CTRL1, c1);
}

// One user-EEPROM byte op: cmd = EEPROMCMD_ReadSingle / _WriteSingle.
static bool eeprom_byte_op(uint8_t cmd, uint8_t addr, uint8_t &data) {
  if (!eeprom_wait_ready())
    return false;
  if (!write_reg_raw(RV3028_EEPROM_ADDR, addr))
    return false;
  if (cmd == EEPROMCMD_WriteSingle && !write_reg_raw(RV3028_EEPROM_DATA, data))
    return false;
  // Command register: "first command" 0x00, then the actual op.
  if (!write_reg_raw(RV3028_EEPROM_CMD, EEPROMCMD_First) ||
      !write_reg_raw(RV3028_EEPROM_CMD, cmd))
    return false;
  if (!eeprom_wait_ready())
    return false;
  if (cmd == EEPROMCMD_ReadSingle && !read_reg_raw(RV3028_EEPROM_DATA, data))
    return false;
  return true;
}

static bool eeprom_span(bool write, uint8_t addr, uint8_t *buf, uint8_t n) {
  if (!rtc_probe() || (uint16_t)addr + n > RTC_USER_EEPROM_SIZE)
    return false;
  if (!eeprom_set_eerd(true))
    return false;
  bool ok = true;
  for (uint8_t i = 0; i < n && ok; i++)
    ok = eeprom_byte_op(write ? EEPROMCMD_WriteSingle : EEPROMCMD_ReadSingle,
                        (uint8_t)(addr + i), buf[i]);
  eeprom_set_eerd(false); // re-enable auto refresh regardless
  eeprom_wait_ready();
  return ok;
}

bool rtc_eeprom_read(uint8_t addr, uint8_t *buf, uint8_t n) {
  return eeprom_span(false, addr, buf, n);
}

bool rtc_eeprom_write(uint8_t addr, const uint8_t *buf, uint8_t n) {
  return eeprom_span(true, addr, (uint8_t *)buf, n);
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
