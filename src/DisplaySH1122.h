// DisplaySH1122.h — SH1122 256x64 4-bit grayscale OLED, 4-wire SPI (J3).
// SPI: SERCOM2 (Arduino SPI object) @ 8 MHz, CS/DC/RST per pins.h.
// Provides LVGL glue: display_init() registers an LVGL driver whose flush
// callback converts RGB565 -> 4-bit luma (2 pixels per byte) and writes a
// windowed update (column addresses are 2-pixel granular; the flush aligns
// x to even coordinates).
//
// SH1122 command notes (for the implementation):
//   0xAE off / 0xAF on, 0x40|line start line, 0xA0/0xA1 segment remap,
//   0xC0/0xC8 COM scan dir, 0x81 <val> contrast, 0xA8 <0x3F> multiplex,
//   0xAD <0x81> DC-DC on, 0xD5 <0x50> clock divide, 0xD3 <0x00> offset,
//   0xD9 <0x22> precharge, 0xDB <0x35> VCOM, 0xDC <0x35> VSEG precharge,
//   0x30 discharge level, 0xA4 RAM display, 0xA6 normal.
//   Addressing: 0xB0 <row> sets row (0..63); column nibble pair via
//   0x00|(col&0x0F) and 0x10|(col>>4), col in 0..127 (2 px per column).
#pragma once
#include <Arduino.h>
#include <lvgl.h>

#define DISP_W 256
#define DISP_H 64

void display_init();                  // HW init + LVGL driver registration
void display_set_contrast(uint8_t c); // 0..255 (brightness)
void display_power(bool on);          // 0xAE/0xAF
void display_task();                  // periodic upkeep (currently none)
