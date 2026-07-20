// DisplayNV3007.h — NV3007 color TFT (RGB565), used LANDSCAPE 428x142.
// Native panel is 142x428 (portrait bar); rotated 90 via MADCTL. Same 4-wire
// SPI wiring as the old ST7789 (CS/DC/RST + MOSI/SCK on SERCOM2, backlight
// on PIN_OLED_BL / D11, PWM-dimmable via display_set_contrast()).
//
// LVGL renders RGB565 directly; the flush byte-swaps to the panel's
// big-endian order and streams it. MADCTL / inversion / window offsets are
// panel-specific and tuned by the NV_* defines in the .cpp.
#pragma once
#include <Arduino.h>
#include <lvgl.h>

#define DISP_W 428
#define DISP_H 142

void display_init();                  // HW init + LVGL display registration
void display_set_contrast(uint8_t c); // 0..255 -> backlight brightness (PWM)
void display_power(bool on);          // display on/off (+ backlight)
void display_task();                  // periodic upkeep (none)
