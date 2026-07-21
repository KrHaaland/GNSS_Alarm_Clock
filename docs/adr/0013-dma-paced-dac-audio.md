# 0013 — DMA-paced DAC audio playback

Date: 2026-07-21 · Status: **Accepted** · Related: [0012](0012-async-dma-display-flush.md)

## Context
Audio playback (WAV from QSPI + builtin melodies) was paced by a TC2 ISR
firing **once per sample** — 22 050–48 000 interrupts per second — popping a
lock-free ring buffer into DAC0. That worked, but burned a few percent CPU
in pure interrupt overhead, added constant IRQ jitter, and the display's new
async DMA flush (ADR-0012) proved the DMAC + Adafruit_ZeroDMA path safe.

## Decision
Let the DMAC move samples instead of the CPU:

- **TC2 stays the sample-rate pacer** (MFRQ at the WAV's rate, IRQ disabled);
  each overflow raises `TC2_DMAC_ID_OVF`, triggering **one halfword beat**
  ring → `DAC->DATA[0]`.
- The ring is now a **two-half ping-pong buffer** (2 × 2048 samples, same
  8 KB as before): the two descriptors are linked in a loop with
  `BLOCKACT=INT`, so the channel plays A → B → A … continuously and the CPU
  gets **one IRQ per half** (~93 ms at 22.05 kHz) instead of one per sample.
- The half-complete IRQ frees the played half for `audio_task()` to refill.
  **Underrun guard:** if the half now starting was never refilled, the IRQ
  overwrites it with midpoint silence — stale audio never loops audibly
  (the analog of the old ISR's midpoint hold).
- Drain (end of a non-looping source) pads the final half with silence and
  stops after both queued halves have played.
- **Fallback:** if DMA channel allocation fails, `audio_begin()` re-enables
  the per-sample TC2 ISR, which walks the same halves.

## Consequences
- CPU cost of playback drops to ~zero; no per-sample IRQ jitter. Together
  with ADR-0012 the ring screen is interrupt-quiet: the DMAC streams pixels
  and samples concurrently while the CPU only decodes WAV and runs LVGL.
- Volume/content changes take effect within ~one half (~0.1–0.2 s), same
  order as the old 4096-sample ring.
- Two DMAC channels now in use (display=0, audio=1); audio's single-beat
  triggers interleave freely with the display's stream.
- Verified on hardware 2026-07-21: melodies + WAV play cleanly while
  scrolling the UI.
