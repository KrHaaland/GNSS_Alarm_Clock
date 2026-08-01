// DisplayNV3007.cpp — NV3007 color TFT driver + LVGL 9 glue. See header.
// Compiled unless the build selects the old panel (see Display.h).
#ifndef DISPLAY_ST7789

#include "DisplayNV3007.h"
#include "pins.h"
#include <Adafruit_ZeroDMA.h>
#include <SPI.h>

// 30 MHz: SERCOM2 is re-clocked from the 120 MHz source (setClockSource in
// display_init; the default 48 MHz source tops out at 24). NV3007's reliable
// ceiling is ~40 MHz per bench reports, so 30 keeps margin — drop to 24 if
// artifacts ever show over the wiring. NOTE: NV3007 requires SPI MODE 0.
static const uint32_t kSpiHz = 30000000;

// Bring-up aid: when 1, display_init() runs an on-glass pattern self-test
// (RGB fills, edge border, origin marker) in an infinite loop INSTEAD of
// starting LVGL. Set back to 0 for normal operation.
#define DISPLAY_SELFTEST 0

// --- Panel-specific: tune these on hardware if the image is wrong -----------
// MADCTL for landscape (bit5 MV row/col exchange, bit6 MX, bit7 MY, bit3 BGR).
// If mirrored/upside-down, try 0x60 / 0xA0 / 0xC0 / 0x70.
#define NV_MADCTL 0x60
// 142x428 modules run non-inverted (vendor init sends no INVON); flip only
// if colors come out negative (one TFT_eSPI report needed it).
#define NV_INVERSION 0
// GRAM is 168(w) x 428(h) native; the 142 visible columns are inset 12/14.
// In LANDSCAPE (MV set) the inset lands on RASET: MADCTL 0x60 -> Y+14,
// MADCTL 0xA0 (flipped) -> Y+12. X spans all 428 rows, offset 0.
#define NV_X_OFFSET 0
#define NV_Y_OFFSET 14

// Two partial-render buffers, 32 rows each, RGB565 (~27 KB apiece): LVGL
// renders the next strip into one while DMA streams the other to the panel.
// A strip is 27392 bytes — safely under the DMAC's 65535-beat descriptor cap.
static uint8_t s_buf[DISP_W * 32 * 2] __attribute__((aligned(4)));
static uint8_t s_buf2[DISP_W * 32 * 2] __attribute__((aligned(4)));
static lv_display_t *s_disp;

// Async flush: one DMAC channel, beat-triggered on SERCOM2 TX-ready. The old
// freeze (ADR-0002) was an *unhandled* DMAC_1 IRQ from the SPI library's own
// DMA path — Adafruit_ZeroDMA installs handlers for all five DMAC IRQ lines,
// which is exactly the missing piece.
static Adafruit_ZeroDMA s_dma;
static DmacDescriptor *s_dmaDesc;
static bool s_dmaReady;                // channel allocated OK at init
static volatile bool s_dmaBusy;        // transfer in flight (cleared in IRQ)

static inline void cs_low() { digitalWrite(PIN_OLED_CS, LOW); }
static inline void cs_high() { digitalWrite(PIN_OLED_CS, HIGH); }

static void wr_cmd(uint8_t c) {
  digitalWrite(PIN_OLED_DC, LOW);
  SPI.transfer(c);
  digitalWrite(PIN_OLED_DC, HIGH);
}

static void wr_data(const uint8_t *d, size_t n) {
  for (size_t i = 0; i < n; i++)
    SPI.transfer(d[i]);
}

// Stream bytes at wire speed: DRE-paced writes straight into SERCOM2's DATA
// register. Per-byte SPI.transfer() waits out a full RX round-trip per byte
// (call overhead + RXC poll), roughly halving throughput; here SCK runs
// back-to-back (DATA is double-buffered). The unread RX side overflows by
// design — drained and cleared afterwards so SPI.transfer() keeps working
// for command bytes.
static inline void spi_drain_rx() {
  volatile SercomSpi &spi = SERCOM2->SPI;
  while (!spi.INTFLAG.bit.TXC) {
  }
  (void)spi.DATA.reg; // drain RX
  (void)spi.DATA.reg;
  spi.STATUS.bit.BUFOVF = 0;
  spi.INTFLAG.reg = SERCOM_SPI_INTFLAG_ERROR;
}

static void spi_write_bulk(const uint8_t *d, uint32_t n) {
  volatile SercomSpi &spi = SERCOM2->SPI;
  for (uint32_t i = 0; i < n; i++) {
    while (!spi.INTFLAG.bit.DRE) {
    }
    spi.DATA.reg = d[i];
  }
  spi_drain_rx();
}

