// AccelLIS3DH.h — LIS3DH accelerometer (I2C 0x18, INT1 on PB08/A4).
// Used for tap-to-snooze: single tap on the clock body while ringing
// snoozes the alarm. Optional feature; degrades gracefully if absent.
#pragma once
#include <Arduino.h>

bool accel_begin();     // configure click detection; false if chip missing
void accel_task();      // poll click source
bool accel_present();
// True once per detected tap (cleared on read).
bool accel_tapped();
