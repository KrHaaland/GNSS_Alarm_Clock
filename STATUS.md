# Project Status Report — GNSS Alarm Clock

_Last updated: 2026-08-02 — **release [v1.0.0](https://github.com/KrHaaland/GNSS_Alarm_Clock/releases/tag/v1.0.0) published** (bin + UF2 for both boards)_

Custom **SAMD51** board (Adafruit Metro M4 compatible) — the **v2 board
(SAMD51J20A, nPM1300 PMIC, Li-ion, USB-C) is now the primary target**; the
v1 prototype (J19A, supercaps) remains supported. Firmware:
PlatformIO + Arduino + **LVGL 9.5**. This report captures the current state,
what changed most recently, and the open items — hardware and firmware.

---

## 1. Overall status

**Firmware is feature-complete — release v1.0.0 — and running on the v2
board.** Every subsystem is implemented and bench-verified. Open hardware
items are parts in the mail (L86 module, 3 V QSPI flash, pull-down display);
none stop daily use — the clock runs off the battery-backed RTC.

| Subsystem | State | Notes |
|---|---|---|
| Display (NV3007) | ✅ Working | 428×142 landscape, vendor-page init, Y+14 offset, 30 MHz mode 0 |
| GNSS time (L86) | 🔴 Module dying | Works only when heated (4-5 proto cycles); replacement ordered. FW verified OK |
| Power/charging (v2) | ✅ Working | CC-based 500/1500 mA budget, 400/800 mA charge → 4.10 V, 80/70 °C thermostat, ship-mode low-batt policy (ADR-0014/0016) |
| Timezone + DST | ✅ Working | Offline coord→POSIX-TZ, persisted to flash. J20: fine polygon map (404 zones, ~1–2 km borders); J19: coarse |
| RTC (RV-3028) | ✅ Working | U5 hand-soldered onto the prototype; ACKs at 0x52, settings EEPROM verified. Backup switchover (LSM) EEPROM-verified at every boot — factory-fresh chips ship with it OFF |
| Alarms + snooze | ✅ Implemented | Verify ring/re-ring/tap on hardware (§4) |
| Audio (DAC→TPA2016) | ✅ Working | WAV + 3 melodies, digital + amp volume |
| LEDs (3 sections) | ✅ Working | Chase/blink; v2: +5 V boost rail (battery-capable), v1: supercap rail |
| Tunes over USB | ⏳ Awaiting part | v2's U8 is a 1.8 V W25Q128FW (wrong variant) — storage disabled until a JVSIQ is fitted; melodies work |
| Settings persistence | ✅ Working | RV-3028 user EEPROM (38 B packed, v4) — survives reboot, reflash **and format upgrades** (in-place migration, verified) |
| UI (9 screens) | ✅ Working | 4-button nav, true-black theme, starry-night option |

---

## 2. Recent changes

**Power budget, RTC backup & bootloader hardening (2026-08-03 → 08-09):**
- **IBATLIM is the system's power wall**: the nPM1300 limits battery
  discharge to 1000 mA — exceeding it collapses VSYS below VSYSPOF and
  resets the device (bench-confirmed: full volume + all LEDs cut ~30 s in
  as the amp's AGC wound up, even at 4.1 V). Volume ceiling re-tuned to
  +7 dBV (~500–900 mA worst case measured); a USB-only higher ceiling was
  rejected — IBATLIM applies instantly at unplug, software can't win that
  race (ADR-0010).
- **Dev telemetry**: live `V/mA` readout on the clock face + ringing screen
  (`UI_DEV_IBAT` in Ui.cpp, 1 Hz) — bench tool for the power work above.
- **200 Hz high-pass in the WAV path** (2nd-order Butterworth, per-tune
  sample-rate aware): deep bass drew the most current (speaker impedance
  minimum) for the least sound — trimming it cuts the sustained peaks that
  threaten IBATLIM and lets the AGC spend the budget on audible content.
  Bench-verified with a log-sweep WAV (`tools/gen_sweep.py`) + the mA
  readout; this board's actual IBATLIM trips somewhere above the 1120 mA
  measurement ceiling (spec says 1000 — margin exists but is unstamped).
- **RTC backup switchover now EEPROM-verified at boot** after a flat-battery
  incident reset the RTC: factory-fresh RV-3028s ship with switchover OFF;
  read-back + rewrite until BSM=LSM verifies, PORF surfaced on System info.
- **Bootloader zeroed by a brownout**: repeated VSYSPOF resets during the
  power bench work left one wild NVMCTRL quad-word write at address 0
  (NVMCTRL.ADDR resets to 0) — the bootloader's vector table was zeroed,
  bricking boot (double-fault lockup). Restored via SWD; **BOOTPROT fuse
  now set (16 KB)** so flash hardware refuses such writes.
- **USB flashing repaired**: the stock J19A bootloader reads the dbl-tap
  magic at a different RAM top than the J20 app writes it; the build now
  patches `initiateReset()` to write both — `pio run -t upload` works over
  USB (ADR-0015 update).
- **Settings format v4**: snoozeTotal u32→u24 + spare byte dropped (40→38 B);
  old blocks migrate in place at boot, nothing resets.
- Display flipped 180° (`MADCTL 0xA0`, Y-offset 12) to match the enclosure.
- **Low-battery farewell chirp**: the automatic 3.40 V shutdown announces
  itself — 0.5 s 2200 Hz beep (fixed moderate volume, independent of the
  alarm volume) + a sequential LED sweep — so a forgotten charger is
  noticed. Manual Shutdown stays silent on purpose.
- **Charger-vs-shutdown races fixed**: the nPM1300 refuses ship-mode entry
  with VBUS present — pmic_enter_ship_mode() used to spin forever (frozen
  screen when a charger was plugged during the LOW BATTERY boot gate); it
  now reboots into a normal charging boot after 300 ms. The boot gate also
  polls VBUS during its 4 s message and continues booting if a charger
  appears. Known remaining gap: a 10 s SHPHLD reset WITH USB attached
  power-cycles the PMIC back to its 100 mA default and brownout-loops in
  the bootloader window (amp noise) — bench workaround: unplug first; real
  fix is the planned custom bootloader with an early ILIM write.
- **Ringing screen**: ALARM 1/2 title lifted above the big digits' opaque
  background (same z-order fix as the clock-face symbols).
- **Per-alarm Lights switch** (alarm editor): off = sound-only alarm — no
  LED chase/blink while ringing. Stored in a free alarm-flag bit
  (inverted; old settings read back as on, no migration needed). Also
  saves the chase's ~0.2-0.3 A on battery alarms.

**Power management completed (2026-08-01 → 08-02)** — ADR-0014/0016:
- **CC-following input limit** (500 mA PC / 1500 mA charger) re-applied
  instantly on replug via the nPM1300's GPIO0 IRQ (wired to PA04) — fixed
  a real bug where a replug left the limit at 100 mA, draining the battery
  in the charger.
- **Charging**: setpoint follows the budget (400/800 mA), 4.10 V
  termination, die thermostat bench-tuned 55/45 → **80/70 °C** (measured:
  800 mA continuous, die ~75 °C cold-board / near 80 heat-soaked; thermal
  vias spread ~1 W across the ground plane). VDDCORE moved to the MCU's
  internal buck (~4–6 mA saved).
- **Battery telemetry**: IBAT measurement on the Battery screen (signed
  current = the bench multimeter on-screen); Lights menu (per-section LED
  switches) as its power-measurement companion.
- **Low-battery policy**: ship mode below 3.40 V on battery (paused while
  an alarm rings/snoozes; the ring beats the last percent), fullscreen
  icon + Montserrat-48 "LOW BATTERY" on a too-early wake (J20-only — it
  overflowed the J19), manual Shutdown menu item; BUTTON1/USB wakes, the
  battery-backed RTC keeps time. Field-proven on a genuinely empty cell.
- **SoC**: IR-compensated voltage estimate (160 mΩ path calibrated from
  the observed plug-in jump); a coulomb-counting tracker was tried and
  deliberately reverted (guesses beat by measurements — ADR-0016). Clock
  face shows a drawn battery gauge (red/orange/green fill), no percent.
- Battery (bring-up): LG HG2 18650 3000 mAh unprotected power cell — the
  firmware's cutoff was the protection layer. **Replaced 2026-08-10 by the
  production cell: generic 505060 LiPo, 2000 mAh, with PCM** (firmware's
  3.40 V cutoff still acts first). Higher source impedance than the HG2 —
  SoC path-resistance re-calibration + worst-case audio re-test pending.

**v2 board bring-up (2026-07-22 → 08-01)** — full session log in
`docs/sessions/`. Every subsystem verified on the new board:
- **MCU is a SAMD51J20A** (verified `bossac -i`; schematic symbol says J19A)
  → `metro_m4_j20` is the default build (ADR-0015). Flash 42 %, RAM 60 %.
- **nPM1300 power** (ADR-0014): VBUS limit raised to 500 mA at every boot
  (100 mA OTP default brownout-looped the LED test on a battery-less
  board), charger enabled at 200 mA → 4.10 V, boot-window load clamping
  (L86 reset, amp off, backlight low + soft-start ramp).
- **Battery UI**: SoC% + level-mirroring battery icon + charge bolt on the
  clock face; "Battery" menu screen with live VBAT/state/die-temp.
- **VDDCORE on the internal buck** (`SUPC->VREG.SEL=1`, board has the L4
  inductor): ~4-6 mA saved continuously vs the core's default LDO.
- **Bench findings, all diagnosed on-board**: LIS3DH cold joint (answers at
  0x19 — SA0 strap not effective; FW probes both addresses), L86 cold VCC
  joint (reflowed; **module now failing after 4-5 prototype cycles — trickle
  of NMEA unless heated; replacement ordered**), U8 flash is a 1.8 V
  W25Q128FW (wrong variant, storage disabled until a JVSIQ arrives),
  display module straps BL with a pull-up (full backlight through the
  bootloader phase → battery-less bootloop; replacement display has a
  pull-down), one aborted bossac write left a half-flashed app that
  enumerated but "saw no satellites" — SWD via Atmel-ICE is now the primary
  flash path (ADR-0015).
- **ledtest diag env**: I2C scan + WHO_AM_I, LED sections, GNSS listener
  with reset-pulse aliveness test, QSPI JEDEC probe, nPM1300 unlock.
- Earlier in the window: async DMA display flush (ADR-0012), DMA-paced
  audio (ADR-0013), Sky view (GSV), starry night, Tunes-freeze fix
  (LVGL pool 80 K).

## 2b. Older changes (v1 era)

**Display bring-up (SH1122 → ST7789)** — committed:
- New `DisplayST7789` driver (RGB565, LVGL 9). Panel is a centered sub-window of
  the 320×240 GRAM → window offsets **`ST_X_OFFSET=18` / `ST_Y_OFFSET=82`**.
- **Inversion off** (`ST_INVERSION=0`) — this panel inverts colors with INVON.
- **SPI raised 4 → 24 MHz** — full-frame flush ~86 ms → ~15 ms; big UI smoothness
  win. Synchronous per-byte flush kept (DMA path freezes the MCU via an
  unhandled `DMAC_1` IRQ).
- Gated `DISPLAY_SELFTEST` bring-up tool retained (default 0).
- **Backlight PWM dimming** — `display_set_contrast()` now uses `analogWrite` on
  D11/PA19 (TCC1_CH3), so the brightness slider + dim-after timeout actually dim
  the panel (was on/off before).

**Timekeeping / GNSS:**
- Added `gnss_get_speed_kmph()` and `gnss_get_altitude_m()` (from RMC/GGA, only
  when fixed) and a **Speed / Alt line on the System-info screen**.
- **Fine-grained timezone table** — expanded `kZones[]` from ~32 to **213
  boxes** covering every UTC zone worldwide: multi-zone countries split
  (US/Canada/Mexico/Russia/Brazil/Australia/China-single…) and no-DST
  exceptions (Arizona, Saskatchewan, Queensland/NT/WA, Kaliningrad, Sonora,
  Quintana Roo, Crimea, Easter I., Lord Howe…), with current 2025/26 DST rules
  (Brazil/Mexico/Iran/Turkey no DST, Egypt DST back, etc.). Generated + verified
  region-by-region. A **100+ city coverage audit** then caught and fixed 5
  errors: broad boxes overreaching neighbors (India→Afghanistan, Myanmar→
  Thailand, Egypt→Jordan — re-aligned to real borders) and two missing
  territories (added America/Nuuk, Atlantic/Cape_Verde). Host-tested: 19 cases
  incl. a 13-city offset/DST check + an 8-case border regression guard. Anything
  not in a box falls to a whole-hour longitude estimate, so every coordinate
  resolves. Flash 86.6% → 88.0%.

**UI:**
- Clock face: **centered HH:MM, seconds removed**.
- **Mode selector** (Menu → Mode, cycles in place; settings **v2** — stored
  settings reset once on first boot): Alarm clock / **Speedometer** (GNSS
  km/h as the big figure) / **Altimeter** (GNSS m) / **Game mode** (LIS3DH as
  a USB HID gamepad: tilt = stick, B2–B4 = buttons, B1 = menu; HID interface
  always enumerated, silent outside the mode). Alarms fire in every mode.
  Sim console gained `mov <kmh> [altM]` to exercise the GNSS modes indoors.
- **Shake / tap to wake** — new `accel_moved()` (motion delta on the LIS3DH)
  restores full brightness / resets the dim timer, like a button press.
  Sensitivity `MOVE_THRESHOLD` (~0.6 g) is tunable on the bench.

**I²C:**
- **Bus dropped 400 → 100 kHz.** RV-3028 intermittently failed to ACK at
  400 kHz (board lacks local HF decoupling — HW review #12). Added a gated
  `I2C_SCAN` bus scanner.

**Build targets:** added **SAMD51J20A** envs for the next board revision
(`metro_m4_j20`, `sim_j20`; custom `boards/samd51j20a_metro.json` +
`ld/samd51j20a_flash_with_bootloader.ld`, 1 MB/256 KB). Same firmware: J19
95.2% flash → J20 47.6%. Select via `default_envs` in platformio.ini. J20
must be SWD-flashed until a J20 UF2 bootloader is built.

**Settings storage moved to the RV-3028 user EEPROM** (packed 39-byte image;
coordinates instead of TZ strings, tune-name hashes matched against the TUNES
dir at boot; memory map in README). Survives firmware reflashes — the old
FlashStorage emulation sat inside the app image and was wiped on every update
— and has 4x the write endurance. FlashStorage_SAMD dropped (flash 95.6% ->
93.3%); the 24LC512 is now unused and **leaves the BOM in hardware v2**.
Bring-up found two RV-3028 quirks (chip NACKs I2C during EEPROM busy at POR
and in windows after each programmed byte) — fixed with tolerant busy-wait +
per-byte retry & read-back verify. **User-verified: settings survive both
reboot and reflash.**

**Amp (TPA2016) rework:** AGC 1:4 loudness leveling (all tunes comparable),
volume 0–10 maps to the output limiter across its full range — v1 = −6.5 dBV
(~28 mW) … v10 = +9 dBV (~1.0 W into the 8 Ω speaker, chip max). **Gentle-wake
ramp is per alarm** (alarm editor "Ramp": Off/15/30/60 s — first ring only;
snooze re-rings and buzzer escalation at full volume; packed into spare alarm
flag bits, EEPROM v3). New **Volume slider** on "Disp & sound" (the setting
previously had no UI control). Amp fault/thermal status on System info.

**Random alarm trigger:** per-alarm "Random" (Off/±1/±5/±9 min) — each
occurrence fires at a fresh hardware-TRNG offset around the set time (absolute
-minute targeting, so windows cross hour/day boundaries). Display keeps
showing the nominal time. Packed into spare alarm-flag bits (no EEPROM cost).

**Display swapped to NV3007 (428×142):** new driver (vendor-page init is
mandatory — without the 0xFF 0xA5 register dump the address engine wraps
writes linearly), GRAM 168×428 with Y+14 landscape offset, SPI mode 0,
inversion off. Init cross-verified against five independent driver
codebases (researched via web). Old ST7789 driver kept, selectable via
DISPLAY SELECT in platformio.ini. UI scaled up for the doubled resolution
(fonts 12→16/14/20, wider sliders/dropdowns, taller day-matrix).

**100 px clock font:** generated a Montserrat-Medium subset (" -.0123456789:AEGM",
4bpp, Pillow from lvgl's bundled TTF, LVGL 9.5 fmt_txt layout) for the big
clock/ring figure on the 142 px panel. Montserrat-48 became unreferenced and
is linker-GC'd: flash 95.7% -> 82.9% (net -67 KB) while the clock doubled in
size. Generator approach documented in src/font_clock_100.c header.

**Display speed (NV3007):** SPI 24 → **30 MHz** (SERCOM2 re-clocked from the
120 MHz GCLK0 — the default 48 MHz source can't divide above 24) and the pixel
stream now goes through a **DRE-paced bulk loop** writing straight into the
double-buffered SERCOM data register (back-to-back SCK instead of a per-byte
`SPI.transfer()` RX round-trip). Full-frame flush ~90 → ~26 ms; user-verified
"veldig mye bedre". Follow-ups: LVGL refresh period 30 → 15 ms and render
buffer 24 → 32 rows.

**Async DMA flush (ADR-0012, supersedes 0002):** dedicated DMAC channel
(Adafruit_ZeroDMA — installs the IRQ handlers whose absence caused the 0002
freeze) + **two** 32-row LVGL buffers (RAM 63 %), so rendering overlaps the
transfer and the main loop no longer stalls during flushes. Synchronous
DRE-loop kept as automatic fallback. User-verified on hardware: "displayet
er smooth!"

**DMA-paced audio (ADR-0013):** the per-sample TC2 ISR (22–48 k IRQ/s) is
gone — TC2 overflow now triggers one DMAC beat per sample from a two-half
ping-pong buffer into DAC0, with one IRQ per 2048-sample half. Underrun
guard silences a stale half; per-sample ISR kept as fallback. Verified:
WAV/melodies play cleanly while scrolling the UI — pixels and samples
stream concurrently on two DMAC channels.

**Sky view (GSV):** new screen — polar az/elev plot + SNR bars of every
satellite in view, colored by signal and constellation (GPS/GLONASS). The L86
now emits GSV every 5th fix (PMTK314; full-rate GSV would crowd the 9600-baud
link) and a small in-house parser runs beside TinyGPS++ (which lacks GSV).
Works without a fix — this is the indoor antenna-placement aid.

**Starry night:** optional (Disp & sound) — 26 twinkling stars behind the
clock digits between 22:00 and 06:00, clock mode only. Stored in a spare
flags bit (b6), no EEPROM format bump. Also: AM/PM and unit tags now sit on
the big digits' baseline.

**Post-mortem:** the sky screen's ~50 LVGL objects exhausted the 48 KB LVGL
pool at boot — LV_ASSERT_MALLOC hangs in a loop with a black panel (USB alive
via IRQs, hence the confusing symptom). Pool now 64 KB, RAM 71.7 %.

**Docs:** README refreshed to current reality; this STATUS report added.

---

## 3. Open HARDWARE items

### ✅ RV-3028 RTC (U5) — RESOLVED (was: not populated)
Bus scan showed `0x52` silent because **U5 was never soldered onto this
prototype**. Hand-fitted on the bench; now ACKs at 0x52, the settings
EEPROM survives both reboot and reflash, and **holdover is verified**
(2026-07-21): power-loss → correct time back via the supercaps. The full
RTC chain — timekeeping, backup power, settings storage — works.

### L86 GNSS signals unrouted (new findings)
- **1PPS (L86 pin 6) is a dead-end net** — no hardware pulse-per-second to the
  MCU. This is the most valuable signal for a *GNSS clock* (sub-µs timing vs our
  ~1 s NMEA timestamp). Would need a bodge wire to a spare GPIO.
- **AADET_N (L86 pin 8) unrouted** — no active-antenna presence/short detection.

### Existing HARDWARE_REVIEW findings (unchanged)
See [`HARDWARE_REVIEW.md`](HARDWARE_REVIEW.md). Most relevant:
- 🔴 **#1 LM3671 (U14) VIN/SW swapped** in symbol+copper — *verify on bench*
  (board does run, so measure the 3.3 V rail / U14 temp before acting).
- 🟠 **#10 L86 V_BCKP on main 3.3 V** → full cold start every boot (root cause of
  the "year-2080/no-fix" behavior). Route to the supercap backup rail.
- 🟠 **#8 RV-3028 ~INT unrouted** → alarm/wake only by polling (can't sleep).
- 🟠 D3 flyback diode reversed; C30 VDDCORE cap to +3.3 V not GND; crystal load
  caps C24/C25 10 pF (want ~18–22 pF); USB ESD array U2 ground; amp INR+ /
  PVCC decoupling.

---

## 3½. Hardware v2 — status (built and running!)

The v2 board is assembled and every subsystem is bench-verified (see
`HARDWARE_V2.md` for the full firmware-facing reference). Realized:
SAMD51J20A, nPM1300 (charger 400/800 mA → 4.10 V, power path, USB-C CC),
LM3671 3.3 V + TPS61023 5 V boost (both battery-capable), battery-backed
RTC, battery icon/SoC in the UI. The escalation buzzer was dropped in
hardware (PB16 unconnected).

**Waiting on parts:**
- **L86 GNSS module** — the veteran module (4-5 prototype cycles) now only
  talks when heated; replacement ordered.
- **W25Q128JVSIQ (3 V)** — fitted U8 is the 1.8 V FW variant; storage/TUNES
  disabled until swapped. FW supports the JVSIQ out of the box.
- **Display with BL pull-down** — current module's pull-up burns the
  backlight through the bootloader phase (battery-less bootloop).

**Still open (firmware):**
- Custom **uf2-samdx1 bootloader**: pid.codes identity + early nPM1300
  ILIM write (kills the last battery-less-boot window, ADR-0014/0015).
- Escalation stage on v2: gate the (nonexistent) buzzer on
  `pmic_present()`, or substitute max-volume + LED blitz.
- L86 V_BCKP still on 3.3 V (not battery): GNSS cold-starts after power
  loss — next board spin, along with an L86 load switch and the U8/U18
  schematic symbol tidy-up.

## 4. Open FIRMWARE items

**Verify on hardware (behavior already implemented):**
- Double-tap-to-snooze (LIS3DH, `CLICK_THRESHOLD=32` — tune on hardware).
- Value up/down editing in menus; alarm ring → snooze → re-ring cycle.
- `CAPGOOD` polarity (HIGH = supercaps charged).

**Polish / nice-to-have:**
- **Red gamma** — bright red renders slightly brown; ST7789 gamma table could be
  tuned (cosmetic; UI is white-on-black).
- ~~Async flush (DMA)~~ — **done** (ADR-0012): DMA + double buffering,
  verified on hardware. The display path is now wire-bound; the only lever
  left is a higher SCK (50 MHz — above the NV3007's ~40 MHz ceiling, revisit
  only on the v2 PCB with proper routing).
- **GSV sky-view** — enable GSV + parse per-satellite SNR for an indoor
  reception/antenna-placement screen (high value given indoor use).
- **Escalation timer** restarts on each snooze re-ring (`s_ringStartMs` reset) —
  confirm that's the intended behavior vs. total-elapsed.

**Code hygiene (non-blocking):**
- Stale header comments still say "SH1122" in `main.cpp` / `Ui.cpp` /
  `platformio.ini` (functionally ST7789). *(Being cleaned up.)*
- `TuneStorage.cpp` includes `ff.c` via a fragile relative path into
  `.pio/libdeps/…` — breaks if the lib layout / env name changes.
- Settings have no migration path — a version/magic bump silently resets to
  defaults.

---

## 5. Build / flash quick reference

- Build: `pio run` (default env `adafruit_metro_m4`).
- Tests: `pio test -e native` (timezone/DST engine).
- Flash (SWD/Atmel-ICE): write **`firmware.bin` at `0x4000`** via OpenOCD —
  **never the `.elf`** (overwrites the UF2 bootloader). See [README](README.md).
- Flash (normal): double-tap reset → drag `firmware.uf2` to `METROM4BOOT`.
- Diagnostics: `DISPLAY_SELFTEST` / `I2C_SCAN` (`#define … 1`, rebuild).

**Toolchain pins that matter:** LVGL 9.5.0; Adafruit TinyUSB **2.4.1**
(do not bump — the core bundles 3.1.0; 2.4.1 is the highest that enumerates);
Adafruit SPIFlash 5.1.1; SdFat-Adafruit 2.3.103; TinyGPS++ 1.0.3;
FlashStorage_SAMD 1.3.2; RV-3028-C7 lib 2.1.0.
