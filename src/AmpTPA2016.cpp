// AmpTPA2016.cpp — TPA2016D2 over raw Wire (Adafruit driver not in lib_deps).
// Mono: right channel only (DAC -> INR, speaker on OUTR). See header for the
// loudness architecture (AGC 1:4, limiter level = volume).
#include "AmpTPA2016.h"
#include "pins.h"
#include <Wire.h>

// TPA2016D2 register map
#define REG_SETUP 0x01
#define REG_ATK 0x02
#define REG_REL 0x03
#define REG_HOLD 0x04
#define REG_GAIN 0x05  // fixed gain, 6-bit two's complement, -28..+30 dB
#define REG_AGC1 0x06  // [7] limiter disable, [6:5] NG threshold, [4:0] limiter level
#define REG_AGC2 0x07  // [7:4] max gain, [1:0] compression ratio

// REG_SETUP bits (Adafruit_TPA2016 canonical map)
#define SETUP_R_EN 0x80
#define SETUP_L_EN 0x40
#define SETUP_SWS 0x20
#define SETUP_FAULT_R 0x10
#define SETUP_FAULT_L 0x08
#define SETUP_THERMAL 0x04
#define SETUP_NOISEGATE 0x01

// Right on, left off, noise gate on; SWS bit added for volume-0 mute.
#define SETUP_BASE (SETUP_R_EN | SETUP_NOISEGATE)

// Fixed input gain into the AGC. The AGC lifts program material toward the
// limiter level, so this stays constant and moderate.
#define FIXED_GAIN_DB 6
// Max AGC gain 30 dB (field = dB-18 = 12), compression ratio 1:4 (10b) to
// even out differently-mastered WAVs.
#define AGC2_VALUE 0xC2
// Volume 1..10 spans -6.5 dBV (step 0, ~28 mW into 8 ohm) up to +7 dBV
// (step 27, ~0.63 W into 8 ohm); ~1.5 dB per volume step, volume 0 = mute.
// The ceiling is NOT the chip max (+9 dBV, step 31): bench-measured
// 2026-08-06 against the nPM1300's 1000 mA battery discharge limit
// (IBATLIM — exceeding it collapses VSYS and resets the device). At step
// 31 the total draw (audio + LED chase + system) crossed the limit as the
// AGC wound up; step 27 swung 500-900 mA in the worst case (ALL LEDs on,
// bass-heavy tune, 8 ohm series speaker pair). A USB-only higher ceiling
// was considered and rejected: IBATLIM applies instantly at unplug while
// software reacts in ~10-50 ms — an unwinnable race at full blast.
// Re-tune this if the speaker configuration changes (lower impedance =
// more current at the same limiter voltage).
#define LIMITER_MAX_STEP 27
// Noise-gate threshold bits (01 = 4 mV).
#define NG_BITS 0x20

static bool present = false;
static bool sdHigh = false;   // ~SD pin state (chip only answers I2C when high)
static uint8_t curVol = 7;

// Ramp state (gentle wake): step the limiter one 0.5 dB notch per interval.
static bool rampActive = false;
static uint8_t rampStep, rampTarget;
static uint32_t rampIntervalMs, rampLastMs;

static bool write_reg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(I2C_ADDR_AMP);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool read_reg(uint8_t reg, uint8_t &val) {
  Wire.beginTransmission(I2C_ADDR_AMP);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom((uint8_t)I2C_ADDR_AMP, (size_t)1) != 1)
    return false;
  val = (uint8_t)Wire.read();
  return true;
}

// vol 1..10 -> limiter level register step: v1 = 0 (-6.5 dBV), v10 = 31 (+9).
static uint8_t limiter_step_for(uint8_t vol) {
  if (vol <= 1)
    return 0;
  return (uint8_t)(((uint16_t)(vol - 1) * LIMITER_MAX_STEP) / 9);
}

static void write_limiter(uint8_t step) {
  if (step > 31)
    step = 31;
  write_reg(REG_AGC1, (uint8_t)(NG_BITS | step)); // limiter enabled (bit7=0)
}

static void apply_config() {
  write_reg(REG_ATK, 0x05);
  write_reg(REG_REL, 0x0B);
  write_reg(REG_HOLD, 0x00);
  write_reg(REG_GAIN, (uint8_t)(FIXED_GAIN_DB & 0x3F));
  write_reg(REG_AGC2, AGC2_VALUE);
  if (curVol == 0) {
    write_limiter(0);
    write_reg(REG_SETUP, SETUP_BASE | SETUP_SWS);
  } else {
    write_limiter(limiter_step_for(curVol));
    write_reg(REG_SETUP, SETUP_BASE);
  }
}

bool amp_begin() {
  pinMode(PIN_AMP_SHUTDOWN, OUTPUT);
  digitalWrite(PIN_AMP_SHUTDOWN, HIGH); // release ~SD so the chip answers I2C
  Wire.begin();
  delay(2); // chip needs a moment out of hardware shutdown

  Wire.beginTransmission(I2C_ADDR_AMP);
  present = (Wire.endTransmission() == 0);
  if (present)
    apply_config();

  digitalWrite(PIN_AMP_SHUTDOWN, LOW); // leave in shutdown per contract
  sdHigh = false;
  return present;
}

void amp_enable(bool on) {
  if (on) {
    digitalWrite(PIN_AMP_SHUTDOWN, HIGH);
    sdHigh = true;
    if (present) {
      delay(2);
      apply_config(); // rewrite in case hardware shutdown dropped register state
    }
  } else {
    digitalWrite(PIN_AMP_SHUTDOWN, LOW);
    sdHigh = false;
    rampActive = false;
  }
}

void amp_set_volume(uint8_t vol0to10) {
  rampActive = false; // direct set cancels any gentle-wake ramp
  curVol = (vol0to10 > 10) ? 10 : vol0to10;
  if (!present)
    return;
  if (curVol == 0) {
    write_reg(REG_SETUP, SETUP_BASE | SETUP_SWS); // software-shutdown mute
    return;
  }
  write_limiter(limiter_step_for(curVol));
  write_reg(REG_SETUP, SETUP_BASE);
}

void amp_ramp_to(uint8_t vol0to10, uint16_t seconds) {
  curVol = (vol0to10 > 10) ? 10 : vol0to10;
  if (!present || curVol == 0 || seconds == 0) {
    amp_set_volume(curVol);
    return;
  }
  rampTarget = limiter_step_for(curVol);
  rampStep = 0;
  rampIntervalMs = (uint32_t)seconds * 1000u / (rampTarget ? rampTarget : 1);
  rampLastMs = millis();
  rampActive = true;
  write_limiter(0); // start at the quietest limiter level (-6.5 dBV)
  write_reg(REG_SETUP, SETUP_BASE);
}

void amp_task() {
  if (!rampActive || !present || !sdHigh)
    return;
  uint32_t now = millis();
  if ((uint32_t)(now - rampLastMs) < rampIntervalMs)
    return;
  rampLastMs = now;
  if (rampStep < rampTarget) {
    rampStep++;
    write_limiter(rampStep);
  }
  if (rampStep >= rampTarget)
    rampActive = false; // reached the configured volume
}

bool amp_present() { return present; }

bool amp_enabled() { return present && sdHigh; }

bool amp_get_status(bool &faultOut, bool &thermal) {
  if (!amp_enabled())
    return false;
  uint8_t v;
  if (!read_reg(REG_SETUP, v))
    return false;
  faultOut = (v & (SETUP_FAULT_R | SETUP_FAULT_L)) != 0;
  thermal = (v & SETUP_THERMAL) != 0;
  return true;
}
