// AccelLIS3DH.h — LIS3DH accelerometer (I2C 0x18, INT1 on PB08/A4).
// Two uses: (1) double-tap-to-snooze while an alarm is ringing; (2) any motion
// ("shake/tap to wake") to bring the dimmed display back to full brightness.
// Optional feature; degrades gracefully if the chip is absent.
#pragma once
#include <Arduino.h>

bool accel_begin();     // configure click + motion detection; false if missing
void accel_task();      // poll click source + motion
bool accel_present();
// True once per detected double-tap (cleared on read).
bool accel_tapped();
// True once per detected motion/shake (cleared on read). Wakes the display.
bool accel_moved();
