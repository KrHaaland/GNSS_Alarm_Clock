# 0001 — Tune storage on QSPI flash + USB mass storage

Date: 2026-07-04 · Status: **Accepted**

## Context
The board carries two candidate "EEPROMs" for alarm tunes: a 24LC512
(64 KB, I²C) and a QSPI serial flash (4 MB on the schematic; assembled
boards carry a 16 MB Winbond W25Q128). Tunes are sampled audio — seconds
of WAV at 22 kHz already exceeds 64 KB.

## Decision
Use the **QSPI flash** for tunes, formatted FAT12 and exposed to the host as
a **USB mass-storage drive** (`TUNES`) for drag-and-drop upload. Leave the
24LC512 unused (later repurposed candidate → superseded by ADR-0004/0008).

## Consequences
- Minutes of audio instead of seconds; standard Adafruit SPIFlash/SdFat stack.
- Tune upload requires no custom tooling — any file manager works.
- WAV playback is refused while the USB host owns the drive (`storage_busy`).
- The 24LC512 became dead weight → eventually dropped from the v2 BOM.
