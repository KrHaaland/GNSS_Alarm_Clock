// PmicNPM1300.h — nPM1300 PMIC on the v2 board (I2C 0x6B): Li-ion charger,
// bucks, USB-C VBUS input. One firmware serves both board revisions: v1 has
// no PMIC and every call degrades to a no-op / false.
//
// CRITICAL on v2: the PMIC powers up with its VBUS input current limit at
// 100 mA and holds it there until the host raises it — a battery-less board
// browns out the moment a bigger load (an LED section) switches on. The
// limit also RESETS to 100 mA on every VBUS replug, so pmic_begin() must run
// early in every boot. pmic_begin() also configures and enables the Li-ion
// charger (200 mA, 4.2 V termination — see the .cpp for the reasoning).
#pragma once
#include <Arduino.h>

struct PmicStatus {
  bool vbusPresent;    // USB power attached
  uint16_t vbatMv;     // battery voltage, mV
  int16_t dieTempCx10; // PMIC die temperature, 0.1 C units
  uint8_t chargeStatus; // raw BCHGCHARGESTATUS (see pmic_charge_text)
};

void pmic_begin();    // detect; raise VBUS limit to 500 mA; start charger
bool pmic_present();  // true when an nPM1300 ACKed at boot
bool pmic_vbus_500(); // true when the 500 mA limit was applied OK
uint16_t pmic_charge_current_ma();     // configured charge current
uint16_t pmic_vterm_mv();              // configured termination voltage
bool pmic_read_status(PmicStatus &out); // live measurement (~2 ms)
const char *pmic_charge_text(uint8_t chargeStatus);
