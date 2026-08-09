# Bench session 2026-08-03 → 08-09 — power budget, RTC backup, bootloader

Follow-up bench work on the v2 board (J20 + nPM1300 + LG HG2). Three
investigations that turned out to be one story: the battery discharge limit.

## 1. The IBATLIM discovery (speaker experiments)

Trigger: 2×4 Ω speakers in parallel (2 Ω) instantly reset the device; the
same pair in series (8 Ω) cut out ~30 s into a full-volume tune with all
LEDs on — even at 4.1 V battery.

Root cause: the **nPM1300 limits battery discharge to 1000 mA (IBATLIM,
its maximum setting)**. Exceeding it collapses VSYS below VSYSPOF → hard
reset. The ~30 s delay was the TPA2016's AGC winding program material up
toward the limiter ceiling — draw creeps for the first half minute of
playback. Measured with the new dev telemetry (live V/mA on the clock
face + ringing screen, `UI_DEV_IBAT`): 500–900 mA swings at the re-tuned
ceiling, worst case (all 34 LEDs continuous + bass-heavy tune).

Decisions:
- **Volume ceiling lowered to +7 dBV** (limiter step 27, was chip-max 31):
  the whole 0–10 knob rescales, max now equals the measured-safe level.
- **USB-only higher ceiling rejected**: IBATLIM applies analog-instantly at
  unplug; software reacts in 10–50 ms. Unwinnable race (ADR-0010).
- Speaker guidance: min 4 Ω per amp channel (chip spec); loudness gains
  come free from enclosure sealing + driver sensitivity, not lower
  impedance. Deep bass costs the most mA for the least sound on small
  drivers — master alarm tunes with a ~200 Hz high-pass.

## 2. RTC reset on a flat battery

The RTC lost time when the cell ran flat, despite VBACKUP sitting on VBAT.
Two real gaps found and fixed:
- Factory-fresh RV-3028s ship with **backup switchover disabled**; the
  library nominally sets LSM every boot but nothing ever verified it.
  `rtc_begin()` now reads the config EEPROM back (via Refresh → RAM mirror;
  ReadSingle can't reach 0x37) and rewrites until BSM=LSM + TCE=off
  verifies. System info shows `ok+LSM` / `BKUP=0xNN!` / `POR!`.
- The PORF flag was latched but never read by anyone — now sticky and
  surfaced, so a future reset is attributable.

The actual trigger was most likely a bench-only ground transient (floating
charger hot-plugged while SWD pinned board GND to PC earth). v3 checklist:
buffer cap on VBACKUP (U5.6).

## 3. Bootloader zeroed by a brownout

After the repeated VSYSPOF resets above, the board went dark: **the UF2
bootloader's first 16 bytes (vector table) were zeroed** — SP=0/PC=0 at
boot → double-fault lockup. Exactly one NVMCTRL quad-word (16 B) written
at address 0, which is NVMCTRL.ADDR's reset default: a wild write during
one of the brownout deaths (the firmware itself never touches internal
flash).

Recovery + hardening:
- Bootloader restored over SWD (Adafruit uf2-samdx1 v3.16.0 for Metro M4);
  app region was untouched.
- **BOOTPROT fuse set to 16 KB** (was 0 = unprotected): flash hardware now
  refuses any write/erase in the bootloader region. Clear with
  `atsame5 bootloader 0` before intentional bootloader updates.

## 4. USB flashing repaired (long-standing mystery solved)

ADR-0015's "1200-baud touches reset into the app instead of the
bootloader" got a root cause: the Arduino core writes the stay-in-bootloader
magic at the **compiled chip's** RAM top (J20 → 0x2003FFFC), the stock
J19A-built bootloader reads **its** RAM top (0x2002FFFC). The
`patch_tinyusb_power.py` build script now also patches `initiateReset()`
to write both addresses. `pio run -e metro_m4_j20 -t upload` verified
end-to-end over USB — bossac writes+verifies in ~4 s (~5× faster than
SWD). SWD remains the surgical path (this session was its showcase:
lockup autopsy, bootloader restore, fuses).

## 5. Settings format v4

snoozeTotal shrunk u32 → u24 (caps 16.7 M) and the spare byte dropped:
40 → 38 B packed, 5 B free. Old blocks migrate **in place at boot** via the
established versioned-migration pattern (v1/v2 chain through the same
shift); verified on-device — counters and alarms survived. Little-endian
made it clean: the u24 keeps the u32's three low bytes in place.

Also this window: display flipped 180° (`MADCTL 0xA0`, Y-offset 12) to
match the enclosure mounting.
