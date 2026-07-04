// AccelLIS3DH.cpp — LIS3DH single-tap detection, polled (INT1 unused).
#include "AccelLIS3DH.h"
#include "pins.h"
#include <Adafruit_LIS3DH.h>

#define CLICK_THRESHOLD 40
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
  lis.setClick(1, CLICK_THRESHOLD, 10, 20, 255); // single click
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
  // CLICK_SRC: 0x10 = single click, 0x20 = double
  if ((src & 0x30) != 0)
    tappedFlag = true;
}

bool accel_present() { return present; }

bool accel_tapped() {
  bool t = tappedFlag;
  tappedFlag = false;
  return t;
}
