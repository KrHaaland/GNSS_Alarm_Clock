// DisplayST7789.cpp — ST7789 color TFT driver + LVGL 9 glue. See header.
#include "DisplayST7789.h"
#include "pins.h"
#include <SPI.h>

// 24 MHz. The display path is confirmed working now (the earlier "24 MHz
// corrupts the init" was actually the window-offset bug plus a bad connector,
// both fixed). If garbage/flicker ever returns over the jumper wiring, step
// this back down (16/12 MHz).
static const uint32_t kSpiHz = 24000000;

// Bring-up aid: when 1, display_init() runs an on-glass pattern self-test
// (solid RGBW fills, L/R + T/B splits, a 1px edge border) in an infinite loop
// INSTEAD of starting LVGL. This isolates the low-level SPI / pixel-format /
// offset path from anything in LVGL/UI. Set back to 0 for normal operation.
#define DISPLAY_SELFTEST 0

// --- Panel-specific: tune these on hardware if the image is wrong -----------
// MADCTL for landscape. Bit5 MV (row/col exchange), bit6 MX, bit7 MY, bit3 RGB
// order (0=RGB,1=BGR). If mirrored/upside-down, try 0x60 / 0xA0 / 0xC0 / 0x70.
#define ST_MADCTL 0x60
// This panel shows INVERTED colors with INVON (on the bench black came out
// white and green came out magenta), so it needs inversion OFF.
#define ST_INVERSION 0
// Visible-window offsets: the 284x76 glass is a centered sub-window of the
// ST7789's 320x240 (landscape) GRAM. X (the 284 axis) = (320-284)/2 = 18;
// Y (the 76 axis) = (240-76)/2 = 82. Fine-tune with the border self-test so the
// white frame hugs all four physical edges.
#define ST_X_OFFSET 18
#define ST_Y_OFFSET 82

// Partial-render buffer: 24 rows, RGB565 (2 bytes/px). Aligned for LVGL 9.
static uint8_t s_buf[DISP_W * 24 * 2] __attribute__((aligned(4)));
static lv_display_t *s_disp;

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

// SERCOM2 is the SPI peripheral (Metro M4 variant PERIPH_SPI).
static inline void spi_drain_tx() {
  while (!SERCOM2->SPI.INTFLAG.bit.TXC) {
  }
}

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  x0 += ST_X_OFFSET;
  x1 += ST_X_OFFSET;
  y0 += ST_Y_OFFSET;
  y1 += ST_Y_OFFSET;
  uint8_t c[4];
  wr_cmd(0x2A); // CASET (columns)
  c[0] = x0 >> 8; c[1] = x0 & 0xFF; c[2] = x1 >> 8; c[3] = x1 & 0xFF;
  wr_data(c, 4);
  wr_cmd(0x2B); // RASET (rows)
  c[0] = y0 >> 8; c[1] = y0 & 0xFF; c[2] = y1 >> 8; c[3] = y1 & 0xFF;
  wr_data(c, 4);
  wr_cmd(0x2C); // RAMWR
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
  const uint32_t px_count =
      (uint32_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);

  // LVGL RGB565 is little-endian; the ST7789 wants MSB first -> swap in place.
  lv_draw_sw_rgb565_swap(px, px_count);

  SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE3));
  cs_low();
  set_window((uint16_t)area->x1, (uint16_t)area->y1, (uint16_t)area->x2,
             (uint16_t)area->y2);
  // Synchronous byte stream (NO DMA). The DMA 4-arg transfer fires a DMA
  // channel-1 completion IRQ that has no handler installed -> Dummy_Handler
  // infinite loop -> the whole MCU freezes (no USB, no display). Per-byte
  // SPI.transfer blocks per byte and needs no DMA; ~16 ms for a full flush.
  const uint32_t nbytes = px_count * 2;
  for (uint32_t i = 0; i < nbytes; i++)
    SPI.transfer(px[i]);
  cs_high();
  SPI.endTransaction();
  lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void) { return millis(); }

