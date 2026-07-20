# 0011 — Compile-time panel select + subset clock font

Date: 2026-07-14 · Status: **Accepted**

## Context
The prototype's ST7789 (284×76) was replaced by an NV3007 bar TFT (native
142×428, driven landscape 428×142) on the same SPI pins. The NV3007 is not
init-compatible: without its vendor register dump (behind the `0xFF 0xA5`
page unlock) the address engine ignores windows and wraps writes linearly
across the glass — bench-observed as colored bands. It also requires SPI
mode 0 and has a GRAM inset (168×428, visible columns offset 12/14).
Separately, the old 48 px clock figure drowned on the 142 px-tall panel,
and LVGL's built-in fonts stop at 48 px.

## Decision
1. **One display API, compile-time panel select.** Both drivers implement the
   same four functions + `DISP_W/H`; `Display.h` picks the driver from a
   build flag (`DISPLAY SELECT` in platformio.ini, NV3007 default, ST7789 for
   the v1 board). The rest of the firmware is panel-agnostic; the UI reads
   `DISP_W/H`.
2. **NV3007 init is the cross-verified vendor sequence** (five independent
   driver codebases agree), kept byte-exact including undocumented registers
   (`0x46 0x10`, doubled `0xEC`). Landscape MADCTL 0x60 with RASET+14.
3. **Big clock figure via a generated subset font**: `font_clock_100.c` is a
   Montserrat-Medium 100 px subset of the 18 characters the big label can
   show, generated with Pillow from lvgl's own bundled TTF directly in the
   LVGL 9.5 `fmt_txt` layout (4bpp continuous packing, adv_w in 1/16 px,
   FORMAT0_TINY cmap, no kerning) — no node/lv_font_conv dependency.

## Consequences
- Panel swaps are a driver file + one flag; bring-up tooling
  (`DISPLAY_SELFTEST` patterns) is part of each driver.
- Retiring both Montserrat-48 users made the 90 KB full-ASCII font
  linker-GC'd: the clock **doubled in size while flash dropped 95.7% → 82.9%**.
  The same subsetting technique is available if flash gets tight again.
- NV3007 quirks to remember: SPI mode 0 only; partial writes can half-light
  adjacent pixels for RGB channel values 1–31 on black (LVGL strip flushes
  avoid it); GRAM readback over SPI does not work.
