# GNSS Alarm Clock — Firmware

Firmware for the custom **SAMD51J19A** alarm-clock board (Adafruit Metro M4
compatible), designed in `HARDWARE/samd51_gps_alarm_clock` (KiCad).

Time is set automatically from GNSS, the timezone (with DST) is derived offline
from the GNSS coordinates and saved to flash so it stays correct indoors, and
alarms drive an LED light-show + a tune through a class-D amp. The UI is LVGL on
a color TFT driven by four buttons.

> **Current state:** firmware is feature-complete and running on hardware —
> display, GNSS time/timezone, alarms, audio, tunes-over-USB, settings, modes
> (speedometer/altimeter/USB-gamepad) and the RV-3028 RTC (hand-fitted; it was
> missing from the prototype) all work. See [`STATUS.md`](STATUS.md) and
> [`HARDWARE_REVIEW.md`](HARDWARE_REVIEW.md).

## What it does

- **Time**: The Quectel **L86** GNSS receiver (Serial1, 9600 NMEA, RMC+GGA @1 Hz,
  TinyGPS++) provides UTC. The clock is a `millis()`-anchored UTC counter
  disciplined by GNSS (re-syncs hourly / on ≥2 s drift) and backed by the
  **RV-3028-C7** RTC (I²C 0x52), whose VBACKUP sits on the supercap rail so it
  holds time through power loss. On boot the clock starts from the RTC
  immediately and refines from GNSS when a fix is available. A cold-start guard
  rejects the L86's free-running "year-2080" date so garbage can't be written
  into the backup RTC.
- **Timezone from coordinates**: An embedded **offline** lookup (213-box table
  covering every UTC zone worldwide — multi-zone countries split by region, plus
  no-DST exceptions like Arizona / Saskatchewan / Queensland / Kaliningrad;
  borders like US-zones and Afghanistan/Pakistan/India aligned to the real
  meridians). **Polygon zones** (`src/TimezonePolyData.h`, ~357 zones /
  ~500 polygons) are checked before the boxes: real IANA timezone borders
  generated from timezone-boundary-builder data by `tools/gen_tz_polys.py`
  (tunable resolution/flash budget; enclaves ordered first), refinable by hand
  in `tools/tz_polygon_editor.html` (map GUI: draw/edit vertices, probe a
  point, export the header). Anything not inside a polygon or box falls back
  to a whole-hour longitude estimate, so **every coordinate resolves**. Maps
  the GNSS position to a
  **POSIX TZ string with DST rules** (e.g. Oslo →
  `CET-1CEST,M3.5.0,M10.5.0/3`). The result is **persisted to internal flash**,
  so local time (including DST changes) stays correct after reboot even with no
  fix indoors. A DST-aware POSIX evaluator (`Timezone.*`, host-unit-tested)
  computes the offset. Manual zone override is available in the menu.
- **Alarms**: Two independent alarms with per-weekday masks. On trigger:
  - the **34 LEDs** (3 sections: left 10 / bottom 14 / right 10) run a chase at
    full brightness from the LTC3226 supercap rail (so brightness doesn't depend
    on the USB supply),
  - a tune plays: a **WAV** from the QSPI flash drive, or one of 3 built-in
    melodies, via **DAC0 → TPA2016D2** class-D amp → speaker,
  - optional **escalation**: after *N* minutes unacknowledged, the power buzzer
    joins in; auto-silence after 15 min.
  - **Snooze**: short-press any button, or double-tap the clock (LIS3DH).
    **Stop**: long-press any button.
- **UI**: **LVGL 9.5** on an **ST7789 color TFT** (see below), driven by the 4
  buttons above the display: `[Back/Menu] [Prev/–] [Next/+] [OK]`. Screens:
  clock, menu, alarm-edit, time & zone, display, tunes, system-info, ringing.
  A true-black theme keeps the panel dark.
- **Modes** (Menu → Mode, cycles in place; alarms keep working in all of them):
  - **Alarm clock** — the normal HH:MM face.
  - **Speedometer** — the big figure shows GNSS ground speed in **km/h**.
  - **Altimeter** — GNSS altitude in **m** (MSL).
  - **Game mode** — the LIS3DH becomes a **USB HID gamepad**: tilt = stick
    X/Y, B2–B4 = buttons 1–3 (B1 keeps its menu role). The HID interface is
    always in the descriptor; it only sends reports in this mode.
- **Tune upload**: the QSPI flash appears as a **USB flash drive** (`TUNES`) —
  drag & drop `.wav` files (PCM, 8/16-bit, mono/stereo, 8–48 kHz).

