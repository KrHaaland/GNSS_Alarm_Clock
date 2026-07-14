# 0008 — Hardware v2: SAMD51J20A + Li-ion

Date: 2026-07-13 · Status: **Accepted (planned)**

## Context
The J19A's 512 KB flash sits at ~93 % with fonts, LVGL and timezone data.
Supercaps limit alarm loudness duration and provide no true portability.

## Decision
v2 swaps in the pin-compatible **SAMD51J20A** (1 MB flash / 256 KB RAM,
build targets `metro_m4_j20`/`sim_j20` already in-tree with custom board
JSON + linker script) and replaces the supercaps with a **Li-ion cell +
USB charger** (power-path topology recommended; MaxPower 500 mA already
declared). The **24LC512 leaves the BOM** (ADR-0004).

## Consequences
- Flash pressure gone (~47 %); room for font subsetting to be optional.
- Firmware work queued for the schematic: charger status pins replace
  CAPGOOD, battery icon/percentage, low-battery behavior, RTC VBACKUP and
  L86 V_BCKP from the battery (fixes GNSS cold-start, HW review #10).
- A J20 uf2-samdx1 bootloader build is needed for UF2 drag-and-drop
  (SWD flashing works meanwhile) — good moment to bake in the pid.codes
  identity (ADR-0005).
