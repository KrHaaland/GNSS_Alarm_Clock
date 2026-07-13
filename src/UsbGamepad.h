// UsbGamepad.h — LIS3DH tilt as a USB HID gamepad (MODE_GAME).
// The HID interface is registered at boot (USB descriptors cannot change
// after enumeration), but reports are only sent while settings().mode is
// MODE_GAME: tilt = left stick X/Y, B2/B3/B4 = buttons 1..3 (B1 keeps its
// UI role so you can always reach the menu).
#pragma once
#include <Arduino.h>

void gamepad_begin(); // register the HID interface (call early in setup)
void gamepad_task();  // send reports at ~50 Hz while in MODE_GAME
