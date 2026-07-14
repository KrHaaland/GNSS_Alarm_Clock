# 0006 — TinyUSB pinned to 2.4.1

Date: 2026-07-04 · Status: **Accepted**

## Context
The Adafruit SAMD core bundles its own TinyUSB copy. Relying on the bundled
copy broke enumeration (device descriptor read error -110); forcing 3.x via
lib_deps clashes with the bundled copy (TU_RESERVED/TU_COUNTER redefinitions).

## Decision
Pin `adafruit/Adafruit TinyUSB Library@2.4.1` — the newest version that
enumerates cleanly alongside this core.

## Consequences
- Stable CDC+MSC+HID composite; do not bump without re-testing enumeration.
- The build compiles **two** copies of some TinyUSB sources (lib_deps + core
  bundle) and the linker may pick either — patches must be applied to both
  (see `tools/patch_tinyusb_power.py`).
