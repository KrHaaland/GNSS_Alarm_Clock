// RtcRV3028.cpp — RV-3028-C7 on I2C 0x52. UTC in the calendar registers.
// Wire.begin() is done by main setup() before rtc_begin().
#include "RtcRV3028.h"
#include "Timezone.h"
#include <Wire.h>
#include <RV-3028-C7.h>

static RV3028 rtc;
static bool s_present = false;
static bool s_lostPower = false; // PORF latched at begin
static int s_backupCfg = -1;  // last EEPROM 0x37 byte read, -1 = read failed
static bool s_backupOk = false; // byte verified: BSM=LSM and TCE off

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

// Poll STATUS.EEBUSY clear. Two hard-won bench facts: (1) the chip NACKs I2C
// entirely while its EEPROM engine runs (e.g. right after ReadSingle), so a
// failed STATUS read means "still busy" — keep polling, don't bail. (2) the
// POR auto-refresh at power-on holds EEBUSY well past 100 ms, hence the
// generous deadline.
static bool eeprom_wait_ready() {
  uint32_t deadline = millis() + 300;
  uint8_t st;
  while ((int32_t)(deadline - millis()) > 0) {
    if (read_reg_raw(RV3028_STATUS, st) && !(st & (1u << STATUS_EEBUSY)))
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

// One user-EEPROM byte op attempt: cmd = EEPROMCMD_ReadSingle / _WriteSingle.
static bool eeprom_byte_attempt(uint8_t cmd, uint8_t addr, uint8_t &data) {
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

// Robust byte op: the chip transiently NACKs register writes in a short
// window after a previous EEPROM programming cycle (seen on the bench as a
// NACKed EEDATA write right after a successful byte), so retry a few times
// with a small backoff. Writes are additionally verified by reading the byte
// back — at our save rates (a handful of bytes, a few times a day) the extra
// read is free insurance.
static bool eeprom_byte_op(uint8_t cmd, uint8_t addr, uint8_t &data) {
  for (uint8_t attempt = 0; attempt < 5; attempt++) {
    if (attempt)
      delay(3);
    if (!eeprom_byte_attempt(cmd, addr, data))
      continue;
    if (cmd != EEPROMCMD_WriteSingle)
      return true;
    uint8_t back = (uint8_t)~data;
    if (eeprom_byte_attempt(EEPROMCMD_ReadSingle, addr, back) && back == data)
      return true;
  }
  return false;
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

  // 2026-08-02: the RTC lost time when the battery ran flat, so the backup
  // switchover was NOT in effect despite the begin() above nominally
  // configuring LSM on every boot. Trust nothing the library returned: read
  // the actual EEPROM byte back and rewrite until it verifies — BSM must be
  // LSM (0b11) and the trickle charger off (VBACKUP is the Li-ion cell on
  // v2; the nPM1300 charges it, not the RTC).
  const uint8_t bsmLsm = (uint8_t)(0x3u << EEPROMBackup_BSM_SHIFT);
  const uint8_t tceBit = (uint8_t)(1u << EEPROMBackup_TCE_BIT);
  for (uint8_t i = 0; i < 3 && !s_backupOk; i++) {
    if (i) { // previous read bad or failed: rewrite via the lib, then recheck
      rtc.setBackupSwitchoverMode(3);
      rtc.disableTrickleCharge();
      eeprom_wait_ready();
    }
    // ReadSingle only reaches the user EEPROM (bench-verified: it fails on
    // 0x37), so read the config EEPROM the indirect way: a Refresh command
    // reloads every config RAM mirror from EEPROM, then a plain register
    // read of 0x37 shows what the EEPROM really holds. This also means a
    // lib write that only reached the RAM mirror gets reverted here and
    // correctly fails verification.
    if (eeprom_set_eerd(true)) {
      uint8_t ee = 0;
      bool got = eeprom_wait_ready() &&
                 write_reg_raw(RV3028_EEPROM_CMD, EEPROMCMD_First) &&
                 write_reg_raw(RV3028_EEPROM_CMD, EEPROMCMD_Refresh) &&
                 eeprom_wait_ready() &&
                 read_reg_raw(EEPROM_Backup_Register, ee);
      eeprom_set_eerd(false);
      eeprom_wait_ready();
      if (got) {
        s_backupCfg = ee; // keep the raw byte for System info diagnostics
        s_backupOk = (ee & bsmLsm) == bsmLsm && !(ee & tceBit);
      }
    }
  }
  return ok && s_present;
}

int rtc_backup_config() { return s_backupOk ? s_backupCfg : -1; }
int rtc_backup_raw() { return s_backupCfg; }

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
  // Sticky for the whole session: this is diagnostic evidence (shown on
  // System info), not a one-shot event — a POR means the displayed time
  // cannot be trusted until a GNSS sync, regardless of who asked first.
  // Also catch a PORF raised after begin (backup drained while running).
  if (s_present && rtc.readBit(RV3028_STATUS, STATUS_PORF)) {
    r = true;
    s_lostPower = true;
    rtc.clearBit(RV3028_STATUS, STATUS_PORF);
  }
  return r;
}
