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
  Top-right: USB (host owns the TUNES drive), power-caps ready, bell (an
  alarm is enabled). Bottom: date and next alarm.
- Display dims after the configured timeout — any button **or a small
  shake/tap** wakes it (LIS3DH motion).

## Menu

`B1` (or `B4`) on the clock screen opens it:

| Item | What it does |
|---|---|
| Alarm 1 / Alarm 2 | Enable switch, time rollers, weekday matrix (S M T W T F S — **no days selected = every day**), tune dropdown, **Ramp** (per-alarm gentle wake: Off/15/30/60 s), **Random** (the alarm fires at a random offset of ±1/±5/±9 min around the set time — a new roll every occurrence, and the display still shows the set time so you can't cheat), Test, Save. *Back does not save.* |
| Time & zone | **Auto TZ (GNSS)** on/off; manual **Zone** = the full GMT ladder (GMT−12…+14 incl. half/quarter hours, fixed offsets, no DST); 24 h switch; **Sync now**. |
| Disp & sound | **Volume** (0–10, live while a tune plays), brightness slider (live), dim-after timeout, dim level. |
| Tunes | Preview built-in melodies and WAVs from the TUNES drive (press again to stop). Unavailable while the USB host owns the drive. |
| System info | Fix/sats/HDOP, position, speed/altitude, zone + POSIX string, UTC offset/DST, GNSS sync age, RTC status, caps status, **snooze counters**, FW version. Scroll with B2/B3. |
| Tap snooze | Toggle double-tap-to-snooze. |
| Mode | Cycles the main screen: **Alarm clock → Speedometer → Altimeter → Game mode**. |
| Back | To the clock. |

## Alarms

- On trigger: LED chase + the chosen tune (WAV, else built-in melody) —
  fading in over the alarm's own **Ramp** setting on the first ring (snooze
  re-rings and buzzer escalation play at full volume immediately). Tunes play
  at comparable loudness regardless of how the WAV was mastered (AGC).
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

## Time & timezone

Time comes from GNSS automatically (the RV-3028 RTC keeps it through power
loss). The timezone — including DST — is derived **offline** from your
position and remembered, so the clock is right indoors and after reboots.
If you ever want a fixed offset instead, switch Auto TZ off and pick from
the GMT ladder.

## Settings persistence

Everything you configure (alarms, zone, brightness, mode, counters) is
stored in the RTC's EEPROM and survives power loss **and firmware updates**.
