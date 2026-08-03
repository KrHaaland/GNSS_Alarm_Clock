// RtcRV3028.h — RV-3028-C7 RTC on I2C (0x52). VBACKUP is wired to the
// battery (v2: Li-ion VBAT; v1: the LTC3226 supercap rail), so the RTC keeps
// time across power loss AND ship mode. We store UTC in the RTC calendar
// registers, always.
//
// rtc_begin() configures (once, values persist in RTC EEPROM):
//   - backup switchover mode = Level Switching Mode (LSM)
//   - trickle charger disabled (the charger charges the battery, not the RTC)
// and VERIFIES the EEPROM byte by reading it back (a flat-battery incident
// on 2026-08-02 reset the RTC — the switchover was silently not in effect).
#pragma once
#include <Arduino.h>
#include <time.h>

bool rtc_begin();               // false if the chip does not respond
bool rtc_present();
bool rtc_get_utc(time_t &utc);  // false if RTC absent or time implausible
bool rtc_set_utc(time_t utc);
// Power-On Reset flag: true if the RTC lost time (backup drained or a POR
// transient) this session. Sticky — diagnostic evidence on System info.
bool rtc_lost_power();
// The verified EEPROM Backup register (0x37) byte, or -1 if verification
// failed at boot (backup switchover NOT guaranteed — shown on System info).
int rtc_backup_config();
// The raw 0x37 byte from the last boot-time read even when verification
// failed, or -1 if the read itself never succeeded. Diagnostics only.
int rtc_backup_raw();

// --- User EEPROM (43 bytes, addresses 0x00..0x2A) ---------------------------
// Settings live here: survives power loss AND firmware reflashes, ~100k write
// cycles per byte. Raw I2C (usable before rtc_begin()); ~16 ms per written
// byte (EEPROM programming time), reads are quick. False on absent chip,
// out-of-range address, or EEPROM-busy timeout.
#define RTC_USER_EEPROM_SIZE 43
bool rtc_eeprom_read(uint8_t addr, uint8_t *buf, uint8_t n);
bool rtc_eeprom_write(uint8_t addr, const uint8_t *buf, uint8_t n);
