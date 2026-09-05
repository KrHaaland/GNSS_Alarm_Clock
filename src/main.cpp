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
//   - UI: LVGL on the NV3007 428x142 color TFT, 4 buttons above the display.
//   - Tunes: the QSPI flash appears as a USB drive (drag & drop WAVs).

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "pins.h"
#include "Settings.h"
#include "TuneStorage.h"
#include "Display.h"
#include "Buttons.h"
#include "Leds.h"
#include "AmpTPA2016.h"
#include "AudioEngine.h"
#include "AccelLIS3DH.h"
#include "Gnss.h"
#include "PmicNPM1300.h"
#include "RtcRV3028.h"

#ifdef __SAMD51J20A__
LV_IMAGE_DECLARE(img_lowbatt); // src/img_lowbatt.c (128x70, I1, white)
#endif
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

  if (a.lights)
    leds_start(LedPattern::Chase, 150); // per-alarm: off = sound-only alarm

  if (!escalated) {
    // Initial fire or snooze re-ring: kill any tune preview and (re)start
    // the alarm sound from the beginning.
    audio_stop();
    // Digital volume pinned high: with AGC leveling on the amp, loudness is
    // set by the amp's limiter (amp_set_volume/amp_ramp_to). See ADR-0010.
    audio_set_volume(10);
    amp_enable(true);
    if (!alarm_fire_was_rering() && a.rampSeconds > 0)
      amp_ramp_to(settings().volume, a.rampSeconds); // gentle wake (per alarm)
    else
      amp_set_volume(settings().volume); // re-ring: no mercy
    bool ok = false;
    if (a.tune[0] != '\0')
      ok = audio_play_wav(a.tune, true);
    if (!ok)
      audio_play_melody(a.melodyId, true);
  }

  if (escalated && !s_escalated) {
    s_escalated = true;
    amp_set_volume(settings().volume); // escalation jumps past any ramp
    buzzer_start();
    if (a.lights)
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
  // Batteryless-boot survival (v2): the nPM1300 holds its VBUS limit at
  // 100 mA until we raise it, and with no battery to supplement, any excess
  // browns the board out. Clamp the two big loads firmware can reach BEFORE
  // anything else runs: hold the L86 in reset (it hangs straight on 3.3V,
  // no enable pin, and bursts ~100 mA acquiring) and force the amp off
  // (R68 pulls TPA2016 ~SD high = ON by default). The UF2 bootloader phase
  // remains unprotected — a custom bootloader is the full fix.
  // Core buck regulator: the Arduino core defaults to the LDO, but the
  // board carries the VSW inductor (L4), so switch VDDCORE to the internal
  // buck — saves ~4-6 mA at 3.3 V continuously (and trims the MCU's draw
  // inside the PMIC's 100 mA power-up window as a bonus).
  SUPC->VREG.bit.SEL = 1;
  while (!SUPC->STATUS.bit.VREGRDY) {
  }

  portb_output(GNSS_RESET_PORTPIN, false); // L86 core stopped
  pinMode(PIN_AMP_SHUTDOWN, OUTPUT);
  digitalWrite(PIN_AMP_SHUTDOWN, LOW); // amp off
  // Some display modules strap BL with a PULL-UP: a floating PA19 means the
  // backlight burns at full power all through the bootloader phase (found
  // the hard way — batteryless boot-loop). Drive it low the instant the app
  // starts; display_init() ramps it up properly later.
  pinMode(PIN_OLED_BL, OUTPUT);
  digitalWrite(PIN_OLED_BL, LOW);

  Wire.begin();
  // 100 kHz (standard mode): the custom board's I2C devices lack local HF
  // decoupling (see HARDWARE_REVIEW finding #12), so 400 kHz was unreliable and
  // the RV-3028 RTC intermittently failed to ACK ("RTC missing"). All bus
  // devices (RTC, accel, EEPROM, amp) work fine at 100 kHz.
  Wire.setClock(100000);

  // v2 boards: raise the nPM1300's VBUS current limit from its 100 mA
  // power-up default BEFORE any real load (LED sections!) can switch on.
  // No-op on v1 (no PMIC on the bus).
  pmic_begin();

  portb_hiz(GNSS_RESET_PORTPIN); // budget secured: release the L86

  Serial.begin(115200); // USB CDC; no wait — clock must boot headless

  // Storage first: it brings up TinyUSB MSC, best done early after boot.
  storage_begin();
  // HID gamepad interface must exist before the host enumerates (descriptors
  // are fixed after that); it stays silent unless MODE_GAME is selected.
  gamepad_begin();

  // After storage (alarm tune names are re-matched against the TUNES
  // directory) and after Wire (settings live in the RTC's user EEPROM).
  settings_begin();

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

  // Low-battery boot gate: woken on battery with the cell still under
  // 3.45 V (hysteresis above the 3.40 V runtime cutoff), we say so for 4 s
  // and return to ship mode. A connected charger always allows boot.
  if (pmic_present()) {
    PmicStatus st;
    if (pmic_read_status(st) && !st.vbusPresent &&
        st.vbatMv < PMIC_VBAT_BOOT_MIN_MV) {
      lv_obj_t *scr = lv_screen_active();
      lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
      lv_obj_t *l = lv_label_create(scr);
#ifdef __SAMD51J20A__
      // 128x70 battery icon (the user's XBM from an earlier project) on the
      // left, big text filling the right half. Montserrat 48 costs ~67 KB
      // flash — fine on the J20, but it OVERFLOWS the J19 by ~36 KB, so v1
      // builds keep the plain small-text message (their PMIC-less boards
      // can never reach this code anyway; pmic_present() is false).
      lv_obj_t *icon = lv_image_create(scr);
      lv_image_set_src(icon, &img_lowbatt);
      lv_obj_align(icon, LV_ALIGN_LEFT_MID, 18, 0);
      lv_obj_set_style_text_font(l, &lv_font_montserrat_48, 0);
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
      lv_label_set_text_static(l, "LOW\nBATTERY");
      lv_obj_align(l, LV_ALIGN_CENTER, 80, 0); // centered in the right half
#else
      lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
      lv_label_set_text_static(l, "LOW BATTERY");
      lv_obj_center(l);
#endif
      lv_obj_set_style_text_color(l, lv_color_white(), 0);
      uint32_t t0 = millis(), lastChk = 0;
      bool charger = false;
      while (millis() - t0 < 4000) {
        lv_timer_handler();
        delay(5);
        // A charger plugged mid-message rescues the boot ("a connected
        // charger always allows boot" — and the PMIC would refuse ship
        // entry with VBUS present anyway, which used to hang here).
        if (millis() - lastChk >= 250) {
          lastChk = millis();
          PmicStatus st2;
          if (pmic_read_status(st2) && st2.vbusPresent) {
            charger = true;
            break;
          }
        }
      }
      if (!charger)
        pmic_enter_ship_mode(); // powers off — or reboots if VBUS won a race
      lv_obj_delete(l); // charger appeared: clean the splash, boot normally
#ifdef __SAMD51J20A__
      lv_obj_delete(icon);
#endif
    }
  }

  display_set_contrast(settings().brightness);
  ui_begin();

  alarm_begin();
  alarm_set_callbacks(start_ringing, stop_ringing);

#ifdef GNSS_SIM
  sim_console_begin(); // env:sim — type coordinates over serial (no real GPS)
#endif
}

