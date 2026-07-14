# 0005 — USB identity via pid.codes

Date: 2026-07-13 · Status: **Accepted**

## Context
The firmware shipped with Adafruit's VID/PID (0x239A:0x8020, Metro M4), so
hosts displayed "Adafruit …". Own VID costs ~$6000 (USB-IF); pid.codes hands
out PIDs under VID 0x1209 to open-source projects, and reserves
0x0001–0x0010 as free-for-testing.

## Decision
Enumerate as **0x1209:0x0001** (pid.codes test PID) with descriptor strings
"K. Haaland / GNSS Alarm Clock", MSC INQUIRY vendor "KH", and **MaxPower
500 mA** (the clock charges storage caps / future Li-ion from VBUS). The
Adafruit bootloader pairs stay in the board `hwids` so PlatformIO's upload
port-hunting still works.

## Consequences
- No "Adafruit" anywhere in app mode; upload/monitor flows unchanged.
- The test PID is shared by definition — register a permanent PID via a
  pid.codes PR (requires public OSI-licensed repo) before distributing.
- Bootloader mode still enumerates as 239A until a custom uf2-samdx1 is
  built (planned with the J20 bootloader, ADR-0008).
- MaxPower required patching TinyUSB 2.4.1 (hardcoded 100 mA) via
  `tools/patch_tinyusb_power.py` + `board_build.usb_power` (the builder's
  own define otherwise wins over `build_flags`).
