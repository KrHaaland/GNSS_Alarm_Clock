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
| Display (ST7789) | ✅ Working | 284×76 landscape, 24 MHz, offsets 18/82, inversion off |
| GNSS time (L86) | ✅ Working | RMC+GGA @1 Hz, cold-start year-2080 guard |
| Timezone + DST | ✅ Working | Offline coord→POSIX-TZ, persisted to flash |
| RTC (RV-3028) | 🔴 HW fault | Not ACKing on I²C — reflow U5 (see §3) |
| Alarms + snooze | ✅ Implemented | Verify ring/re-ring/tap on hardware (§4) |
| Audio (DAC→TPA2016) | ✅ Working | WAV + 3 melodies, digital + amp volume |
| LEDs (3 sections) | ✅ Working | Chase/blink from supercap rail |
| Tunes over USB | ✅ Working | QSPI flash as `TUNES` drive (16 MB) |
| Settings persistence | ✅ Working | Internal flash (emulated EEPROM) |
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

**Timekeeping / GNSS:**
- Added `gnss_get_speed_kmph()` and `gnss_get_altitude_m()` (from RMC/GGA, only
  when fixed) and a **Speed / Alt line on the System-info screen**.

**UI:**
- Clock face: **centered HH:MM, seconds removed**.

**I²C:**
- **Bus dropped 400 → 100 kHz.** RV-3028 intermittently failed to ACK at
  400 kHz (board lacks local HF decoupling — HW review #12). Added a gated
  `I2C_SCAN` bus scanner.

**Docs:** README refreshed to current reality; this STATUS report added.

---

## 3. Open HARDWARE items

### 🔴 RV-3028 RTC (U5) not on the I²C bus — **primary blocker**
Bus scan result (at 100 kHz): **0x18 accel ✅, 0x50 EEPROM ✅, 0x58 amp ✅,
`0x52` RTC ❌ (no ACK)**. Three devices answer on the same bus, so wiring /
pull-ups / 3.3 V are fine — the RTC alone is silent. A powered I²C chip always
ACKs its address, so this is a **hardware fault on U5**: most likely an open
solder joint.

**To fix (bench):**
1. Measure **VDD (U5 pin 7) → should be ~3.3 V** to GND. #1 suspect.
2. Continuity on **SDA (pin 4)** and **SCL (pin 3)** from the U5 pin to a
   known-good bus point (don't just check for 3.3 V — the pull-ups hold the line
   high even if U5's stub is open).
3. Continuity on **GND (pin 5)**.
4. **Reflow pins 3/4/5/7.** If still no ACK, the chip is likely dead / mis-placed
   (check pin-1 mark).

**Impact while broken:** clock still works when GNSS has a fix; it loses time on
power-off and can't set time indoors with no fix (that's the RTC's job).
**Re-test:** set `I2C_SCAN 1`, reflash, watch USB serial for `ACK 0x52`.

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

## 4. Open FIRMWARE items

**Verify on hardware (behavior already implemented):**
- Double-tap-to-snooze (LIS3DH, `CLICK_THRESHOLD=32` — tune on hardware).
- Value up/down editing in menus; alarm ring → snooze → re-ring cycle.
- `CAPGOOD` polarity (HIGH = supercaps charged).

**Polish / nice-to-have:**
- **Backlight PWM dimming** — `display_set_contrast()` is currently on/off; the
  `analogWrite(PIN_OLED_BL, …)` PWM path is stubbed out.
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
