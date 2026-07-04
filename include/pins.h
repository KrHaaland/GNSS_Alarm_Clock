// pins.h — Board pin map for the GNSS Alarm Clock (custom SAMD51J19A board,
// Adafruit Metro M4 compatible variant).
//
// Extracted from HARDWARE/samd51_gps_alarm_clock (KiCad) production netlist.
// Arduino pin numbers refer to the adafruit_metro_m4 variant.
//
//  SAMD51 pin | Net         | Arduino | Function
//  -----------+-------------+---------+---------------------------------------
//  PA02       | ANALOG      | A0/DAC0 | Audio out -> C27 -> TPA2016D2 INR-
//  PA04       | CAPGOOD     | A3      | LTC3226 CAPGD (open drain, 10k pullup,
//             |             |         | HIGH = supercaps charged)
//  PA06       | LEDSR-S     | A2      | Right LED section gate (IRLML6344)
//  PA12/13/14 | SPI         | MOSI/SCK/MISO (SERCOM2) -> display J3
//  PA16       | OLED_RST    | D13     | SH1122 reset (active low)
//  PA17       | OLED_DC     | D12     | SH1122 data/command
//  PA18       | OLED_CS     | D10     | SH1122 chip select (active low)
//  PA19       | OLED_BL     | D11     | Display aux/backlight output
//  PA20       | LEDSB-S     | D9      | Bottom LED section gate
//  PA21       | LEDSL-S     | D8      | Left LED section gate
//  PA22       | MCUTX       | D1      | Serial1 TX -> L86 GNSS RX
//  PA23       | MCURX       | D0      | Serial1 RX <- L86 GNSS TX
//  PB02/PB03  | I2C_SDA/SCL | SDA/SCL | RV-3028 (0x52), TPA2016 (0x58),
//             |             |         | 24LC512 (0x50), LIS3DH (0x18)
//  PB08       | LIS3DH_INT1 | A4      | Accelerometer interrupt 1
//  PB12       | BUTTON3     | D7      | Button 3 (active low, 1M ext. pullup)
//  PB13       | BUTTON2     | D4      | Button 2 (active low)
//  PB14       | BUTTON1     | D5      | Button 1 (active low)
//  PB15       | BUTTON4     | D6      | Button 4 (active low)
//  PB16       | SPEAKER     | D3      | Buzzer MOSFET gate (J5 load on
//             |             |         | ALARMPOWER rail) - tone() capable
//  PB17       | AMPSHUTDOWN | D2      | TPA2016 ~SD (HIGH = amp enabled,
//             |             |         | 10k pullup)
//  PA08..11,  | FLASH-IO    | QSPI    | MX25L3233F 4MB flash (tune storage)
//  PB10,PB11  |             |         |
//  PB00       | GNSS_FON    |  -      | L86 FORCE_ON (direct port access)
//  PB31       | GNSS_RESET  |  -      | L86 RESET, active low (direct port)
//  PB01       | EEPROM_WP   |  -      | 24LC512 write protect (direct port)
//
// Buttons sit in a row above the display, left to right: SW1 SW2 SW3 SW4.

#pragma once
#include <Arduino.h>

// --- Display (SH1122 256x64, 4-bit grayscale, 4-wire SPI on J3) ---
#define PIN_OLED_CS 10
#define PIN_OLED_DC 12
#define PIN_OLED_RST 13
#define PIN_OLED_BL 11

// --- Buttons, left to right as mounted above the display ---
#define PIN_BUTTON1 5 // PB14 (SW1, leftmost)
#define PIN_BUTTON2 4 // PB13 (SW2)
#define PIN_BUTTON3 7 // PB12 (SW3)
#define PIN_BUTTON4 6 // PB15 (SW4, rightmost)

// --- LED sections (gates of low-side MOSFETs, HIGH = section on) ---
// LED anodes are fed from ALARMPOWER (supercap rail) via 80R resistors.
#define PIN_LEDS_LEFT 8    // PA21, 10 LEDs
#define PIN_LEDS_BOTTOM 9  // PA20, 14 LEDs
#define PIN_LEDS_RIGHT A2  // PA06, 10 LEDs

// --- Audio ---
#define PIN_AUDIO_DAC PIN_DAC0 // PA02, AC-coupled into TPA2016D2 right input
#define PIN_AMP_SHUTDOWN 2     // PB17, HIGH = amp on (TPA2016 ~SD)
#define PIN_BUZZER 3           // PB16, gates J5 load on the supercap rail

// --- Power / status ---
#define PIN_CAPGOOD A3 // PA04, HIGH = LTC3226 reports supercaps charged
#define PIN_ACCEL_INT1 A4 // PB08, LIS3DH INT1

// --- I2C addresses ---
#define I2C_ADDR_RTC 0x52    // RV-3028-C7
#define I2C_ADDR_AMP 0x58    // TPA2016D2
#define I2C_ADDR_EEPROM 0x50 // 24LC512 (unused, reserved)
#define I2C_ADDR_ACCEL 0x18  // LIS3DH (SA0 low)

// --- Pins not present in the Metro M4 variant: direct PORT access ---
// PORTB = PORT->Group[1]
#define GNSS_FON_PORTPIN 0    // PB00, L86 FORCE_ON (leave hi-Z normally)
#define EEPROM_WP_PORTPIN 1   // PB01, 24LC512 WP
#define GNSS_RESET_PORTPIN 31 // PB31, L86 RESET_N (active low, keep hi-Z)

static inline void portb_output(uint8_t pin, bool level) {
  if (level)
    PORT->Group[1].OUTSET.reg = (1ul << pin);
  else
    PORT->Group[1].OUTCLR.reg = (1ul << pin);
  PORT->Group[1].DIRSET.reg = (1ul << pin);
}

static inline void portb_hiz(uint8_t pin) {
  PORT->Group[1].DIRCLR.reg = (1ul << pin);
  PORT->Group[1].PINCFG[pin].reg = 0; // no pull, input buffer off
}
