// pins.h — Board pin map for the GNSS Alarm Clock (custom SAMD51J19A board,
// Adafruit Metro M4 compatible variant).
//
// Verified against the v2 KiCad design (HARDWARE/, see HARDWARE_V2.md for the
// full 64-pad reference) and cross-checked against HARDWARE_OLD/ (v1). The map
// is near-identical between v1 and v2; the v2-only differences are:
//   - PA04 = nPM1300 GPIO0 (was LTC3226 CAPGOOD; unconfigured reads LOW)
//   - PB16 unconnected (escalation buzzer removed)
//   - PB01 unconnected (24LC512 EEPROM removed)
//   - BUTTON1 (PB14) is driven by a 74LVC1G17 buffer (U9) whose input is the
//     PMIC SHPHLD pin, not a raw switch; the physical SW1 doubles as the
//     nPM1300 wake/reset. Same active-low polarity as v1 at the MCU.
// Arduino pin numbers are the adafruit_metro_m4 variant (verified against
// variant.cpp / variant.h). "direct port" = pin is NOT in that variant, so it
// needs raw PORT register access (see portb_*() helpers below).
//
//  SAMD51 pin | Net         | Arduino | Function
//  -----------+-------------+---------+---------------------------------------
//  PA02       | Analog      | 14/A0   | Audio out (DAC0) -> C27 -> TPA2016 INR-
//  PA04       | PMIC_GPIO0  | 17/A3   | nPM1300 GPIO0 (v1: LTC3226 CAPGOOD)
//  PA06       | LedsR-S     | 16/A2   | Right LED section gate -> 470R -> U12
//  PA12       | SPI_MOSI    | 26/MOSI | Display SPI MOSI (SERCOM2) -> J4.5
//  PA13       | SPI_SCK     | 25/SCK  | Display SPI SCK  (SERCOM2) -> J4.6
//  PA14       | SPI_MISO    | 24/MISO | SPI MISO (not routed to J4 panel)
//  PA16       | DISP_RST    | 13      | display reset (active low), J4.4
//  PA17       | DISP_DC     | 12      | display data/command, J4.3
//  PA18       | DISP_CS     | 10      | display chip select (active low), J4.2
//  PA19       | DISP_BL     | 11      | display aux/backlight output, J4.1
//  PA20       | LedsB-S     |  9      | Bottom LED section gate -> 470R -> U11
//  PA21       | LedsL-S     |  8      | Left LED section gate  -> 470R -> U10
//  PA22       | McuTX       |  1      | Serial1 TX -> L86 GNSS RX (U4.1)
//  PA23       | McuRX       |  0      | Serial1 RX <- L86 GNSS TX (U4.2)
//  PB02       | I2C_SDA     | 22/SDA  | RV-3028 (0x52), TPA2016 (0x58),
//  PB03       | I2C_SCL     | 23/SCL  | LIS3DH (0x18), nPM1300 (0x6B); 4.7k PU
//  PB08       | LIS3DH_INT1 | 18/A4   | Accelerometer interrupt 1 (U7.11)
//  PB12       | BUTTON3     |  7      | Button 3 (active low, 1M pullup R9)
//  PB13       | BUTTON2     |  4      | Button 2 (active low, 1M pullup R8)
//  PB14       | BUTTON1     |  5      | Button 1 (active low; via U9 buffer,
//             |             |         | input = PMIC SHPHLD)
//  PB15       | BUTTON4     |  6      | Button 4 (active low, 1M pullup R10)
//  PB16       | (unconn.)   |  3      | v2: unconnected (v1: SPEAKER buzzer)
//  PB17       | AmpShutdown |  2      | TPA2016 ~SD (HIGH = amp on, 10k PU R68)
//  PA08..11,  | Flash IO    | QSPI    | U8 tune flash. Supported: W25Q128JV
//  PB10,PB11  |             |         | (3V) or MX25L3233F. NB: first v2
//             |             |         | board has a 1.8V W25Q128FW — wrong
//             |             |         | part, storage disabled (HARDWARE_V2)
//  --- not in the Metro M4 variant: direct PORT access ---
//  PB00       | GNSS_FOn    |  -      | L86 FORCE_ON (U4.7)
//  PB31       | GNSS_Reset  |  -      | L86 RESET_N (U4.10), active low
//  PB01       | (unconn.)   |  -      | v2: unconnected (v1: 24LC512 WP)
//  PA00/PA01  | 32.768 kHz  |  -      | X1 crystal (XOSC32K) — not GPIO
//
// Buttons sit in a row above the display, left to right: SW1 SW2 SW3 SW4.

#pragma once
#include <Arduino.h>

// --- Display (NV3007 428x142 landscape RGB565 TFT, 4-wire SPI on J4) ---
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
// v2: LED anodes fed from +5V (TPS61023 boost) via 100R each (R20-R53);
// each gate has a 470R series resistor + 10k pulldown. (v1 fed from the
// supercap ALARMPOWER rail.)
#define PIN_LEDS_LEFT 8    // PA21, 10 LEDs (LED1-10),  gate R15
#define PIN_LEDS_BOTTOM 9  // PA20, 14 LEDs (LED11-24), gate R54
#define PIN_LEDS_RIGHT A2  // PA06, 10 LEDs (LED25-34), gate R56

// --- Audio ---
#define PIN_AUDIO_DAC PIN_DAC0 // PA02, AC-coupled into TPA2016 right input
#define PIN_AMP_SHUTDOWN 2     // PB17, HIGH = amp on (TPA2016 ~SD, 10k PU R68)
#define PIN_BUZZER 3           // PB16, v2: UNCONNECTED (buzzer removed);
                               // v1 gated J5 load on the supercap rail

// --- Power / status ---
// v2: PA04 is nPM1300 GPIO0 (not a supercap CAPGOOD). Kept as PIN_CAPGOOD
// for v1 source compatibility; on v2 it reads the (unconfigured) PMIC GPIO0.
#define PIN_CAPGOOD A3 // PA04, v2: nPM1300 GPIO0 (v1: LTC3226 CAPGOOD)
#define PIN_ACCEL_INT1 A4 // PB08, LIS3DH INT1

// --- I2C addresses ---
#define I2C_ADDR_RTC 0x52    // RV-3028-C7
#define I2C_ADDR_AMP 0x58    // TPA2016D2
#define I2C_ADDR_EEPROM 0x50 // 24LC512 — v2: NOT FITTED (v1 only)
#define I2C_ADDR_PMIC 0x6B   // nPM1300 PMIC (v2 only)
#define I2C_ADDR_ACCEL 0x18  // LIS3DH; v2 straps SA0 low (R18 10k->GND) => 0x18
                             // by design, but boards have answered at 0x19 too
                             // -> the driver probes both

// --- Pins not present in the Metro M4 variant: direct PORT access ---
// PORTB = PORT->Group[1]
#define GNSS_FON_PORTPIN 0    // PB00, L86 FORCE_ON (leave hi-Z normally)
#define EEPROM_WP_PORTPIN 1   // PB01, v2: UNCONNECTED (v1: 24LC512 WP)
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
