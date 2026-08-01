// PmicNPM1300.cpp — nPM1300 support: VBUS current limit, Li-ion charger
// config, and status readout for the Battery screen. Register offsets and
// encodings cross-checked against Zephyr's npm13xx drivers.
#include "PmicNPM1300.h"
#include <Wire.h>

#define NPM_ADDR 0x6B

// VBUSIN group (base 0x02)
#define NPM_TASKUPDATEILIMSW 0x0200 // write 1: apply VBUSINILIM0
#define NPM_VBUSINILIM0 0x0201      // 1 = 100 mA (power-up default), 5 = 500 mA
#define NPM_VBUSINSTATUS 0x0207     // b0 VBUSINPRESENT
#define NPM_ILIM_500MA 5

// BCHARGER group (base 0x03)
#define NPM_BCHGENABLESET 0x0304   // b0 = enable charging
#define NPM_BCHGISETMSB 0x0308     // charge current index = mA/2; MSB = idx/2
#define NPM_BCHGISETLSB 0x0309     // LSB = idx & 1
#define NPM_BCHGVTERM 0x030C       // 4 = 4.00 V .. 13 = 4.45 V (50 mV steps)
#define NPM_BCHGCHARGESTATUS 0x0334
// The board's NTC pin has a fixed 10 kOhm to GND (no thermistor in the
// battery): the charger reads a constant ~25 C, i.e. temperature limits are
// effectively bypassed. 10 kOhm is the type we must tell the ADC about.
#define NPM_ADCNTCRSEL 0x050A // 1 = 10k NTC

// ADC group (base 0x05)
#define NPM_TASKVBATMEASURE 0x0500
#define NPM_TASKTEMPMEASURE 0x0502 // die temperature
#define NPM_ADCVBATRESULTMSB 0x0511
#define NPM_ADCTEMPRESULTMSB 0x0513
#define NPM_ADCGP0RESULTLSBS 0x0515 // 2-bit LSBs: VBAT [1:0], TEMP [5:4]

// Charge config: 200 mA into the 2000-3000 mAh cell (0.07-0.1C, gentle) and
// well inside the user's hard rule that the WHOLE device stays under the
// 500 mA USB budget — which the VBUS input limit enforces in hardware
// regardless. Termination 4.10 V (user choice: the clock lives on the
// charger, and stopping below 4.2 V markedly extends cell life at the cost
// of some capacity).
#define NPM_CHARGE_MA 200
#define NPM_VTERM_MV 4100
// BCHGVTERM: index 4 = 4.00 V, 50 mV steps (4.10 V -> 6)
#define NPM_VTERM_IDX (4 + (NPM_VTERM_MV - 4000) / 50)

static bool s_present;
static bool s_limitOk;

static bool npm_write(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(NPM_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static int npm_read(uint16_t reg) {
  Wire.beginTransmission(NPM_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0)
    return -1;
  if (Wire.requestFrom((uint8_t)NPM_ADDR, (uint8_t)1) != 1)
    return -1;
  return Wire.read();
}

void pmic_begin() {
  Wire.beginTransmission(NPM_ADDR);
  s_present = (Wire.endTransmission() == 0);
  if (!s_present)
    return; // v1 board: LTC3226 + supercaps, nothing to configure

  // 1) Unlock the USB budget FIRST — the 100 mA power-up default browns the
  // board out on any real load. Re-applied every boot (resets with VBUS).
  s_limitOk = npm_write(NPM_VBUSINILIM0, NPM_ILIM_500MA) &&
              npm_write(NPM_TASKUPDATEILIMSW, 1) &&
              npm_read(NPM_VBUSINILIM0) == NPM_ILIM_500MA;

  // 2) Charger: current + termination BEFORE enabling.
  uint16_t idx = NPM_CHARGE_MA / 2;
  npm_write(NPM_ADCNTCRSEL, 1); // 10k on the NTC pin (fixed resistor)
  npm_write(NPM_BCHGISETMSB, (uint8_t)(idx / 2));
  npm_write(NPM_BCHGISETLSB, (uint8_t)(idx & 1));
  npm_write(NPM_BCHGVTERM, NPM_VTERM_IDX);
  npm_write(NPM_BCHGENABLESET, 1);
}

bool pmic_present() { return s_present; }
bool pmic_vbus_500() { return s_limitOk; }
uint16_t pmic_charge_current_ma() { return NPM_CHARGE_MA; }
uint16_t pmic_vterm_mv() { return NPM_VTERM_MV; }

int pmic_soc_percent(uint16_t vbatMv) {
  // Rough SoC from voltage, linear 3.5 V .. the configured termination
  // voltage. Reads high while charging; good enough for a status glance.
  int span = (int)NPM_VTERM_MV - 3500;
  int pct = ((int)vbatMv - 3500) * 100 / (span > 0 ? span : 700);
  return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

bool pmic_read_status(PmicStatus &out) {
  if (!s_present)
    return false;

  // Trigger VBAT + die-temp conversions (~250 us each), then collect.
  npm_write(NPM_TASKVBATMEASURE, 1);
  npm_write(NPM_TASKTEMPMEASURE, 1);
  delay(2);

  int vbatMsb = npm_read(NPM_ADCVBATRESULTMSB);
  int tempMsb = npm_read(NPM_ADCTEMPRESULTMSB);
  int lsbs = npm_read(NPM_ADCGP0RESULTLSBS);
  int chg = npm_read(NPM_BCHGCHARGESTATUS);
  int vbus = npm_read(NPM_VBUSINSTATUS);
  if (vbatMsb < 0 || tempMsb < 0 || lsbs < 0 || chg < 0 || vbus < 0)
    return false;

  uint16_t vbatCode = (uint16_t)((vbatMsb << 2) | (lsbs & 0x03));
  uint16_t tempCode = (uint16_t)((tempMsb << 2) | ((lsbs >> 4) & 0x03));
  out.vbatMv = (uint16_t)(((uint32_t)vbatCode * 5000u) / 1024u);
  // die temp: mdegC = 394670 - code * 792.6
  out.dieTempCx10 =
      (int16_t)((394670 - ((int32_t)tempCode * 7926) / 10) / 100);
  out.chargeStatus = (uint8_t)chg;
  out.vbusPresent = (vbus & 0x01) != 0;
  return true;
}

// BCHGCHARGESTATUS bits: b0 battery detected, b1 completed, b2 trickle,
// b3 constant current, b4 constant voltage, b5 recharge, b6 paused (die
// temp high), b7 supplement mode active.
const char *pmic_charge_text(uint8_t st) {
  if (!(st & 0x01))
    return "no battery";
  if (st & 0x40)
    return "paused (hot)";
  if (st & 0x02)
    return "charged";
  if (st & 0x04)
    return "trickle charge";
  if (st & 0x08)
    return "charging (CC)";
  if (st & 0x10)
    return "charging (CV)";
  if (st & 0x20)
    return "recharging";
  if (st & 0x80)
    return "battery assisting"; // supplement: load exceeds USB budget
  return "idle";
}
