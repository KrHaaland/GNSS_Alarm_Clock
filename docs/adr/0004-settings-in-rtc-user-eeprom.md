# 0004 — Settings in the RV-3028 user EEPROM (packed 39 B)

Date: 2026-07-14 · Status: **Accepted** (supersedes internal-flash storage)

## Context
Settings originally lived in SAMD51 internal flash via FlashStorage_SAMD.
That storage array sits **inside the app image** (0x4C000), so every firmware
reflash wiped alarms/zone/counters. Alternatives: the idle 24LC512 (1M
cycles, but leaving the v2 BOM) or the RTC's 43-byte user EEPROM (~100k
cycles, survives reflash by construction, chip must exist anyway).

## Decision
Pack settings into a **39-byte image in the RV-3028 user EEPROM** (map in
README). Derived data is not stored: TZ strings re-derive at boot from
packed lat/lon centidegrees or the manual GMT-ladder index; alarm tune
filenames pack as 16-bit FNV-1a hashes matched against the TUNES directory.
Checksum written last so torn writes invalidate the block.

## Consequences
- Settings survive power loss **and reflashes** (user-verified both).
- 24LC512 can leave the v2 BOM; FlashStorage dependency dropped (−12 KB).
- Bench-found chip quirks are handled in `RtcRV3028.cpp`: the RV-3028
  **NACKs all I²C while its EEPROM engine runs** (POR auto-refresh >100 ms;
  short windows after each programmed byte) → tolerant 300 ms busy-wait,
  per-byte retry (≤5×) and read-back verification.
- Writes are slow (~16 ms/byte) — irrelevant at a-few-bytes-a-day rates.
