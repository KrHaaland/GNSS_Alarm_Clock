# GNSS Alarm Clock — Firmware

Firmware for the custom SAMD51J19A alarm-clock board (Adafruit Metro M4
compatible), designed in `HARDWARE/samd51_gps_alarm_clock` (KiCad).

## What it does

- **Time**: The Quectel L86 GNSS receiver (Serial1) provides UTC. It
  disciplines the RV-3028-C7 RTC, which keeps time through power loss thanks
  to its VBACKUP pin on the LTC3226 supercap rail. On boot the clock starts
  from the RTC immediately and refines from GNSS when available.
- **Timezone from coordinates**: An embedded offline lookup maps the GNSS
  position to a timezone (fine-grained in Europe, coarse worldwide) with a
  POSIX TZ string including DST rules. The result is **persisted to internal
  flash**, so local time (including DST changes) stays correct after reboot
  even when no fix is possible indoors. Manual zone override is available in
  the menu.
- **Alarms**: Two independent alarms with weekday masks. On trigger:
  - the 34 LEDs (3 sections: left / bottom / right) run a chase sequence at
    full brightness from the supercap rail (LTC3226-charged, so brightness
    doesn't depend on the USB supply),
  - a tune plays: WAV file from the 4 MB QSPI flash, or a built-in melody,
    through DAC0 → TPA2016D2 class-D amp → speaker (J6),
  - optional escalation: after N minutes unacknowledged, the power buzzer
    (J5, MOSFET-switched from the supercap rail) joins in.
  - **Snooze**: short-press any button, or tap the clock (LIS3DH tap
    detection). **Stop**: long-press any button.
- **UI**: LVGL 8 on the SH1122 256×64 grayscale OLED, driven by the 4
  buttons above the display: `[Back] [Prev] [Next] [OK]`.
- **Tune upload**: the QSPI flash appears as a **USB flash drive** — drag &
  drop `.wav` files (PCM, 8/16-bit, mono/stereo, 8–48 kHz).

## Storage decision

The board has two candidate "EEPROMs" for tunes: the 24LC512 (64 KB, I²C)
and the MX25L3233F (4 MB, QSPI). The firmware uses the **MX25L3233F**: it is
64× larger (minutes of sampled audio instead of seconds), much faster, sits
on the same QSPI pins as a Metro M4's onboard flash (standard library
support), and enables the USB mass-storage workflow. The 24LC512 is left
untouched/reserved. Settings (timezone, alarms, …) live in SAMD51 internal
flash (emulated EEPROM), not in either chip.

## Pin map (from the KiCad netlist)

| Function | MCU pin | Arduino pin |
|---|---|---|
| SH1122 CS / DC / RST / BL | PA18 / PA17 / PA16 / PA19 | D10 / D12 / D13 / D11 |
| SH1122 SPI (SERCOM2) | PA12 MOSI, PA13 SCK | MOSI/SCK |
| GNSS L86 UART (SERCOM3) | PA22 TX → L86, PA23 RX | Serial1 |
| GNSS FORCE_ON / RESET_N | PB00 / PB31 | direct port |
| I²C (SERCOM5): RTC 0x52, amp 0x58, accel 0x18, EEPROM 0x50 | PB02 SDA, PB03 SCL | SDA/SCL |
| Buttons SW1..SW4 (L→R, active low) | PB14, PB13, PB12, PB15 | D5, D4, D7, D6 |
| LED gates: left / bottom / right | PA21 / PA20 / PA06 | D8 / D9 / A2 |
| Audio DAC → TPA2016 | PA02 (DAC0) | A0 |
| Amp ~SD (HIGH = on) | PB17 | D2 |
| Power buzzer gate (J5) | PB16 | D3 |
| Supercap CAPGOOD (LTC3226) | PA04 | A3 |
| LIS3DH INT1 | PB08 | A4 |
| 24LC512 WP | PB01 | direct port |
| QSPI flash MX25L3233F | PA08–11, PB10, PB11 | QSPI |

## Building

```sh
pio run                      # build
pio run -t upload            # flash over USB (BOSSA bootloader)
pio device monitor           # 115200 baud USB CDC
pio test -e native           # host-side unit tests (timezone/DST engine)
```

## Source layout

| File | Responsibility |
|---|---|
| `src/main.cpp` | Boot order, loop, alarm ↔ UI ↔ button routing |
| `src/ClockKeeper.*` | UTC anchor, GNSS→RTC discipline, local time |
| `src/Timezone.*` | Coord→zone table + POSIX TZ/DST evaluator (host-testable) |
| `src/Gnss.*` | L86 NMEA via TinyGPS++, PMTK config |
| `src/RtcRV3028.*` | RTC driver wrapper (backup switchover, UTC) |
| `src/AlarmManager.*` | Trigger scan, ringing/snooze/escalation state machine |
| `src/AudioEngine.*` | TC2 ISR → DAC0 playback, WAV streaming, melodies, buzzer |
| `src/TuneStorage.*` | QSPI flash, FAT volume, USB mass storage |
| `src/DisplaySH1122.*` | SH1122 driver + LVGL flush (RGB565→4-bit gray) |
| `src/Ui.*` | LVGL screens: clock, menu, alarm edit, zone, tunes, info |
| `src/Buttons.*` / `src/Leds.*` | Debounced input, LED section patterns |
| `src/AmpTPA2016.*` / `src/AccelLIS3DH.*` | Amp gain/enable, tap-to-snooze |
| `src/Settings.*` | Persistent settings in internal flash |

## Notes / verify on hardware

- `CAPGOOD` polarity: code assumes HIGH = supercaps charged (LTC3226 CAPGD
  open-drain with 10 kΩ pullup). Verify on first power-up.
- The L86 backup/FORCE_ON pins are left hi-Z; the module cold-starts on
  power loss of its own backup domain (typ. fix in ~30 s outdoors, longer
  indoors — hence the persisted timezone).
- SH1122 SPI runs at 8 MHz; raise toward 16 MHz after scope-checking.