// Any code touching the SPI bus (or the buffers) outside flush_cb must wait
// out an in-flight DMA transfer first. Full strip = ~7 ms at 30 MHz.
static inline void dma_wait_idle() {
  while (s_dmaBusy) {
  }
}

// DMAC IRQ: last byte handed to the SERCOM. Wait out the shifter (<1 us),
// clean up the RX side so polled SPI.transfer() keeps working, release CS
// and hand the buffer back to LVGL.
static void dma_done_cb(Adafruit_ZeroDMA *) {
  spi_drain_rx();
  cs_high();
  s_dmaBusy = false;
  lv_display_flush_ready(s_disp);
}

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  x0 += NV_X_OFFSET;
  x1 += NV_X_OFFSET;
  y0 += NV_Y_OFFSET;
  y1 += NV_Y_OFFSET;
  uint8_t c[4];
  wr_cmd(0x2A); // CASET
  c[0] = x0 >> 8; c[1] = x0 & 0xFF; c[2] = x1 >> 8; c[3] = x1 & 0xFF;
  wr_data(c, 4);
  wr_cmd(0x2B); // RASET
  c[0] = y0 >> 8; c[1] = y0 & 0xFF; c[2] = y1 >> 8; c[3] = y1 & 0xFF;
  wr_data(c, 4);
  wr_cmd(0x2C); // RAMWR
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
  const uint32_t px_count =
      (uint32_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);

  // LVGL RGB565 is little-endian; the panel wants MSB first -> swap in place.
  lv_draw_sw_rgb565_swap(px, px_count);

  dma_wait_idle(); // LVGL already gates on flush_ready; belt and suspenders
  cs_low();
  set_window((uint16_t)area->x1, (uint16_t)area->y1, (uint16_t)area->x2,
             (uint16_t)area->y2);

  if (s_dmaReady) {
    // Async: hand the strip to the DMAC and return — LVGL renders the next
    // strip into the other buffer meanwhile. dma_done_cb() releases CS and
    // calls lv_display_flush_ready() from the IRQ.
    s_dmaBusy = true;
    s_dma.changeDescriptor(s_dmaDesc, px, (void *)&SERCOM2->SPI.DATA.reg,
                           px_count * 2);
    if (s_dma.startJob() == DMA_STATUS_OK)
      return;
    s_dmaBusy = false; // channel refused the job — fall through, synchronous
  }
  spi_write_bulk(px, px_count * 2);
  cs_high();
  lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void) { return millis(); }

