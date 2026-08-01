# 0015 — SAMD51J20A as default target; SWD-first flashing

Date: 2026-08-01 · Status: **Accepted**

## Context
The v2 board was designed around the schematic's ATSAMD51J19A symbol, but
the fitted chip reports `ATSAMD51x20` via `bossac -i` (1 MB flash / 256 KB
RAM) — the J19/J20 are pin-identical in TQFP64. Running the J19 build left
half the silicon unused and forced repeated memory squeezes (LVGL pool
incidents, font subsetting). Meanwhile, USB-based flashing (1200-baud touch
+ bossac) proved unreliable on the bench: racing re-enumeration, a write
aborted mid-flash (leaving a half-programmed app that enumerated but
misbehaved — worth remembering as a failure mode), and touches that reset
into the app instead of the bootloader.

## Decision
- **`metro_m4_j20` is the default PlatformIO environment** (512 KB → 1 MB
  flash, 192 → 256 KB RAM). The J19 env stays for the v1 prototype.
- **SWD via Atmel-ICE is the primary flashing path**: openocd CMSIS-DAP,
  `firmware.bin` written at **0x4000** (never the `.elf` — its LOAD segment
  at 0x0 overwrites the UF2 bootloader). Fast (~40 KiB/s), deterministic,
  immune to USB enumeration races, and it cannot half-flash silently
  (verify is part of the write).
- Known quirk: openocd's `reset halt` + `reset run` counts as a double-tap
  and parks the board in the bootloader; issue one extra single
  `reset run` (or press reset once) to enter the app.
- USB paths (1200-baud touch, UF2 drag-and-drop) remain as fallbacks when
  no debug probe is attached.

## Consequences
- Memory ceilings are gone for the foreseeable future (flash 42 %, RAM
  60 % at time of writing); features like a finer timezone polygon map
  (ADR-0003 tooling supports regeneration) become practical.
- The stock Metro M4 bootloader handles the x20 over bossac fine, so the
  old "J20 needs SWD for first flash" caveat is dropped; the custom
  uf2-samdx1 bootloader (pid.codes identity + early PMIC ILIM write,
  ADR-0014) remains planned but is not a blocker.
- CI builds all four envs, so the J19 build cannot silently rot.
