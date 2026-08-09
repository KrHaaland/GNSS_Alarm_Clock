# 0010 — Amp loudness architecture: AGC leveling, limiter as volume

Date: 2026-07-14, ceiling re-tuned 2026-08-09 · Status: **Accepted**

## Context
The TPA2016D2 was used as "fixed gain + peak limiter"; loudness = the gain
register, digital volume scaled the DAC stream in parallel. User-supplied
WAV tunes are mastered wildly differently, and a bedside alarm benefits from
a gentle volume ramp.

## Decision
Enable the chip's **AGC at 1:4 compression** so program material is leveled
toward the output ceiling. With AGC active the **output limiter level is the
real volume control**, so:
- volume 0–10 maps to the limiter level: v1 = −6.5 dBV (~28 mW into 8 Ω) …
  v10 = **+7 dBV** (~0.63 W into 8 Ω). The ceiling is deliberately below the
  chip max (+9 dBV): bench measurement (2026-08-09) showed max volume + LED
  chase crossing the nPM1300's **1000 mA battery discharge limit** as the
  AGC wound up — IBATLIM collapses VSYS and resets the device. A USB-only
  higher ceiling was rejected: the limit applies instantly at unplug while
  software reacts in ~10–50 ms, an unwinnable race. See ADR-0014,
- fixed gain stays constant (+6 dB into the AGC),
- the digital (DAC) volume is pinned high — it would otherwise be undone by
  the AGC,
- **gentle wake**: on an alarm's first ring the limiter ramps from minimum to
  the set volume over 0/15/30/60 s — configured **per alarm** in the alarm
  editor (weekday alarm can fade in, weekend alarm can blast); snooze
  **re-rings skip the ramp**, and buzzer escalation jumps to full volume,
- the SETUP register's fault/thermal bits are surfaced on System info.

## Consequences
- All tunes play at comparable loudness regardless of mastering.
- Wake-up starts gentle but cannot be slept through (re-ring + escalation
  bypass the ramp).
- Volume perception is now tied to the limiter. The floor cannot go below
  the chip's −6.5 dBV while AGC leveling is active (the AGC lifts material
  toward the ceiling regardless of input gain) — a true whisper mode would
  need compression disabled at low volumes.