## Display (ST7789)

The original SH1122 grayscale OLED was replaced (July 2026) with an **ST7789
color TFT**. Driver: [`src/DisplayST7789.cpp`](src/DisplayST7789.cpp).

- Native panel is 76×284 portrait; driven **landscape 284×76** via `MADCTL 0x60`.
- The glass is a **centered sub-window** of the controller's 320×240 GRAM, so the
  driver applies window offsets **`ST_X_OFFSET=18`, `ST_Y_OFFSET=82`**
  (= (320−284)/2 and (240−76)/2). If the panel/wiring changes, re-verify with the
  built-in border self-test.
- **Inversion OFF** (`ST_INVERSION=0`) — this panel shows inverted colors with
  INVON (black→white, green→magenta).
- **SPI 24 MHz**, MSBFIRST, `SPI_MODE3`, on SERCOM2. Step down to 16/12 MHz if
  garbage/flicker ever appears over jumper wiring.
- Flush is **synchronous per-byte** (LVGL RGB565 → byte-swapped → streamed). DMA
  is deliberately avoided: the 4-arg DMA `SPI.transfer` fires an unhandled
  `DMAC_1` IRQ that freezes the MCU. Full-frame flush ≈ 15 ms.
- **Backlight (BLK)** on **D11 / PA19**, **PWM-dimmed** via `analogWrite`
  (TCC1_CH3) — the brightness slider and "dim after" timeout control the level.
  *Note:* J3 has no backlight line (HW review #14) — BLK is wired separately.
- **Shake / tap to wake:** motion on the LIS3DH accelerometer restores full
  brightness and resets the dim timer, just like a button press.
- **Bring-up aid:** compile-time `DISPLAY_SELFTEST` (default 0) draws RGB fills +
  an edge border instead of the UI, to re-verify SPI / offsets / orientation.

## Storage decision

The board has two candidate "EEPROMs" for tunes: the 24LC512 (64 KB, I²C) and a
QSPI serial flash. The firmware uses the **QSPI flash** (exposed as the USB
drive): far larger, faster, on the standard Metro M4 QSPI pins, and it enables
the drag-and-drop workflow.

**Settings** are packed into the **RV-3028 RTC's 43-byte user EEPROM**.
Chosen because it survives **firmware reflashes** (the old internal-flash
emulation sat inside the app image and was wiped on every update), takes
~100k writes/byte, and frees the **24LC512 to be dropped from the hardware-v2
BOM** (it still ACKs at 0x50 on v1 boards, unused). Verified on hardware:
settings survive both power cycles and reflashes.

### RTC user-EEPROM memory map (39 of 43 bytes)

Space is won by not storing derived data: TZ strings re-derive at boot from
the stored position (or the manual GMT-ladder index), and alarm tune filenames
are stored as 16-bit FNV-1a hashes re-matched against the TUNES directory
(missing file → builtin melody). Multi-byte fields are little-endian.

| Addr | Size | Field |
|---|---|---|
| `0x00` | 2 | Magic `'G' 'C'` |
| `0x02` | 1 | Pack-format version (1) |
| `0x03` | 1 | Flags: b0 tzAuto, b1 tapSnooze, b2 use24h, b3 havePosition, b4–5 mode |
| `0x04` | 2 | Latitude, centidegrees (i16) |
| `0x06` | 2 | Longitude, centidegrees (i16) |
| `0x08` | 1 | Manual zone index into `TZ_TABLE` (`0xFF` = none/auto) |
| `0x09` | 1 | Volume (0–10) |
| `0x0A` | 1 | Snooze minutes |
| `0x0B` | 1 | Buzzer escalation after N min (0 = off) |
| `0x0C` | 1 | Brightness |
| `0x0D` | 2 | Dim timeout, seconds (u16) |
| `0x0F` | 1 | Dim brightness |
| `0x10` | 4 | Snooze counter, all time (u32) |
| `0x14` | 2 | Snooze counter, this week (u16) |
| `0x16` | 2 | Week start, local epoch-day − 18262 (u16, base 2020-01-01) |
| `0x18` | 7 | Alarm 1: flags (b0 enabled), hour, minute, daysMask, melodyId, tuneHash (u16) |
| `0x1F` | 7 | Alarm 2: same layout |
| `0x26` | 1 | Checksum (block sums to `0xFF`) — **written last**, so a torn write invalidates the image |
| `0x27` | 4 | Free / future |

> **RV-3028 EEPROM quirks (bench-found, handled in `RtcRV3028.cpp`):** the
> chip **NACKs all I²C** while its EEPROM engine runs — both during the
> power-on auto-refresh (>100 ms) and in short windows after each programmed
> byte. The driver therefore treats failed STATUS reads as "still busy"
> (300 ms deadline), retries each byte transaction up to 5×, and read-back
> verifies every written byte.

> **Flash BOM note:** the schematic specifies a 4 MB Macronix MX25L3233F, but
> assembled boards carry a **16 MB Winbond W25Q128** (JEDEC 0xEF4018). The driver
> auto-detects the real chip via the library's default device list, so the true
> size is used for the USB drive.

## Pin map (from the KiCad netlist)

| Function | MCU pin | Arduino pin |
|---|---|---|
| ST7789 CS / DC / RST / BL | PA18 / PA17 / PA16 / PA19 | D10 / D12 / D13 / D11 |
| ST7789 SPI (SERCOM2) | PA12 MOSI, PA13 SCK | MOSI/SCK |
| GNSS L86 UART (SERCOM3) | PA22 TX → L86, PA23 RX ← L86 | Serial1 |
| GNSS FORCE_ON / RESET_N | PB00 / PB31 | direct port |
| I²C @100 kHz: RTC 0x52, amp 0x58, accel 0x18, EEPROM 0x50 | PB02 SDA, PB03 SCL | SDA/SCL |
| Buttons SW1..SW4 (L→R, active low) | PB14, PB13, PB12, PB15 | D5, D4, D7, D6 |
| LED gates: left / bottom / right | PA21 / PA20 / PA06 | D8 / D9 / A2 |
| Audio DAC → TPA2016 | PA02 (DAC0) | A0 |
| Amp ~SD (HIGH = on) | PB17 | D2 |
| Power buzzer gate (J5) | PB16 | D3 |
| Supercap CAPGOOD (LTC3226) | PA04 | A3 |
| LIS3DH INT1 (polled, unused) | PB08 | A4 |
| 24LC512 WP | PB01 | direct port |
| QSPI flash | PA08–11, PB10, PB11 | QSPI |

## Building & flashing

```sh
pio run                      # build (default env: adafruit_metro_m4)
pio test -e native           # host-side unit tests (timezone/DST engine)
```

### MCU select (J19 / J20)

The prototype has a **SAMD51J19A** (512 KB flash, ~95% full); the next revision
uses a **SAMD51J20A** (1 MB flash, ~48% — same pinout). Pick the build in
`platformio.ini` (`default_envs`) or per-invocation:

| env | MCU | notes |
|---|---|---|
| `adafruit_metro_m4` | J19A 512 KB | default (current prototype) |
| `metro_m4_j20` | J20A 1 MB | `boards/samd51j20a_metro.json` + `ld/samd51j20a_*.ld` |
| `sim` / `sim_j20` | either | + serial GNSS simulator (`-DGNSS_SIM`) |

```sh
pio run -e metro_m4_j20      # J20 build; flash .pio/build/metro_m4_j20/firmware.bin @0x4000
```

> **J20 bootloader note:** the stock Metro M4 UF2 bootloader is built for the
> J19 — on a J20 board flash the app **over SWD** (same `.bin @ 0x4000` recipe).
> For UF2 drag-and-drop on the J20, build
> [uf2-samdx1](https://github.com/adafruit/uf2-samdx1) for the SAMD51J20A once.

Two flashing paths:

**A) UF2 bootloader (normal, no tools):** double-tap RESET to mount the
`METROM4BOOT` drive, then drag `firmware.uf2` onto it.

**B) SWD via Atmel-ICE (used in development):**

