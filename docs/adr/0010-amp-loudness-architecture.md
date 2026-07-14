# 0010 — Amp loudness architecture: AGC leveling, limiter as volume

Date: 2026-07-14 · Status: **Accepted**

## Context
The TPA2016D2 was used as "fixed gain + peak limiter"; loudness = the gain
register, digital volume scaled the DAC stream in parallel. User-supplied
WAV tunes are mastered wildly differently, and a bedside alarm benefits from
a gentle volume ramp.

## Decision
Enable the chip's **AGC at 1:4 compression** so program material is leveled
toward the output ceiling. With AGC active the **output limiter level is the
real volume control**, so:
- volume 0–10 maps to the limiter level (0.5 dBV steps, ceiling +6.5 dBV
  until the speaker's rating justifies more; register max +9 dBV),
- fixed gain stays constant (+6 dB into the AGC),
- the digital (DAC) volume is pinned high — it would otherwise be undone by
  the AGC,
- **gentle wake**: on an alarm's first ring the limiter ramps from minimum to
  the set volume over 0/15/30/60 s (menu: "Ramp"); snooze **re-rings skip the
  ramp**, and buzzer escalation jumps straight to full volume,
- the SETUP register's fault/thermal bits are surfaced on System info.

## Consequences
- All tunes play at comparable loudness regardless of mastering.
- Wake-up starts gentle but cannot be slept through (re-ring + escalation
  bypass the ramp).
- Volume perception is now tied to the limiter: revisit the ceiling
  (`LIMITER_MAX_STEP`) once the speaker's power handling is confirmed.
