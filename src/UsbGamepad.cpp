// UsbGamepad.cpp — USB HID gamepad fed by the LIS3DH (MODE_GAME). See header.
#include "UsbGamepad.h"

#include <Adafruit_TinyUSB.h>

#include "AccelLIS3DH.h"
#include "Buttons.h"
#include "Settings.h"

// Standard TinyUSB gamepad report (X/Y/Z/Rz/Rx/Ry sticks, hat, 32 buttons).
static const uint8_t kReportDesc[] = {TUD_HID_REPORT_DESC_GAMEPAD()};
static Adafruit_USBD_HID s_hid(kReportDesc, sizeof(kReportDesc),
                               HID_ITF_PROTOCOL_NONE, 10, false);

#define REPORT_INTERVAL_MS 20 // ~50 Hz
// Full stick deflection at this tilt (m/s^2 along the axis). ~3.4 m/s^2 is a
// comfortable ~20 degrees of tilt; tune to taste.
#define TILT_FULL_SCALE 3.4f

static uint32_t s_lastMs;
static bool s_wasActive; // send one neutral report when leaving MODE_GAME

static int8_t axis_from(float a) {
  float v = (a / TILT_FULL_SCALE) * 127.0f;
  if (v > 127.0f)
    v = 127.0f;
  if (v < -127.0f)
    v = -127.0f;
  return (int8_t)v;
}

void gamepad_begin() {
  // Registered unconditionally: the config descriptor is fixed at
  // enumeration, so the gamepad interface exists in every mode (idle unless
  // MODE_GAME). Hosts just see a centered, silent controller.
  s_hid.begin();
}

void gamepad_task() {
  uint32_t now = millis();
  if ((uint32_t)(now - s_lastMs) < REPORT_INTERVAL_MS)
    return;
  s_lastMs = now;

  bool active = settings().mode == MODE_GAME;
  if (!active && !s_wasActive)
    return;
  if (!TinyUSBDevice.mounted() || !s_hid.ready())
    return;

  hid_gamepad_report_t rp = {};
  if (active) {
    float ax, ay, az;
    if (accel_get(ax, ay, az)) {
      // Board flat on the desk: gravity on Z, tilt shows up on X/Y.
      rp.x = axis_from(ax);
      rp.y = axis_from(ay);
    }
    // B1 keeps its UI role (menu/home); B2..B4 are gamepad buttons 1..3.
    if (buttons_is_down(BtnId::B2))
      rp.buttons |= GAMEPAD_BUTTON_0;
    if (buttons_is_down(BtnId::B3))
      rp.buttons |= GAMEPAD_BUTTON_1;
    if (buttons_is_down(BtnId::B4))
      rp.buttons |= GAMEPAD_BUTTON_2;
  }
  // else: one last all-neutral report so the host doesn't keep a stuck axis.

  s_hid.sendReport(0, &rp, sizeof(rp));
  s_wasActive = active;
}
