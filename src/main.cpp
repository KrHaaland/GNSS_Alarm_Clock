// GNSS Alarm Clock — main firmware entry.
//
// Custom SAMD51J19A board (Metro M4 compatible). See include/pins.h for the
// full hardware map and README.md for behavior. Broad strokes:
//   - Time: GNSS (L86, Serial1) disciplines the RV-3028 RTC; timezone is
//     derived offline from coordinates and persisted to internal flash.
//   - Alarm: LED sections chase from the supercap rail, a tune plays from
//     QSPI-flash WAV (or builtin melody) through DAC0 -> TPA2016D2, with
//     optional power-buzzer escalation. Snooze by button short-press or
//     accelerometer tap, stop by long-press.
//   - UI: LVGL on the ST7789 284x76 color TFT, 4 buttons above the display.
//   - Tunes: the QSPI flash appears as a USB drive (drag & drop WAVs).

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "pins.h"
#include "Settings.h"
#include "TuneStorage.h"
#include "DisplayST7789.h"
#include "Buttons.h"
#include "Leds.h"
#include "AmpTPA2016.h"
#include "AudioEngine.h"
#include "AccelLIS3DH.h"
#include "Gnss.h"
#include "RtcRV3028.h"
#include "ClockKeeper.h"
#include "AlarmManager.h"
#include "Ui.h"
#include "SimConsole.h"
#include "UsbGamepad.h"

// Diagnostic: when 1, setup() scans the whole I2C bus and prints every address
// that ACKs, in a loop over USB serial, instead of booting normally. Set to 0
// for normal operation. Known devices: RTC 0x52, LIS3DH 0x18/0x19,
// 24LC512 EEPROM 0x50, TPA2016 amp 0x58.
#define I2C_SCAN 0

static bool s_escalated = false;
static uint32_t s_ringStartMs = 0;   // when the current ring/re-ring began
#define TAP_ARM_MS 2000u             // ignore taps for the first 2 s of a ring

// ---- Alarm ring/silence orchestration -------------------------------------

static void start_ringing(int8_t idx, bool escalated) {
  const AlarmConfig &a = settings().alarms[idx];

  // Discard any stale accelerometer tap. The tap flag is set on any handling
  // of the clock but only consumed while ringing, so without this a bump from
  // minutes ago would instantly snooze the alarm the moment it fires.
  accel_tapped();
  if (!escalated)
    s_ringStartMs = millis(); // arm-delay reference for tap-to-snooze

  leds_start(LedPattern::Chase, 150);

  if (!escalated) {
    // Initial fire or snooze re-ring: kill any tune preview and (re)start
    // the alarm sound from the beginning.
    audio_stop();
    amp_set_volume(settings().volume);
    audio_set_volume(settings().volume);
    amp_enable(true);
    bool ok = false;
    if (a.tune[0] != '\0')
      ok = audio_play_wav(a.tune, true);
    if (!ok)
      audio_play_melody(a.melodyId, true);
  }

  if (escalated && !s_escalated) {
    s_escalated = true;
    buzzer_start();
    leds_start(LedPattern::Blink, 250); // more aggressive pattern
  }

  ui_show_ringing(idx);
}

static void stop_ringing() {
  buzzer_stop();
  audio_stop();
  amp_enable(false);
  leds_stop();
  s_escalated = false;
  ui_hide_ringing();
}

// Route button events: while ringing, short = snooze / long = stop; while
// snoozed, long = stop (short presses navigate normally); otherwise UI nav.
static void dispatch_buttons() {
  ButtonEvent ev;
  while (buttons_pop_event(ev)) {
    ui_poke();
    if (alarm_state() == AlarmState::Ringing) {
      if (ev.action == BtnAction::Long)
        alarm_stop();
      else
        alarm_snooze();
    } else if (alarm_state() == AlarmState::Snoozed &&
               ev.action == BtnAction::Long) {
      alarm_stop();
    } else {
      ui_handle_event(ev);
    }
  }
  if (alarm_state() == AlarmState::Ringing && settings().tapSnooze) {
    bool tapped = accel_tapped(); // always drain so it can't accumulate
    if (tapped && (millis() - s_ringStartMs) >= TAP_ARM_MS)
      alarm_snooze();
  }

  // Shake/tap to wake: any motion restores full brightness and resets the dim
  // timer (same as a button press). Harmless while ringing (already bright).
  if (accel_moved())
    ui_poke();
}

// ---- Arduino entry points --------------------------------------------------

void setup() {
  Serial.begin(115200); // USB CDC; no wait — clock must boot headless

  Wire.begin();
  // 100 kHz (standard mode): the custom board's I2C devices lack local HF
  // decoupling (see HARDWARE_REVIEW finding #12), so 400 kHz was unreliable and
  // the RV-3028 RTC intermittently failed to ACK ("RTC missing"). All bus
  // devices (RTC, accel, EEPROM, amp) work fine at 100 kHz.
  Wire.setClock(100000);

  settings_begin();

  // Storage first: it brings up TinyUSB MSC, best done early after boot.
  storage_begin();
  // HID gamepad interface must exist before the host enumerates (descriptors
  // are fixed after that); it stays silent unless MODE_GAME is selected.
  gamepad_begin();

#if I2C_SCAN
  // Loop-scan the bus so the result streams over USB serial (delay() services
  // TinyUSB, keeping CDC alive). Reports which 7-bit addresses ACK.
  for (;;) {
    Serial.println("=== I2C scan (0x03..0x77) ===");
    uint8_t found = 0;
    for (uint8_t a = 0x03; a <= 0x77; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        Serial.print("  ACK 0x");
        if (a < 0x10)
          Serial.print('0');
        Serial.print(a, HEX);
        const char *name = (a == 0x52)                  ? "  <- RV-3028 RTC"
                           : (a == 0x18 || a == 0x19)   ? "  <- LIS3DH accel"
                           : (a >= 0x50 && a <= 0x57)   ? "  <- 24LC512 EEPROM"
                           : (a == 0x58)                ? "  <- TPA2016 amp"
                                                        : "";
        Serial.println(name);
        found++;
      }
    }
    Serial.print("  total = ");
    Serial.println(found);
    delay(1500);
  }
#endif

  buttons_begin();
  leds_begin();
  amp_begin();
  audio_begin();
  accel_begin();

  rtc_begin();
  gnss_begin();
  clock_begin();

  display_init();
  display_set_contrast(settings().brightness);
  ui_begin();

  alarm_begin();
  alarm_set_callbacks(start_ringing, stop_ringing);

#ifdef GNSS_SIM
  sim_console_begin(); // env:sim — type coordinates over serial (no real GPS)
#endif
}

void loop() {
  // Inputs & housekeeping
  buttons_task();
  accel_task();
  gnss_task();
#ifdef GNSS_SIM
  sim_console_task(); // read simulated coordinates from the USB serial console
#endif
  clock_task();
  storage_task();
  audio_task();
  leds_task();
  display_task();

  // Alarm engine
  alarm_task();

  dispatch_buttons();
  ui_task();
  gamepad_task(); // HID reports while in MODE_GAME (tilt + B2..B4)

  lv_timer_handler();
}
