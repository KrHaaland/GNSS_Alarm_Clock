# GNSS Alarm Clock — Firmware

Firmware for the custom **SAMD51J19A** alarm-clock board (Adafruit Metro M4
compatible), designed in `HARDWARE/samd51_gps_alarm_clock` (KiCad).

Time is set automatically from GNSS, the timezone (with DST) is derived offline
from the GNSS coordinates and saved to flash so it stays correct indoors, and
alarms drive an LED light-show + a tune through a class-D amp. The UI is LVGL on
a color TFT driven by four buttons.

> **Current state:** firmware is feature-complete and running on hardware. The
> display, GNSS time/timezone, alarms, audio, tunes-over-USB and settings all
> work. One **hardware** issue is open — the RV-3028 RTC does not respond on the
> I²C bus (bad joint on U5, being reflowed). See [`STATUS.md`](STATUS.md) and
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
  meridians). Anything not inside a box falls back to a whole-hour longitude
  estimate, so **every coordinate resolves**. Maps the GNSS position to a
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
the drag-and-drop workflow. The 24LC512 is left reserved (still ACKs at 0x50,
unused). Settings live in **SAMD51 internal flash** (emulated EEPROM), not in
either chip.

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

## Source layout

| File | Responsibility |
|---|---|
| `src/main.cpp` | Boot order, super-loop, alarm ↔ UI ↔ button routing, I²C @100 kHz |
| `src/ClockKeeper.*` | UTC anchor, GNSS→RTC discipline, local time / DST |
| `src/Timezone.*` | Coord→zone table + POSIX TZ/DST evaluator (host-testable) |
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
