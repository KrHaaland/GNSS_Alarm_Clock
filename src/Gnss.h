// Gnss.h — Quectel L86-M33 GNSS receiver on Serial1 (PA22 TX -> L86 RX,
// PA23 RX <- L86 TX), NMEA at 9600 baud, parsed with TinyGPS++.
// L86 RESET_N (PB31) and FORCE_ON (PB00) are left hi-Z; gnss_hw_reset()
// pulses RESET_N low via direct port access.
#pragma once
#include <Arduino.h>
#include <time.h>

void gnss_begin();  // opens Serial1, configures L86 (RMC+GGA, 1 Hz)
void gnss_task();   // call every loop iteration; feeds parser

// True when the receiver reports a valid date+time (fix not required for
// time on many receivers, but we only trust time once date is valid too).
bool gnss_time_valid();
// UTC epoch of the last decoded time + how many ms ago it was received.
// Returns false if no valid time yet.
bool gnss_get_utc(time_t &utc, uint32_t &ageMs);

bool gnss_has_fix();
bool gnss_get_position(float &lat, float &lon); // last valid fix
uint8_t gnss_num_sats();
uint16_t gnss_hdop_x10(); // HDOP * 10, 0xFFFF when unknown
uint32_t gnss_chars_seen(); // diagnostics: raw NMEA chars processed

void gnss_hw_reset(); // pulse L86 RESET_N (blocks ~110 ms)