// NV3007 init: the vendor register dump is MANDATORY — 0xFF 0xA5 unlocks the
// extended register page (power/bias, gamma, gate-driver map, source timing);
// without it the address engine misbehaves (writes wrap linearly across the
// glass). 0xFF 0x00 returns to the standard MIPI-DCS page for COLMOD/MADCTL/
// SLPOUT. Sequence cross-verified across the vendor reference txt,
// Arduino_GFX, LVGL's lv_nv3007, a STM32 driver and an esp_lcd component
// (142x428 "2.79-inch" variant). Keep 0x46 0x10 and the doubled 0xEC — they
// appear in every working sequence, undocumented.
static const uint8_t kInit[] = {
    // cmd, num-args, args...
    0xFF, 1, 0xA5, // unlock vendor register page
    // power / bias
    0x9A, 1, 0x08, 0x9B, 1, 0x08, 0x9C, 1, 0xB0, 0x9D, 1, 0x16, 0x9E, 1, 0xC4,
    0x8F, 2, 0x55, 0x04,
    0x84, 1, 0x90, 0x83, 1, 0x7B, 0x85, 1, 0x33,
    // gamma (positive 0x6x, negative 0x7x)
    0x60, 1, 0x00, 0x70, 1, 0x00, 0x61, 1, 0x02, 0x71, 1, 0x02,
    0x62, 1, 0x04, 0x72, 1, 0x04, 0x6C, 1, 0x29, 0x7C, 1, 0x29,
    0x6D, 1, 0x31, 0x7D, 1, 0x31, 0x6E, 1, 0x0F, 0x7E, 1, 0x0F,
    0x66, 1, 0x21, 0x76, 1, 0x21, 0x68, 1, 0x3A, 0x78, 1, 0x3A,
    0x63, 1, 0x07, 0x73, 1, 0x07, 0x64, 1, 0x05, 0x74, 1, 0x05,
    0x65, 1, 0x02, 0x75, 1, 0x02, 0x67, 1, 0x23, 0x77, 1, 0x23,
    0x69, 1, 0x08, 0x79, 1, 0x08, 0x6A, 1, 0x13, 0x7A, 1, 0x13,
    0x6B, 1, 0x13, 0x7B, 1, 0x13, 0x6F, 1, 0x00, 0x7F, 1, 0x00,
    0x50, 1, 0x00, 0x52, 1, 0xD6, 0x53, 1, 0x08, 0x54, 1, 0x08,
    0x55, 1, 0x1E, 0x56, 1, 0x1C,
    // gate driver (GOA)
    0xA0, 3, 0x2B, 0x24, 0x00,
    0xA1, 1, 0x87, 0xA2, 1, 0x86, 0xA5, 1, 0x00, 0xA6, 1, 0x00,
    0xA7, 1, 0x00, 0xA8, 1, 0x36, 0xA9, 1, 0x7E, 0xAA, 1, 0x7E,
    0xB9, 1, 0x85, 0xBA, 1, 0x84, 0xBB, 1, 0x83, 0xBC, 1, 0x82,
    0xBD, 1, 0x81, 0xBE, 1, 0x80, 0xBF, 1, 0x01, 0xC0, 1, 0x02,
    0xC1, 1, 0x00, 0xC2, 1, 0x00, 0xC3, 1, 0x00, 0xC4, 1, 0x33,
    0xC5, 1, 0x7E, 0xC6, 1, 0x7E,
    0xC8, 2, 0x33, 0x33,
    0xC9, 1, 0x68, 0xCA, 1, 0x69, 0xCB, 1, 0x6A, 0xCC, 1, 0x6B,
    0xCD, 2, 0x33, 0x33,
    0xCE, 1, 0x6C, 0xCF, 1, 0x6D, 0xD0, 1, 0x6E, 0xD1, 1, 0x6F,
    0xAB, 2, 0x03, 0x67, 0xAC, 2, 0x03, 0x6B,
    0xAD, 2, 0x03, 0x68, 0xAE, 2, 0x03, 0x6C,
    0xB3, 1, 0x00, 0xB4, 1, 0x00, 0xB5, 1, 0x00,
    0xB6, 1, 0x32, 0xB7, 1, 0x7E, 0xB8, 1, 0x7E,
    // source / timing
    0xE0, 1, 0x00, 0xE1, 2, 0x03, 0x0F, 0xE2, 1, 0x04, 0xE3, 1, 0x01,
    0xE4, 1, 0x0E, 0xE5, 1, 0x01, 0xE6, 1, 0x19, 0xE7, 1, 0x10,
    0xE8, 1, 0x10, 0xEA, 1, 0x12, 0xEB, 1, 0xD0, 0xEC, 1, 0x04,
    0xED, 1, 0x07, 0xEE, 1, 0x07, 0xEF, 1, 0x09, 0xF0, 1, 0xD0,
    0xF1, 1, 0x0E, 0xF9, 1, 0x17,
    0xF2, 4, 0x2C, 0x1B, 0x0B, 0x20,
    0xE9, 1, 0x29, // dot inversion
    0xEC, 1, 0x04, // (doubled on purpose — present in all working dumps)
    0x35, 1, 0x00,       // TE on (V-blank)
    0x44, 2, 0x00, 0x10, // TE scanline
    0x46, 1, 0x10,       // vendor, undocumented, load-bearing
    0xFF, 1, 0x00, // back to the standard DCS page
    0x3A, 1, 0x05, // COLMOD: RGB565
    0x36, 1, NV_MADCTL,
    0x11, 0, // SLPOUT (delay handled in the init loop)
    0xFF, 0xFF, // end marker
};

#if DISPLAY_SELFTEST
static void st_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                    uint16_t color) {
  SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
  cs_low();
  set_window(x0, y0, x1, y1);
  const uint32_t n = (uint32_t)(x1 - x0 + 1) * (y1 - y0 + 1);
  const uint8_t hi = color >> 8, lo = color & 0xFF;
  for (uint32_t i = 0; i < n; i++) {
    SPI.transfer(hi);
    SPI.transfer(lo);
  }
  cs_high();
  SPI.endTransaction();
}

// Never returns. VERIFICATION in real UI coordinates (MADCTL + offsets):
//   RED, GREEN, BLUE fills (1.5 s)  -> correct colors? full glass, no noise?
//   white 2px BORDER on black (2.5 s) -> hugs all 4 edges? (offsets)
//   white block at origin (2.5 s)     -> which corner? (orientation)
static void display_selftest() {
  const uint16_t R = 0xF800, G = 0x07E0, B = 0x001F, W = 0xFFFF, K = 0x0000;
  for (;;) {
    st_fill(0, 0, DISP_W - 1, DISP_H - 1, R);
    delay(1500);
    st_fill(0, 0, DISP_W - 1, DISP_H - 1, G);
    delay(1500);
    st_fill(0, 0, DISP_W - 1, DISP_H - 1, B);
    delay(1500);
    st_fill(0, 0, DISP_W - 1, DISP_H - 1, K);
    st_fill(0, 0, DISP_W - 1, 1, W);
    st_fill(0, DISP_H - 2, DISP_W - 1, DISP_H - 1, W);
    st_fill(0, 0, 1, DISP_H - 1, W);
    st_fill(DISP_W - 2, 0, DISP_W - 1, DISP_H - 1, W);
    delay(2500);
    st_fill(0, 0, DISP_W - 1, DISP_H - 1, K);
    st_fill(0, 0, 79, 29, W); // origin (0,0) corner marker
    delay(2500);
  }
}
#endif

