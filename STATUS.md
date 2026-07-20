# Project Status Report — GNSS Alarm Clock

_Last updated: 2026-07-05_

Custom **SAMD51J19A** board (Adafruit Metro M4 compatible). Firmware:
PlatformIO + Arduino + **LVGL 9.5**. This report captures the current state,
what changed most recently, and the open items — hardware and firmware.

---

## 1. Overall status

**Firmware is feature-complete and running on hardware.** Every subsystem is
implemented and exercised on the bench. The one blocker is a **hardware** fault
on the RTC (below); it does not stop the clock from working off GNSS.

| Subsystem | State | Notes |
|---|---|---|
| Display (NV3007) | ✅ Working | 428×142 landscape, vendor-page init, Y+14 offset, 24 MHz mode 0 |
| GNSS time (L86) | ✅ Working | RMC+GGA @1 Hz, cold-start year-2080 guard |
| Timezone + DST | ✅ Working | Offline coord→POSIX-TZ, persisted to flash |
| RTC (RV-3028) | 🔴 HW fault | Not ACKing on I²C — reflow U5 (see §3) |
| Alarms + snooze | ✅ Implemented | Verify ring/re-ring/tap on hardware (§4) |
| Audio (DAC→TPA2016) | ✅ Working | WAV + 3 melodies, digital + amp volume |
| LEDs (3 sections) | ✅ Working | Chase/blink from supercap rail |
| Tunes over USB | ✅ Working | QSPI flash as `TUNES` drive (16 MB) |
| Settings persistence | ✅ Working | RV-3028 user EEPROM (39 B packed) — survives reboot **and reflash** (verified) |
| UI (8 screens) | ✅ Working | 4-button nav, true-black theme |

---

## 2. Recent changes (this session)

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
writes linearly), GRAM 168×428 with Y+14 landscape offset, SPI mode 0 @
24 MHz, inversion off. Init cross-verified against five independent driver
codebases (researched via web). Old ST7789 driver kept, selectable via
DISPLAY SELECT in platformio.ini. UI scaled up for the doubled resolution
(fonts 12→16/14/20, wider sliders/dropdowns, taller day-matrix).

**100 px clock font:** generated a Montserrat-Medium subset (" -.0123456789:AEGM",
4bpp, Pillow from lvgl's bundled TTF, LVGL 9.5 fmt_txt layout) for the big
clock/ring figure on the 142 px panel. Montserrat-48 became unreferenced and
is linker-GC'd: flash 95.7% -> 82.9% (net -67 KB) while the clock doubled in
size. Generator approach documented in src/font_clock_100.c header.

**Docs:** README refreshed to current reality; this STATUS report added.

---

## 3. Open HARDWARE items

### 🔴 RV-3028 RTC (U5) not on the I²C bus — **root cause found: not populated!**
Bus scan (100 kHz) showed 0x18/0x50/0x58 answering and `0x52` silent — and the
bench inspection revealed why: **U5 was never soldered onto this prototype.**
Being hand-fitted now.

**After soldering:** power-cycle (the RTC is probed once at boot), then check
Menu → System info → should read **"RTC ok"** instead of "RTC MISSING". For a
live probe instead: set `I2C_SCAN 1` in main.cpp, reflash, watch USB serial
for `ACK 0x52`.

**Impact while absent:** clock works when GNSS has a fix; loses time on
power-off and can't set time indoors (that's the RTC's job).

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

## 3½. Hardware v2 (planned)

Next board revision, decided so far:
- **MCU: SAMD51J20A** (1 MB flash / 256 KB RAM) — build targets already in
  place (`metro_m4_j20`, `sim_j20`).
- **Li-ion battery replaces the supercaps** — needs USB charging, which is why
  the firmware now declares **MaxPower 500 mA** (done, verified).

Firmware consequences to prepare when the v2 schematic lands:
- Charger with **power-path/load-sharing** recommended (e.g. BQ24074/MCP73871
  class) so the system runs while charging within the USB budget.
- Replace `CAPGOOD` logic with charger **CHG/PGOOD** status inputs; battery
  icon/percentage in the UI (ADC divider or fuel gauge).
- `ALARMPOWER` (LEDs + buzzer + amp) moves to the battery rail — revisit
  escalation/brightness current budget.
- **RTC VBACKUP and L86 `V_BCKP` from the battery** — fixes HW-review #10
  (GNSS cold-start) and makes timekeeping holdover robust in one stroke.
- Low-battery behavior (dim/limit alarm? shutdown threshold) — TBD.
- A v2 **uf2-samdx1 bootloader** build (J20 + our pid.codes USB identity).

## 4. Open FIRMWARE items

**Verify on hardware (behavior already implemented):**
- Double-tap-to-snooze (LIS3DH, `CLICK_THRESHOLD=32` — tune on hardware).
- Value up/down editing in menus; alarm ring → snooze → re-ring cycle.
- `CAPGOOD` polarity (HIGH = supercaps charged).

**Polish / nice-to-have:**
- **Red gamma** — bright red renders slightly brown; ST7789 gamma table could be
  tuned (cosmetic; UI is white-on-black).
- **Faster flush** — a direct SERCOM data-register loop (or a proper DMAC IRQ
  handler) would take full-frame ~15 ms → ~8 ms.
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
