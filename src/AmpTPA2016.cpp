// AmpTPA2016.cpp — TPA2016D2 over raw Wire (Adafruit driver not in lib_deps).
// Mono: right channel only (DAC -> INR, speaker on OUTR). AGC compression is
// off (1:1) so the output limiter acts as a plain peak limiter; loudness is
// set through the fixed-gain register.
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
#define SETUP_NOISEGATE 0x01

// Right on, left off, noise gate on; SWS bit added for volume-0 mute.
#define SETUP_BASE (SETUP_R_EN | SETUP_NOISEGATE)
// Limiter enabled (bit7=0), NG threshold 4 mV (01), level 0x1A = 6.5 dBV
// (chip POR default).
#define AGC1_VALUE 0x3A
// Max gain 30 dB (field = dB-18 = 12), compression ratio 1:1 (off).
#define AGC2_VALUE 0xC0

static bool present = false;
static uint8_t curVol = 7;

// vol 1..10 -> fixed gain dB, roughly linear -20..+12
static const int8_t kGainDb[10] = {-20, -16, -13, -9, -6, -2, 1, 5, 8, 12};

static bool write_reg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(I2C_ADDR_AMP);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static void apply_config() {
  write_reg(REG_ATK, 0x05);
  write_reg(REG_REL, 0x0B);
  write_reg(REG_HOLD, 0x00);
  write_reg(REG_AGC1, AGC1_VALUE);
  write_reg(REG_AGC2, AGC2_VALUE);
  if (curVol == 0) {
    write_reg(REG_SETUP, SETUP_BASE | SETUP_SWS);
  } else {
    write_reg(REG_GAIN, (uint8_t)(kGainDb[curVol - 1] & 0x3F));
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
  return present;
}

void amp_enable(bool on) {
  if (on) {
    digitalWrite(PIN_AMP_SHUTDOWN, HIGH);
    if (present) {
      delay(2);
      apply_config(); // rewrite in case hardware shutdown dropped register state
    }
  } else {
    digitalWrite(PIN_AMP_SHUTDOWN, LOW);
  }
}

void amp_set_volume(uint8_t vol0to10) {
  curVol = (vol0to10 > 10) ? 10 : vol0to10;
  if (!present)
    return;
  if (curVol == 0) {
    write_reg(REG_SETUP, SETUP_BASE | SETUP_SWS); // software-shutdown mute
    return;
  }
  write_reg(REG_GAIN, (uint8_t)(kGainDb[curVol - 1] & 0x3F));
  write_reg(REG_SETUP, SETUP_BASE);
}

bool amp_present() { return present; }
