// DisplayST7789.h — ST7789 color TFT (RGB565), used LANDSCAPE 284x76.
// Native panel is 76x284 (portrait); rotated 90 via MADCTL. 4-wire SPI on J3
// (CS/DC/RST + MOSI/SCK on SERCOM2). Backlight (BLK) on PIN_OLED_BL (D11),
// PWM-dimmable via display_set_contrast().
//
// LVGL renders RGB565 directly, so the flush just byte-swaps to the panel's
// big-endian order and streams it — no grayscale conversion (unlike the old
// SH1122). MADCTL / inversion / column+row offsets are panel-specific and
// tuned by the ST_* defines in the .cpp.
#pragma once
#include <Arduino.h>
#include <lvgl.h>

#define DISP_W 284
#define DISP_H 76

void display_init();                  // HW init + LVGL display registration
void display_set_contrast(uint8_t c); // 0..255 -> backlight brightness (PWM)
void display_power(bool on);          // display on/off (+ backlight)
void display_task();                  // periodic upkeep (none)
