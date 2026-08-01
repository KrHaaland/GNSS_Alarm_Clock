# Session log: v2 board bring-up (2026-07-22 → 2026-08-01)

Human-readable summary of the bench session that brought the v2 board from
freshly soldered to fully operational. The complete raw log (every command,
measurement and wrong turn) is in `2026-07-13_2026-08-01-session-raw.jsonl.gz`.

## Starting point

Freshly hand-soldered v2 board: SMD only (THT display/SWD held back until
the LEDs were proven), no battery. The same session window also produced
the display/audio DMA work (ADR-0012/0013), Sky view, starry night and the
Tunes-freeze fix — this summary covers the hardware bring-up arc.

## The bring-up, in order of discovery

1. **Board enumerated immediately** — bootloader + USB + MCU good.
2. **Bottom LED section brownout-looped the board.** Root cause: the
   nPM1300 powers up with a 100 mA VBUS limit until the host raises it.
   Wrote the `ledtest` diag env (I2C scan + serial-armed LED cycling) and
   the nPM1300 unlock (`VBUSINILIM0=5` + `TASKUPDATEILIMSW`). All three
   sections passed with no battery fitted. → ADR-0014, `PmicNPM1300` module.
3. **I2C census:** RTC (0x52) ✅, TPA2016 (0x58) ✅, nPM1300 (0x6B) ✅ —
   and LIS3DH silent. After reflow of U7 it answered at **0x19** despite
   the design strapping SA0 low (R18): the strap isn't reaching the pin
   (cosmetic — FW now probes 0x18 and 0x19, WHO_AM_I-verified).
4. **Battery arrived (3000 mAh interim, 2000 mAh on order).** Charger
   configured: 200 mA, terminate 4.10 V (user's longevity choice), NTC
   type 10k (board has a fixed resistor — no thermistor in the pack).
   Battery/PMIC menu screen added.
5. **USB unplug killed the board** despite the battery → the battery
   holder was at fault; replaced by the user. Power path then verified:
   seamless VBUS ↔ battery switchover.
6. **GNSS silent (0 bytes).** The `ledtest` reset-pulse aliveness test
   (a healthy L86 answers $PMTK011 within ~1 s of reset even with no
   antenna) proved it electrically dead. Multimeter: RESET_N read 2.8 V
   (backup domain alive) while FORCE_ON read 0 V (main domain dead) →
   cold VCC joint on U4. Reflow → **13 satellites indoors.**
7. **Tunes screen froze the clock.** Two independent finds: LVGL pool
   exhaustion (64 K → 80 K; same failure signature as the earlier Sky-view
   incident) and, via a raw JEDEC probe (`EF 60 18`), that U8 is a
   **W25Q128FW — the 1.8 V variant** on a 3.3 V rail. Storage stays
   gracefully disabled until a W25Q128JVSIQ (3 V) is fitted; the BOM's
   "25Q128" listing hid the voltage suffix.
8. **The MCU is actually a SAMD51J20A** (`bossac -i`: ATSAMD51x20) — the
   schematic symbol says J19A. Switched default build to `metro_m4_j20`:
   flash 84.8 → 42.4 %, RAM 80.2 → 60.2 %. → ADR-0015.
9. **USB flashing degraded all day** (aborted write at 56 % → an app that
   enumerated but was half-flash garbage — "no satellites" red herring;
   failing 1200-baud touches). Switched to **SWD via Atmel-ICE** (openocd,
   `.bin` @ 0x4000 — never the `.elf`): fast and deterministic. Quirk:
   openocd's double reset parks the UF2 bootloader; one extra single reset
   enters the app.
10. **Battery-less boot bootlooped with a ghost image.** Long diagnosis
    (panel GRAM survives shallow brownouts, so the "image" was the previous
    boot's frame) ended with the user's find: the current display module
    straps **BL with a pull-up**, so a floating PA19 burns the backlight at
    full power through the whole bootloader phase — inside the 100 mA
    window. Tying BL to GND let it boot; the replacement display has a
    pull-down. FW now clamps BL/L86/amp as its first instructions and
    soft-starts the backlight; the bootloader-phase window remains until
    the custom bootloader lands.
11. **GNSS module end-of-life:** after all fixes the veteran L86 (4-5
    prototype solder cycles, spec allows 2-3) only produces NMEA when
    heated. Replacement ordered; the clock runs fine off the battery-backed
    RTC meanwhile.

## Where that leaves the project

Fully working v2 board pending three parts in the mail: L86-M33 module,
W25Q128JVSIQ flash, pull-down display. Open firmware items: custom
uf2-samdx1 bootloader (pid.codes identity + early ILIM write), v2
escalation stage (no buzzer in hardware), low-battery policy, finer
timezone polygons (flash headroom now allows it).

## Lessons collected

- Suffixes are the datasheet: W25Q128**FW** (1.8 V) vs **JV** (3 V);
  J19A symbol vs J20A chip. Verify silicon, not schematics.
- A PMIC's power-up defaults (100 mA, charger off) shape the whole boot
  story — and reset on every replug.
- A half-flashed app can enumerate and look alive; SWD verify-on-write
  beats USB bootloader flashing for bench work.
- Panel GRAM survives brownouts: a "working display" proves nothing about
  boot progress.
- Modules have reflow-cycle budgets; veterans of many prototypes fail in
  creative, heat-dependent ways.
