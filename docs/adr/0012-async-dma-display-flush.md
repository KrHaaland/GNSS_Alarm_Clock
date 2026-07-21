# 0012 — Async DMA display flush with double buffering

Date: 2026-07-21 · Status: **Accepted** · Supersedes: [0002](0002-synchronous-display-flush.md)

## Context
ADR-0002 banned DMA after the SPI library's own DMA path (4-arg
`SPI.transfer`) froze the MCU: its completion fired **DMAC channel-1's IRQ,
which had no handler installed**, parking the CPU in `Dummy_Handler`. The
synchronous flush that replaced it was later upgraded to a DRE-paced SERCOM
register loop at 30 MHz (wire speed, ~26 ms full frame on the NV3007) — but
it still blocks: LVGL cannot render the next strip while the previous one is
on the wire, and the main loop (GNSS parsing, buttons, WAV feed) stalls for
the duration of every flush.

The root cause of the 0002 freeze was the *missing handler*, not DMA itself.

## Decision
Flush the NV3007 asynchronously with a dedicated DMAC channel via
**Adafruit_ZeroDMA** (bundled with the core), which installs handlers for all
five SAMD51 DMAC IRQ lines — the exact piece missing in the 0002 incident:

- One channel, beat-triggered on `SERCOM2_DMAC_ID_TX`, byte beats, one
  descriptor rewritten per flush (`changeDescriptor` + `startJob`). A 32-row
  strip is 27 392 beats, under the 65 535-beat descriptor cap.
- **Two 32-row LVGL buffers** (2 × ~27 KB, RAM 49 → 63 %): `flush_cb` starts
  the DMA job and returns; LVGL renders the next strip into the other buffer
  while the transfer runs.
- The completion IRQ waits out the shift register (TXC, <1 µs), drains the RX
  side and clears BUFOVF/ERROR (so polled `SPI.transfer` keeps working for
  command bytes), raises CS, and calls `lv_display_flush_ready()`.
- **Fallbacks:** if channel allocation or `startJob` fails, the driver falls
  back to the synchronous DRE-paced loop — the display never goes dark over a
  DMA problem. `display_power()` (and `flush_cb` itself, belt-and-suspenders)
  wait out an in-flight transfer before touching the bus.

## Consequences
- Rendering and transfer overlap: full-screen redraws drop from
  render + ~26 ms to ~max(render, 26 ms); scroll/animation frames more easily
  fit the 15 ms refresh tick.
- The CPU is free during transfers — audio feed, GNSS parsing and button
  polling no longer stall behind a flush (most visible on the ring screen).
- +27 KB RAM for the second buffer (63 % on the J19; a non-issue on the
  J20's 256 KB).
- Anything new touching SERCOM2 or the render buffers outside `flush_cb`
  must call `dma_wait_idle()` first.
- Verified on hardware 2026-07-21: menus, fast screen changes, ring screen
  with audio — stable and visibly smoother ("displayet er smooth!").
