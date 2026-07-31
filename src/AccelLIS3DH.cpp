// AccelLIS3DH.cpp — LIS3DH double-tap + motion detection, polled (INT1 unused).
#include "AccelLIS3DH.h"
#include "pins.h"
#include <Adafruit_LIS3DH.h>
#include <Wire.h>
#include <math.h>

// Double-tap: a deliberate two-hit gesture that the speaker's continuous
// on-board vibration won't reproduce (unlike a single tap). Threshold is a
// first-pass value; tune on hardware if taps are hard/too easy to register.
#define CLICK_THRESHOLD 32
#define POLL_INTERVAL_MS 20

// Motion "shake to wake": summed inter-sample acceleration change (m/s^2 over
// the 3 axes) above which a deliberate nudge/shake is registered. Set above
// ambient vibration but below a firm tap. Tune on hardware. (~6 = ~0.6 g.)
#define MOVE_THRESHOLD 6.0f

static Adafruit_LIS3DH lis;
static uint8_t s_addr = I2C_ADDR_ACCEL;
static bool present = false;
static bool tappedFlag = false;
static bool movedFlag = false;
static uint32_t lastPollMs = 0;
static float s_ax = 0, s_ay = 0, s_az = 0; // previous accel sample (m/s^2)
static bool s_haveAccel = false;

bool accel_begin() {
  // SA0 floats on both board revisions, so the I2C address is a per-board
  // coin toss: the v1 prototype landed on 0x18, the first v2 board on 0x19.
  // Probe both. (Strap SA0 on the next spin.)
  s_addr = I2C_ADDR_ACCEL;
  present = lis.begin(s_addr);
  if (!present) {
    s_addr = I2C_ADDR_ACCEL + 1; // SA0 floated high
    present = lis.begin(s_addr);
  }
  if (!present)
    return false;
  lis.setRange(LIS3DH_RANGE_4_G);
  lis.setDataRate(LIS3DH_DATARATE_400_HZ); // click detect wants a high ODR
  lis.setClick(2, CLICK_THRESHOLD, 10, 20, 255); // 2 = double click
  // Enable the high-pass filter on the CLICK path (CTRL_REG2). Without it the
  // static ~1 g on Z sits at the click threshold and the detector fires
  // continuously (observed src=0x64 every poll). HPM=normal(10) | HPCLICK(bit2).
  Wire.beginTransmission(s_addr);
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

  // Motion detection for "shake/tap to wake": the data-output path is NOT
  // high-pass filtered (CTRL_REG2 FDS=0), so getEvent() returns absolute
  // acceleration incl. gravity — steady at rest, spiking when moved. Wake on a
  // large change between successive samples.
  sensors_event_t ev;
  if (lis.getEvent(&ev)) {
    if (s_haveAccel) {
      float d = fabsf(ev.acceleration.x - s_ax) +
                fabsf(ev.acceleration.y - s_ay) +
                fabsf(ev.acceleration.z - s_az);
      if (d > MOVE_THRESHOLD)
        movedFlag = true;
    }
    s_ax = ev.acceleration.x;
    s_ay = ev.acceleration.y;
    s_az = ev.acceleration.z;
    s_haveAccel = true;
  }
}

bool accel_present() { return present; }

bool accel_tapped() {
  bool t = tappedFlag;
  tappedFlag = false;
  return t;
}

bool accel_moved() {
  bool m = movedFlag;
  movedFlag = false;
  return m;
}

bool accel_get(float &x, float &y, float &z) {
  if (!s_haveAccel)
    return false;
  x = s_ax;
  y = s_ay;
  z = s_az;
  return true;
}