void display_init() {
  lv_init();
  lv_tick_set_cb(tick_cb);

  pinMode(PIN_OLED_CS, OUTPUT);
  digitalWrite(PIN_OLED_CS, HIGH);
  pinMode(PIN_OLED_DC, OUTPUT);
  digitalWrite(PIN_OLED_DC, HIGH);
  pinMode(PIN_OLED_BL, OUTPUT);
  digitalWrite(PIN_OLED_BL, LOW); // backlight off until the panel is ready
  pinMode(PIN_OLED_RST, OUTPUT);
  digitalWrite(PIN_OLED_RST, HIGH);
  delay(100);
  digitalWrite(PIN_OLED_RST, LOW);
  delay(120);
  digitalWrite(PIN_OLED_RST, HIGH);
  delay(120);

  SPI.begin();
  // Re-clock SERCOM2 from 120 MHz so 30 MHz SCK is reachable (48 MHz source
  // quantizes to 24 MHz max).
  SPI.setClockSource(SERCOM_CLOCK_SOURCE_FCPU); // 120 MHz (GCLK0)
  SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
  cs_low();

  wr_cmd(0x01); // SWRESET
  delay(150);

  for (const uint8_t *p = kInit; p[0] != 0xFF || p[1] != 0xFF;) {
    uint8_t cmd = *p++;
    uint8_t n = *p++;
    wr_cmd(cmd);
    if (n) {
      wr_data(p, n);
      p += n;
    }
    if (cmd == 0x11)
      delay(150); // SLPOUT settle (vendor uses 220 ms, Arduino_GFX 120)
  }

#if NV_INVERSION
  wr_cmd(0x21); // INVON (only if this glass shows negative colors)
#endif
  wr_cmd(0x29); // DISPON
  delay(120);

  // Clear the visible area to black, then hand off.
  set_window(0, 0, DISP_W - 1, DISP_H - 1);
  for (uint32_t i = 0; i < (uint32_t)DISP_W * DISP_H; i++) {
    SPI.transfer(0x00);
    SPI.transfer(0x00);
  }
  cs_high();
  SPI.endTransaction();

  // Backlight soft-start: a full-step turn-on spikes VBUS past even the
  // raised 500 mA limit and brownout-loops a battery-less board (the panel
  // caps charge at the same instant). Ramp the PWM over ~100 ms instead;
  // the UI applies the user's brightness right after boot anyway.
  for (int d = 0; d <= 160; d += 8) {
    analogWrite(PIN_OLED_BL, d);
    delay(5);
  }

#if DISPLAY_SELFTEST
  display_selftest(); // never returns — bring-up pattern test
#endif

  // DMA channel for the async pixel flush. On any allocation failure the
  // driver silently stays on the synchronous DRE-paced path.
  s_dma.setTrigger(SERCOM2_DMAC_ID_TX);
  s_dma.setAction(DMA_TRIGGER_ACTON_BEAT);
  if (s_dma.allocate() == DMA_STATUS_OK) {
    s_dmaDesc = s_dma.addDescriptor(
        s_buf, (void *)&SERCOM2->SPI.DATA.reg, 1, DMA_BEAT_SIZE_BYTE,
        true /*src increments*/, false /*fixed dst*/);
    if (s_dmaDesc) {
      s_dma.setCallback(dma_done_cb);
      s_dmaReady = true;
    }
  }

  s_disp = lv_display_create(DISP_W, DISP_H);
  lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(s_disp, flush_cb);
  lv_display_set_buffers(s_disp, s_buf, s_buf2, sizeof(s_buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
}

void display_set_contrast(uint8_t c) {
  // Backlight brightness via PWM on D11/PA19 (TCC1_CH3). 0 = off, 255 = full.
  analogWrite(PIN_OLED_BL, c);
}

void display_power(bool on) {
  dma_wait_idle(); // don't jam a command into an in-flight pixel stream
  SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
  cs_low();
  wr_cmd(on ? 0x29 : 0x28); // DISPON / DISPOFF
  cs_high();
  SPI.endTransaction();
  if (!on)
    analogWrite(PIN_OLED_BL, 0);
}

void display_task() {}

#endif // !DISPLAY_ST7789
