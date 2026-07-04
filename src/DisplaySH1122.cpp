// DisplaySH1122.cpp — SH1122 256x64 4-bit grayscale OLED driver + LVGL 9 glue.
#include "DisplaySH1122.h"
#include "pins.h"
#include <SPI.h>

static const uint32_t kSpiHz = 8000000;

// Partial-render buffer: half the screen, RGB565 (2 bytes/pixel).
// Must be aligned to LV_DRAW_BUF_ALIGN (4) — lv_display_set_buffers asserts
// on it, and a failed LVGL assert hangs in while(1).
static uint8_t s_buf[DISP_W * (DISP_H / 2) * 2] __attribute__((aligned(4)));
static uint8_t s_line[DISP_W / 2]; // one packed panel row, 2 px/byte
static lv_display_t *s_disp;

static void spi_cmds(const uint8_t *c, size_t n) {
  SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_OLED_CS, LOW);
  digitalWrite(PIN_OLED_DC, LOW);
  for (size_t i = 0; i < n; i++)
    SPI.transfer(c[i]);
  digitalWrite(PIN_OLED_CS, HIGH);
  SPI.endTransaction();
}

static void spi_cmd1(uint8_t c) { spi_cmds(&c, 1); }

// RGB565 (little-endian pair) -> 4-bit luma nibble.
static inline uint8_t luma4_565(uint8_t lo, uint8_t hi) {
  uint16_t v = (uint16_t)lo | ((uint16_t)hi << 8);
  uint8_t r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
  uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
  uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
  uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));
  uint32_t luma8 = ((uint32_t)r * 77 + (uint32_t)g * 150 + (uint32_t)b * 29) >> 8;
  return (uint8_t)(luma8 >> 4);
}

// v9 replaces the rounder_cb with an INVALIDATE_AREA event. Column address
// unit is 2 pixels: keep the invalidated area even-aligned in x.
static void invalidate_area_cb(lv_event_t *e) {
  lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
  area->x1 &= ~1;
  area->x2 |= 1;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
  const lv_coord_t w = area->x2 - area->x1 + 1; // even (rounder guarantees)
  const size_t nbytes = (size_t)w / 2;

  // Software 180 rotation: panel stays in native 0xA0/0xC0 (a config we know
  // renders cleanly with no offset). Map LVGL (x,y) -> panel (W-1-x, H-1-y).
  const uint8_t col = (uint8_t)((uint16_t)(DISP_W - 1 - area->x2) >> 1);
  const uint8_t col_lo = 0x00 | (col & 0x0F);
  const uint8_t col_hi = 0x10 | (col >> 4);

  SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_OLED_CS, LOW);
  for (lv_coord_t y = area->y1; y <= area->y2; y++) {
    const uint8_t *rp = px + (size_t)(y - area->y1) * w * 2; // RGB565 row
    uint8_t *dst = s_line;
    // Reverse pixel order within the row (horizontal flip). High nibble =
    // LVGL pixel at the far (x2) end.
    for (lv_coord_t i = 0; i < w; i += 2) {
      const uint8_t *pH = rp + (size_t)(w - 1 - i) * 2;
      const uint8_t *pL = rp + (size_t)(w - 2 - i) * 2;
      uint8_t hi = luma4_565(pH[0], pH[1]);
      uint8_t lo = luma4_565(pL[0], pL[1]);
      *dst++ = (uint8_t)((hi << 4) | lo);
    }
    const uint8_t prow = (uint8_t)(DISP_H - 1 - y); // vertical flip
    digitalWrite(PIN_OLED_DC, LOW);
    SPI.transfer(0xB0);         // set row address (two-byte cmd)
    SPI.transfer(prow);         // flipped row 0..63
    SPI.transfer(col_lo);       // lower column nibble
    SPI.transfer(col_hi);       // higher column nibble
    digitalWrite(PIN_OLED_DC, HIGH);
    // Synchronous byte-by-byte: each transfer() returns only after the byte
    // has fully shifted out, so DC can never drop while data is still on the
    // wire (a DMA transfer returns ~2 bytes early and corrupts the next row
    // command -> whole UI collapses to one top line). ~8 ms per full refresh.
    for (size_t i = 0; i < nbytes; i++)
      SPI.transfer(s_line[i]);
  }
  digitalWrite(PIN_OLED_CS, HIGH);
  SPI.endTransaction();
  lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void) { return millis(); }

void display_init() {
  lv_init();
  lv_tick_set_cb(tick_cb); // v9: no LV_TICK_CUSTOM; feed the tick from millis

  pinMode(PIN_OLED_CS, OUTPUT);
  digitalWrite(PIN_OLED_CS, HIGH);
  pinMode(PIN_OLED_DC, OUTPUT);
  digitalWrite(PIN_OLED_DC, LOW);
  pinMode(PIN_OLED_BL, OUTPUT);
  digitalWrite(PIN_OLED_BL, HIGH);
  pinMode(PIN_OLED_RST, OUTPUT);
  digitalWrite(PIN_OLED_RST, LOW);
  delay(12);
  digitalWrite(PIN_OLED_RST, HIGH);
  delay(12);

  SPI.begin();

  static const uint8_t init_cmds[] = {
      0xAE,       // display off
      0x40,       // start line 0
      0xA0,       // segment remap normal (native; 180 done in software)
      0xC0,       // COM scan normal      (native; 180 done in software)
      0x81, 0x80, // contrast
      0xA8, 0x3F, // multiplex 64
      0xAD, 0x81, // DC-DC on
      0xD5, 0x50, // clock divide
      0xD3, 0x00, // display offset
      0xD9, 0x22, // precharge
      0xDB, 0x35, // VCOM
      0xDC, 0x35, // VSEG precharge
      0x30,       // discharge level
      0xA4,       // RAM display
      0xA6,       // normal (non-inverted)
  };
  spi_cmds(init_cmds, sizeof(init_cmds));

  // Clear GRAM row by row (8192 zero bytes total)
  SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_OLED_CS, LOW);
  for (uint8_t y = 0; y < DISP_H; y++) {
    digitalWrite(PIN_OLED_DC, LOW);
    SPI.transfer(0xB0);
    SPI.transfer(y);
    SPI.transfer(0x00);
    SPI.transfer(0x10);
    digitalWrite(PIN_OLED_DC, HIGH);
    for (uint16_t i = 0; i < DISP_W / 2; i++)
      SPI.transfer((uint8_t)0x00);
  }
  digitalWrite(PIN_OLED_CS, HIGH);
  SPI.endTransaction();

  spi_cmd1(0xAF); // display on
  delay(100);     // SH1122 datasheet: wait for panel after display-on

  s_disp = lv_display_create(DISP_W, DISP_H);
  lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(s_disp, flush_cb);
  lv_display_set_buffers(s_disp, s_buf, NULL, sizeof(s_buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_add_event_cb(s_disp, invalidate_area_cb, LV_EVENT_INVALIDATE_AREA,
                          NULL);
}

void display_set_contrast(uint8_t c) {
  uint8_t b[2] = {0x81, c};
  spi_cmds(b, 2);
}

void display_power(bool on) { spi_cmd1(on ? 0xAF : 0xAE); }

void display_task() {}