void loop() {
  // Low-battery cutoff (battery operation only): three consecutive 10 s
  // readings under 3.40 V -> ship mode, so the cell never grinds through
  // the brownout-loop zone below the LM3671's dropout. BUTTON1 or a USB
  // plug wakes the PMIC; the RTC keeps time throughout (VBACKUP on VBAT).
  // nPM1300 IRQ line (GPIO0 -> PA04): high while VBUS attach/removal events
  // pend. Attach must re-apply the input-current limit, which the PMIC
  // resets to 100 mA on every replug.
  if (pmic_present() && digitalRead(PIN_CAPGOOD))
    pmic_handle_irq();
  pmic_task(); // delayed VBUS reconfigure passes (CC settle + retry)

  // The check pauses whenever an alarm is Ringing OR Snoozed: the LED show
  // + speaker sag VBAT 50-100 mV (cell IR), which would false-trigger the
  // cutoff mid-alarm — and a shutdown while snoozed would kill the re-ring.
  // Waking someone up beats saving the last percent of a battery.
  static uint32_t s_batChkMs;
  static uint8_t s_batLowCount;
  if (pmic_present() && (uint32_t)(millis() - s_batChkMs) >= 10000) {
    s_batChkMs = millis();
    PmicStatus st;
    if (pmic_read_status(st)) {
      bool alarmActive = alarm_state() != AlarmState::Idle;
      if (!st.vbusPresent && !alarmActive && st.vbatMv < PMIC_VBAT_SHIP_MV) {
        if (++s_batLowCount >= 3) {
          // Farewell alert: one short high chirp + a sequential light sweep,
          // so a clock dying on the nightstand announces "charge me" instead
          // of silently going dark. Moderate volume + one LED section at a
          // time keeps the draw small at this battery level. The manual
          // Shutdown menu stays silent on purpose (the user is present).
          amp_enable(true);
          amp_set_volume(6);
          audio_play_beep(2200, 500);
          static const bool SEQ[4][3] = {
              {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}};
          for (uint8_t i = 0; i < 4; i++) {
            leds_set(SEQ[i][0], SEQ[i][1], SEQ[i][2]);
            uint32_t t0 = millis();
            while ((uint32_t)(millis() - t0) < 170)
              audio_task(); // keep the chirp fed while the sweep runs
          }
          amp_enable(false);
          pmic_enter_ship_mode(); // does not return
        }
      } else {
        s_batLowCount = 0;
      }
    }
  }

  // Debug screenshot: a '~' on the USB serial console streams the next frame
  // (tools/screenshot.py). peek() so GNSS_SIM console lines pass through.
  if (Serial.available() && Serial.peek() == '~') {
    Serial.read();
    display_screenshot_arm();
  }

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
  amp_task();     // gentle-wake volume ramp

  lv_timer_handler();
}
