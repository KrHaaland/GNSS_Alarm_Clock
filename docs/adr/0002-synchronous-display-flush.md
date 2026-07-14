# 0002 — Synchronous per-byte display flush (no DMA)

Date: 2026-07-05 · Status: **Accepted**

## Context
The ST7789 flush originally used the SAMD51 SPI DMA path (4-arg
`SPI.transfer`). On this core the DMA completion fires **DMAC channel-1's
IRQ, which has no handler installed** — the CPU lands in `Dummy_Handler`'s
infinite loop: display dead, USB dead, MCU frozen. Diagnosed over SWD
(PC parked in `Dummy_Handler`).

## Decision
Stream pixels with **synchronous per-byte `SPI.transfer`** at 24 MHz and
accept ~15 ms per full-frame flush. Do not enable DMA without first
installing proper DMAC IRQ handlers.

## Consequences
- Absolute stability; LVGL partial rendering keeps typical flushes small.
- ~15 ms of blocking CPU per full-screen redraw (menu transitions) — fine in
  practice at 24 MHz.
- A future optimization (direct SERCOM register loop or DMA-with-handler)
  can halve it if ever needed.
