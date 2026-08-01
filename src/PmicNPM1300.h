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
  int16_t ibatMa;      // battery current, mA: + charging, - discharging
  int16_t dieTempCx10; // PMIC die temperature, 0.1 C units
  uint8_t chargeStatus; // raw BCHGCHARGESTATUS (see pmic_charge_text)
};

// Low-battery policy thresholds (only enforced when VBUS is absent — on a
// connected charger the clock always boots and runs):
#define PMIC_VBAT_SHIP_MV 3400     // runtime cutoff -> ship mode
#define PMIC_VBAT_BOOT_MIN_MV 3450 // boot gate (hysteresis above cutoff)

void pmic_begin();    // detect; set VBUS limit from CC; start charger
// Re-read the USB-C CC advertisement and set the input limit: 500 mA on
// Default USB/PC ports (hard rule), 1500 mA on detected 1.5/3 A sources.
// MUST be called on every VBUS re-attach — the limit resets to 100 mA.
void pmic_vbus_reconfigure();
bool pmic_vbus_limit_ok();     // last limit write verified
uint16_t pmic_vbus_limit_ma(); // active input limit (500 or 1500)
// Service the PMIC's IRQ line (GPIO0 -> PA04, configured in pmic_begin):
// acks pending events; a VBUS attach re-applies the input-limit config.
void pmic_handle_irq();
// Cut the battery from VSYS (<500 nA). Wake: BUTTON1 (SHPHLD) or USB plug.
// The RTC keeps time through this (its VBACKUP sits directly on VBAT).
// Does not return when VBUS is absent.
void pmic_enter_ship_mode();
bool pmic_present();  // true when an nPM1300 ACKed at boot
uint16_t pmic_charge_current_ma();     // configured charge current
uint16_t pmic_vterm_mv();              // configured termination voltage
bool pmic_read_status(PmicStatus &out); // live measurement (~2 ms)
const char *pmic_charge_text(uint8_t chargeStatus);
int pmic_soc_percent(uint16_t vbatMv, int16_t ibatMa); // IR-compensated SoC 0..100
