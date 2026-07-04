// Leds.h — 34 white LEDs in 3 sections (Left 10 / Bottom 14 / Right 10)
// around the face, fed from the ALARMPOWER supercap rail via 80R and
// switched by low-side MOSFETs (gate HIGH = section on). Full brightness
// even on weak USB supplies thanks to the LTC3226-charged supercaps.
#pragma once
#include <Arduino.h>

enum class LedPattern : uint8_t {
  Off,
  Chase,     // L -> B -> R rotating, one section at a time
  Blink,     // all three flashing together
  BuildUp,   // L, L+B, L+B+R, off, repeat
  On         // all on steady
};

void leds_begin();
void leds_task();                       // advances the active pattern
void leds_start(LedPattern p, uint16_t stepMs = 150);
void leds_stop();                       // all off
bool leds_active();
void leds_set(bool left, bool bottom, bool right); // manual control
bool supercaps_ready(); // LTC3226 CAPGOOD (HIGH = charged)