```sh
# build first, then flash the .bin at 0x4000 (offset preserves the bootloader)
openocd -c "adapter driver cmsis-dap" -c "transport select swd" \
        -c "adapter speed 2000" -f target/atsame5x.cfg \
        -c "init" -c "reset halt" \
        -c "flash write_image erase .pio/build/adafruit_metro_m4/firmware.bin 0x00004000 bin" \
        -c "reset run" -c "shutdown"
```

> ⚠️ **Never flash the `.elf` over SWD.** Its LOAD segment sits at address `0x0`
> and will overwrite the UF2 bootloader at flash start, bricking drag-and-drop
> programming (recovery = re-flash the bootloader). Always flash the **`.bin` at
> `0x4000`**.

`pio device monitor -b 115200` opens the USB-CDC console.

### USB identity

The app enumerates as **`1209:0001` — "K. Haaland / GNSS Alarm Clock"**.
VID `0x1209` belongs to [pid.codes](https://pid.codes) (free USB IDs for
open-source hardware); `0x0001` is their official **test PID**, fine for
personal use — register a permanent PID via a pid.codes PR (requires a public,
OSI-licensed repo) before distributing hardware. Identity lives in
`boards/samd51j19a_kh.json` / `boards/samd51j20a_metro.json`. Bootloader mode
(double-tap reset) still enumerates as Adafruit `239A` until a custom
uf2-samdx1 is built.

