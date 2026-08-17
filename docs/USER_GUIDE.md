# GNSS Alarm Clock — User Guide

How to operate the clock. (Building/flashing lives in the
[README](../README.md); design decisions in [docs/adr/](adr/).)

## The four buttons

Above the display, left to right:

| | B1 | B2 | B3 | B4 |
|---|---|---|---|---|
| Normal | **Back** / open menu | Prev / up | Next / down | **OK** / open menu |
| Editing a value | Cancel | − step | + step | Commit |
| Long-press | **Home** (clock) from anywhere | Coarse − | Coarse + | — |
| While ringing | any short press = **snooze**, any long press = **stop** | | | |
| While snoozed | long press = stop; short presses navigate normally | | | |
| Game mode | menu | gamepad button 1 | button 2 | button 3 |

**Editing:** focus a value (roller/dropdown/slider/day-matrix) with B2/B3,
press B4 to enter edit mode, adjust with B2/B3 (long-press jumps: ×5 in
dropdowns, ×10 in rollers, ±50 on sliders), B4 commits, B1 cancels.
Dropdown lists open as a full-screen overlay with the current choice centered.

## The clock screen

- Big **HH:MM** (or the active mode's figure). Top-left: satellite count.
  Top-right: USB (host owns the TUNES drive), a charge bolt while charging,
  a **drawn battery gauge** (white frame, fill width = charge level, colored
  red under 20% / orange under 50% / green above — no percent number; the
  estimate is approximate by nature, especially while charging), bell (an
  alarm is enabled). v1 boards show a full green battery when the supercaps
  are ready. Bottom: date and next alarm.
- **Low battery** (v2): under 3.40 V on battery (never during an alarm, never on a charger) the clock powers itself off; woken below 3.45 V without a charger it shows **LOW BATTERY** for 4 s and sleeps again. The RTC keeps time throughout.
- Display dims after the configured timeout — any button **or a small
  shake/tap** wakes it (LIS3DH motion).
- **Starry night** (Disp & sound): between **22:00 and 06:00** a couple dozen
  faintly twinkling stars appear behind the digits. Clock mode only.

## Menu

`B1` (or `B4`) on the clock screen opens it:

| Item | What it does |
|---|---|
| Alarm 1 / Alarm 2 | Enable switch, time rollers, weekday matrix (S M T W T F S — **no days selected = every day**), tune dropdown, **Ramp** (per-alarm gentle wake: Off/15/30/60 s), **Random** (the alarm fires at a random offset of ±1/±5/±9 min around the set time — a new roll every occurrence, and the display still shows the set time so you can't cheat), **Lights** (the LED show while ringing; off = sound-only alarm), Test, Save. *Back does not save.* |
| Time & zone | **Auto TZ (GNSS)** on/off; manual **Zone** = the full GMT ladder (GMT−12…+14 incl. half/quarter hours, fixed offsets, no DST); 24 h switch; **Sync now**. |
| Disp & sound | **Volume** (0–10, live while a tune plays), brightness slider (live), dim-after timeout, dim level, **Starry night** (stars behind the clock 22–06). |
| Tunes | Preview built-in melodies and WAVs from the TUNES drive (press again to stop). Unavailable while the USB host owns the drive. |
| System info | Fix/sats/HDOP, position, speed/altitude, zone + POSIX string, UTC offset/DST, GNSS sync age, RTC status, caps status, **snooze counters**, FW version. Scroll with B2/B3. |
| Battery | (v2 boards) Live power status: USB present, battery voltage + estimated SoC, charge state (trickle/CC/CV/charged — "battery assisting" means the load momentarily exceeds the 500 mA USB budget and the battery covers the difference), PMIC die temperature. Charging targets 400 mA on a 500 mA source and 800 mA on a detected 1.5/3 A USB-C source, to 4.10 V (the PMIC gives charging whatever remains of the budget, and pauses above 80 °C die temp until it cools to 70 °C). Live battery current (+ = charging, − = discharging) is shown on this screen. |
| Sky view | Live satellite map: left a **polar plot** (center = straight up, outer ring = horizon, N = north; dot color green/yellow/red/grey = signal strength, border white = GPS / cyan = GLONASS), right **SNR bars** for the strongest 12 with PRN numbers (65–96 = GLONASS). Updates every ~5 s — walk the clock around to find the best indoor reception spot. Works without a fix. |
| Lights | Manual on/off switch per LED section (Left 10 / Bottom 14 / Right 10). A power-measurement tool: flip a section, hop to Battery and read the current. State persists while navigating; an alarm's light show overrides it. |
| Tap snooze | Toggle double-tap-to-snooze. |
| Mode | Cycles the main screen: **Alarm clock → Speedometer → Altimeter → Game mode**. |
| Shutdown | (v2, battery only) Confirm with OK → the PMIC cuts the battery (<500 nA); the RTC keeps time. **B1 or a USB plug wakes it.** On USB power it asks you to unplug instead. The clock also does this by itself when the battery runs down to 3.40 V (never while an alarm rings/snoozes) — announced by a short high chirp and a light sweep right before it goes dark, so you notice a forgotten charger. Woken too early without a charger it shows LOW BATTERY and turns back off — plug in USB and it boots normally. |
| Back | To the clock. |

## Alarms

- On trigger: LED chase + the chosen tune (WAV, else built-in melody) —
  fading in over the alarm's own **Ramp** setting on the first ring (snooze
  re-rings and buzzer escalation play at full volume immediately). The light
  show is per-alarm: **Lights off** in the alarm editor gives a sound-only
  alarm. Tunes play at comparable loudness regardless of how the WAV was
  mastered (AGC).
  After the configured minutes unacknowledged, the power buzzer joins in.
  Auto-silence after 30 min; the alarm re-arms for its next day.
- **Snooze**: short-press any button, or **double-tap the clock body**
  (if Tap snooze is on; taps are ignored the first 2 s of each ring).
  Default 9 minutes, then it re-rings.
- **Stop**: long-press any button (also while snoozed).
- The **snooze shame counter** counts every snooze — this week and all time —
  shown on the ringing screen ("snooze (3x this week!)") and in System info.

## Modes

- **Speedometer / Altimeter**: the big figure shows GNSS ground speed (km/h)
  or altitude (m). `---` until there is a fix. Alarms still work.
- **Game mode**: the clock is a **USB gamepad** ("K. Haaland GNSS Alarm
  Clock"): tilt = stick X/Y (~20° for full deflection), B2–B4 = buttons 1–3,
  B1 = menu. Test on Linux with `jstest-gtk`. Alarms still work.

## Tunes (WAV upload)

Plug into a computer — the **TUNES** drive appears. Drop `.wav` files in the
root: PCM, 8- or 16-bit, mono or stereo, 8–48 kHz, filename ≤ 31 chars.
Select the tune in the alarm editor. If a chosen file is later deleted, the
alarm falls back to its built-in melody.

**Converting MP3s**: `python3 tools/tune_converter.py` (Linux/Windows,
needs ffmpeg on PATH) — opens a small GUI: load any audio file, drag across
the waveform to cut out the section you want, preview it, and it writes a
clock-ready WAV (16-bit mono, rate of your choice, peak-normalized,
filename-length guarded).

Playback high-passes WAVs at **200 Hz**: the small speaker can't reproduce
deep bass anyway — it only burns battery current (below the driver's
resonance the impedance is at its minimum while the cone barely moves), so
sub-bass is traded for louder, cleaner output in the audible band. Pick or
master alarm tunes for mid/high energy; sub-heavy tracks lose nothing you
would have heard.

## Time & timezone

Time comes from GNSS automatically (the RV-3028 RTC keeps it through power
loss). The timezone — including DST — is derived **offline** from your
position and remembered, so the clock is right indoors and after reboots.
If you ever want a fixed offset instead, switch Auto TZ off and pick from
the GMT ladder.

## Settings persistence

Everything you configure (alarms, zone, brightness, mode, counters) is
stored in the RTC's EEPROM and survives power loss **and firmware updates**.
