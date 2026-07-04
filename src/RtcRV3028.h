// RtcRV3028.h — RV-3028-C7 RTC on I2C (0x52). VBACKUP is wired to the
// LTC3226 supercap rail (ALARMPOWER), so the RTC keeps time across power
// loss. We store UTC in the RTC calendar registers, always.
//
// rtc_begin() configures (once, values persist in RTC EEPROM):
//   - backup switchover mode = Level Switching Mode (LSM)
//   - trickle charger disabled (the LTC3226 charges the caps, not the RTC)
#pragma once
#include <Arduino.h>
#include <time.h>

bool rtc_begin();               // false if the chip does not respond
bool rtc_present();
bool rtc_get_utc(time_t &utc);  // false if RTC absent or time implausible
bool rtc_set_utc(time_t utc);
// Power-On Reset flag: true if the RTC lost time (backup drained). Cleared
// after reading.
bool rtc_lost_power();
