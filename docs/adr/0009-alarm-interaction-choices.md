# 0009 — Alarm interaction choices

Date: 2026-07-05..13 · Status: **Accepted**

## Context
Several small behavioral decisions shape how the alarm feels; each had a
bench-found reason.

## Decision
- **Double-tap** (not single-tap) snoozes: the speaker's own vibration
  triggers single-tap detection; the LIS3DH click path also needs its
  high-pass filter enabled (CTRL_REG2=0x84) or gravity sits at the
  threshold and fires continuously.
- Taps are ignored the first **2 s** of each ring (stale-tap drain +
  arm delay) so a bump can't instantly snooze.
- **daysMask 0 == daily** (0x7F) — an alarm with no days selected still
  rings rather than silently never firing.
- A newly-due alarm **takes over an active snooze** instead of being
  swallowed; stopping during the trigger minute cannot re-fire it.
- Escalation (power buzzer after N min) restarts its countdown on each
  snooze re-ring; auto-silence after 15 min.
- Every snooze increments the persisted **shame counter** (week + total),
  shown on the ringing screen and System info.

## Consequences
Predictable wake-up behavior with the failure modes (vibration self-snooze,
forgotten day-mask, swallowed alarms) engineered out.
