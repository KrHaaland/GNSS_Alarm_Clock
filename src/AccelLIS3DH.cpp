// AccelLIS3DH.cpp — LIS3DH single-tap detection, polled (INT1 unused).
#include "AccelLIS3DH.h"
#include "pins.h"
#include <Adafruit_LIS3DH.h>
#include <Wire.h>

// Double-tap: a deliberate two-hit gesture that the speaker's continuous
// on-board vibration won't reproduce (unlike a single tap). Threshold is a
// first-pass value; tune on hardware if taps are hard/too easy to register.
#define CLICK_THRESHOLD 32
#define POLL_INTERVAL_MS 20

static Adafruit_LIS3DH lis;
static bool present = false;
static bool tappedFlag = false;
static uint32_t lastPollMs = 0;

bool accel_begin() {
  present = lis.begin(I2C_ADDR_ACCEL);
  if (!present)
    return false;
  lis.setRange(LIS3DH_RANGE_4_G);
  lis.setDataRate(LIS3DH_DATARATE_400_HZ); // click detect wants a high ODR
  lis.setClick(2, CLICK_THRESHOLD, 10, 20, 255); // 2 = double click
  // Enable the high-pass filter on the CLICK path (CTRL_REG2). Without it the
  // static ~1 g on Z sits at the click threshold and the detector fires
  // continuously (observed src=0x64 every poll). HPM=normal(10) | HPCLICK(bit2).
  Wire.beginTransmission(I2C_ADDR_ACCEL);
  Wire.write(0x21); // CTRL_REG2
  Wire.write(0x84);
  Wire.endTransmission();
  return true;
}

void accel_task() {
  if (!present)
    return;
  uint32_t now = millis();
  if ((uint32_t)(now - lastPollMs) < POLL_INTERVAL_MS)
    return; // throttle CLICK_SRC reads to spare the I2C bus
  lastPollMs = now;
  uint8_t src = lis.getClick();
  // CLICK_SRC bit 0x20 = double-click detected (0x10 = single, ignored)
  if (src & 0x20)
    tappedFlag = true;
}

bool accel_present() { return present; }

bool accel_tapped() {
  bool t = tappedFlag;
  tappedFlag = false;
  return t;
}
