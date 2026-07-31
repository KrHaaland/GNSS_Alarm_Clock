// Leds.cpp — 3 LED sections on low-side MOSFETs (gate HIGH = on), patterns
// stepped from leds_task().
#include "Leds.h"
#include "PmicNPM1300.h"
#include "pins.h"

static LedPattern pattern = LedPattern::Off;
static uint16_t stepInterval = 150;
static uint32_t lastStepMs = 0;
static uint8_t phase = 0;

static void apply(bool l, bool b, bool r) {
  digitalWrite(PIN_LEDS_LEFT, l ? HIGH : LOW);
  digitalWrite(PIN_LEDS_BOTTOM, b ? HIGH : LOW);
  digitalWrite(PIN_LEDS_RIGHT, r ? HIGH : LOW);
}

static uint8_t phase_count(LedPattern p) {
  switch (p) {
  case LedPattern::Chase:
    return 3;
  case LedPattern::Blink:
    return 2;
  case LedPattern::BuildUp:
    return 4;
  default:
    return 1;
  }
}

static void render() {
  switch (pattern) {
  case LedPattern::Off:
    apply(false, false, false);
    break;
  case LedPattern::On:
    apply(true, true, true);
    break;
  case LedPattern::Chase: // exactly one section, L -> B -> R
    apply(phase == 0, phase == 1, phase == 2);
    break;
  case LedPattern::Blink:
    apply(phase == 0, phase == 0, phase == 0);
    break;
  case LedPattern::BuildUp: // L, L+B, L+B+R, none
    apply(phase <= 2, phase >= 1 && phase <= 2, phase == 2);
    break;
  }
}

void leds_begin() {
  pinMode(PIN_LEDS_LEFT, OUTPUT);
  pinMode(PIN_LEDS_BOTTOM, OUTPUT);
  pinMode(PIN_LEDS_RIGHT, OUTPUT);
  apply(false, false, false);
  pinMode(PIN_CAPGOOD, INPUT); // external 10k pullup on CAPGD
}

void leds_start(LedPattern p, uint16_t stepMs) {
  pattern = p;
  stepInterval = (stepMs == 0) ? 1 : stepMs;
  phase = 0;
  lastStepMs = millis();
  render();
}

void leds_task() {
  if (pattern == LedPattern::Off || pattern == LedPattern::On)
    return; // steady states, nothing to step
  uint32_t now = millis();
  if ((uint32_t)(now - lastStepMs) < stepInterval)
    return;
  lastStepMs = now;
  phase = (uint8_t)((phase + 1) % phase_count(pattern));
  render();
}

void leds_stop() {
  pattern = LedPattern::Off;
  apply(false, false, false);
}

bool leds_active() { return pattern != LedPattern::Off; }

void leds_set(bool left, bool bottom, bool right) {
  pattern = LedPattern::Off; // manual control cancels any running pattern
  apply(left, bottom, right);
}

bool supercaps_ready() {
  // v2: no supercaps — PA04 is the nPM1300's GPIO0 (unconfigured, reads
  // LOW). Report ready whenever the PMIC is present and powering us; the
  // real battery/charge status UI comes with the full v2 power work.
  if (pmic_present())
    return true;
  // v1: LTC3226 CAPGD is open-drain and releases (pulled HIGH by the
  // external 10k) once the supercaps reach ~92% of regulation.
  return digitalRead(PIN_CAPGOOD) == HIGH;
}
