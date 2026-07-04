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
//   - UI: LVGL on the SH1122 256x64 OLED, 4 buttons above the display.
//   - Tunes: the QSPI flash appears as a USB drive (drag & drop WAVs).

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "pins.h"
#include "Settings.h"
#include "TuneStorage.h"
#include "DisplaySH1122.h"
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
}

// ---- Arduino entry points --------------------------------------------------

void setup() {
  Serial.begin(115200); // USB CDC; no wait — clock must boot headless

  Wire.begin();
  Wire.setClock(400000);

  settings_begin();

  // Storage first: it brings up TinyUSB MSC, best done early after boot.
  storage_begin();

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
}

void loop() {
  // Inputs & housekeeping
  buttons_task();
  accel_task();
  gnss_task();
  clock_task();
  storage_task();
  audio_task();
  leds_task();
  display_task();

  // Alarm engine
  alarm_task();

  dispatch_buttons();
  ui_task();

  lv_timer_handler();
}