### Simulated GNSS (`env:sim`)

To test the timezone/clock pipeline on-device **without a real fix**, build the
`sim` env — identical firmware plus `-DGNSS_SIM` (the L86 init is skipped and a
serial console feeds position/time):

```sh
pio run -e sim -t upload      # or SWD-flash .pio/build/sim/firmware.bin @0x4000
pio device monitor -b 115200
```

Then type commands into the monitor:

```
pos 59.91 10.75              set a fix; resolves + applies the timezone
tz  -33.87 151.2             query a zone (Jan/Jul offset + DST), no state change
utc 2026 7 13 12 0 0         set the UTC clock so local time shows on the display
status                       print fix / UTC / local time / zone
nofix                        drop the simulated fix
```

`pos` also updates the on-screen clock's zone immediately, so you can watch the
display follow you around the globe. The `sim` code compiles to nothing in the
normal build — zero production-flash cost.

## Source layout

| File | Responsibility |
|---|---|
| `src/main.cpp` | Boot order, super-loop, alarm ↔ UI ↔ button routing, I²C @100 kHz |
| `src/ClockKeeper.*` | UTC anchor, GNSS→RTC discipline, local time / DST |
| `src/Timezone.*` | Coord→zone polygons+boxes + POSIX TZ/DST evaluator (host-testable) |
| `src/TimezonePolyData.h` | Polygon zone data: `tools/gen_tz_polys.py` (world set) / `tools/tz_polygon_editor.html` (hand edits) |
| `src/Gnss.*` | L86 NMEA via TinyGPS++, PMTK config, position/speed/altitude |
| `src/RtcRV3028.*` | RV-3028-C7 driver wrapper (backup switchover, UTC, PORF) |
| `src/AlarmManager.*` | Trigger scan, ringing/snooze/escalation state machine |
| `src/AudioEngine.*` | TC2 ISR → DAC0 playback, WAV streaming, melodies, buzzer |
| `src/TuneStorage.*` | QSPI flash, FAT12 volume, USB mass storage |
| `src/DisplayST7789.*` | ST7789 driver + LVGL 9 flush (RGB565), bring-up self-test |
| `src/Ui.*` | LVGL screens: clock, menu, alarm edit, zone, display, tunes, info, ringing |
| `src/Buttons.*` / `src/Leds.*` | Debounced input, LED section patterns |
| `src/AmpTPA2016.*` / `src/AccelLIS3DH.*` | Amp gain/enable, double-tap-to-snooze |
| `src/Settings.*` | Persistent settings in internal flash (emulated EEPROM) |
| `src/SimConsole.*` | Serial "virtual GPS" console — `env:sim` only (`-DGNSS_SIM`) |

## Diagnostics (compile-time, default off)

- `DISPLAY_SELFTEST` in [`DisplayST7789.cpp`](src/DisplayST7789.cpp) — on-glass
  pattern test (RGB fills, edge border) instead of LVGL.
- `I2C_SCAN` in [`main.cpp`](src/main.cpp) — loops scanning the I²C bus and
  prints ACKing addresses over USB serial (used to diagnose the RTC).

## Known issues / verify on hardware

See [`STATUS.md`](STATUS.md) for the full list. Highlights:

- 🔴 **RV-3028 RTC (U5) does not ACK on I²C** — bad joint / power on U5; being
  reflowed. (Time still works from GNSS; only power-off holdover is affected.)
- **L86 1PPS (pin 6) and AADET_N (pin 8) are unrouted** — no hardware PPS
  timing, no antenna-detect.
- **`CAPGOOD` polarity** assumed HIGH = charged (LTC3226 open-drain + pullup).
- Plus the schematic findings in [`HARDWARE_REVIEW.md`](HARDWARE_REVIEW.md)
  (LM3671 VIN/SW, L86 V_BCKP cold-start, D3 diode, C30, crystal caps, …).