static const uint8_t kInit[] = {
    // cmd, num-args, args...
    0x11, 0,                        // SLPOUT (handled with delay below)
    0x3A, 1, 0x55,                  // COLMOD = 16-bit RGB565
    0x36, 1, ST_MADCTL,             // MADCTL orientation
    0xB2, 5, 0x0C, 0x0C, 0x00, 0x33, 0x33, // PORCTRL
    0xB7, 1, 0x35,                  // GCTRL
    0xBB, 1, 0x19,                  // VCOMS
    0xC0, 1, 0x2C,                  // LCMCTRL
    0xC2, 1, 0x01,                  // VDVVRHEN
    0xC3, 1, 0x12,                  // VRHS
    0xC4, 1, 0x20,                  // VDVSET
    0xC6, 1, 0x0F,                  // FRCTR2 (~60Hz)
    0xD0, 2, 0xA4, 0xA1,            // PWCTRL1
    0xE0, 14, 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18,
              0x0D, 0x0B, 0x1F, 0x23, // PVGAMCTRL
    0xE1, 14, 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F,
              0x1F, 0x1F, 0x20, 0x23, // NVGAMCTRL
    0xFF, 0xFF, // end marker
};

#if DISPLAY_SELFTEST
// Fill an inclusive rectangle with a solid RGB565 color (big-endian to panel).
static void st_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                    uint16_t color) {
  SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE3));
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

// Set MADCTL directly (bypasses the init value) for orientation experiments.
static void st_madctl(uint8_t v) {
  SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE3));
  cs_low();
  wr_cmd(0x36);
  SPI.transfer(v);
  cs_high();
  SPI.endTransaction();
}

// Never returns. OFFSET/COLOR VERIFICATION: draws in the real UI coordinate
// system (init MADCTL, DISP_W x DISP_H, ST_X/Y_OFFSET applied by set_window).
// Loop:
//   RED, GREEN, BLUE full-screen (1.5 s each) -> colors correct? full glass?
//   white 2px BORDER on black    (2.5 s)      -> frame should hug all 4 edges
//   white block at the (0,0)     (2.5 s)      -> marks the UI origin corner
static void display_selftest() {
  const uint16_t R = 0xF800, G = 0x07E0, B = 0x001F, W = 0xFFFF, K = 0x0000;
  st_madctl(ST_MADCTL); // real UI orientation
  for (;;) {
    st_fill(0, 0, DISP_W - 1, DISP_H - 1, R);
    delay(1500);
    st_fill(0, 0, DISP_W - 1, DISP_H - 1, G);
    delay(1500);
    st_fill(0, 0, DISP_W - 1, DISP_H - 1, B);
    delay(1500);

    st_fill(0, 0, DISP_W - 1, DISP_H - 1, K);
    st_fill(0, 0, DISP_W - 1, 1, W);                   // top
    st_fill(0, DISP_H - 2, DISP_W - 1, DISP_H - 1, W); // bottom
    st_fill(0, 0, 1, DISP_H - 1, W);                   // left
    st_fill(DISP_W - 2, 0, DISP_W - 1, DISP_H - 1, W); // right
    delay(2500);

    st_fill(0, 0, DISP_W - 1, DISP_H - 1, K);
    st_fill(0, 0, 59, 19, W); // origin (0,0) corner marker
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
  delay(10);
  digitalWrite(PIN_OLED_RST, LOW);
  delay(10);
  digitalWrite(PIN_OLED_RST, HIGH);
  delay(120);

  SPI.begin();
  SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE3));
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
      delay(120); // SLPOUT needs settle time
  }

#if ST_INVERSION
  wr_cmd(0x21); // INVON
#else
  wr_cmd(0x20); // INVOFF
#endif
  wr_cmd(0x13); // NORON
  delay(10);
  wr_cmd(0x29); // DISPON
  delay(20);

  // Clear the visible area to black, then hand off to LVGL.
  set_window(0, 0, DISP_W - 1, DISP_H - 1);
  for (uint32_t i = 0; i < (uint32_t)DISP_W * DISP_H; i++) {
    SPI.transfer(0x00);
    SPI.transfer(0x00);
  }
  cs_high();
  SPI.endTransaction();

  digitalWrite(PIN_OLED_BL, HIGH); // backlight on

#if DISPLAY_SELFTEST
  display_selftest(); // never returns — bring-up pattern test
#endif

  s_disp = lv_display_create(DISP_W, DISP_H);
  lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(s_disp, flush_cb);
  lv_display_set_buffers(s_disp, s_buf, NULL, sizeof(s_buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
}

void display_set_contrast(uint8_t c) {
  // Backlight brightness via PWM on D11/PA19 (TCC1_CH3). 0 = off, 255 = full.
  analogWrite(PIN_OLED_BL, c);
}

void display_power(bool on) {
  SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE3));
  cs_low();
  wr_cmd(on ? 0x29 : 0x28); // DISPON / DISPOFF
  spi_drain_tx();
  cs_high();
  SPI.endTransaction();
  if (!on)
    analogWrite(PIN_OLED_BL, 0);
}

void display_task() {}
