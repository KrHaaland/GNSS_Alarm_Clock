// PmicNPM1300.cpp — nPM1300 support: VBUS current limit, Li-ion charger
// config, and status readout for the Battery screen. Register offsets and
// encodings cross-checked against Zephyr's npm13xx drivers.
#include "PmicNPM1300.h"
#include <Wire.h>

#define NPM_ADDR 0x6B

// VBUSIN group (base 0x02)
#define NPM_TASKUPDATEILIMSW 0x0200 // write 1: apply VBUSINILIM0
#define NPM_VBUSINILIM0 0x0201      // 1 = 100 mA (power-up default), 5 = 500 mA
#define NPM_USBCDETECTSTATUS 0x0205 // CC1 [1:0] / CC2 [3:2]: 0 none,
                                    // 1 Default USB, 2 = 1.5 A, 3 = 3 A
#define NPM_VBUSINSTATUS 0x0207     // b0 VBUSINPRESENT
#define NPM_ILIM_500MA 5
#define NPM_ILIM_1500MA 15

// BCHARGER group (base 0x03)
#define NPM_BCHGENABLESET 0x0304   // b0 = enable charging
#define NPM_BCHGENABLECLR 0x0305   // b0 = disable charging
#define NPM_BCHGISETMSB 0x0308     // charge current index = mA/2; MSB = idx/2
#define NPM_BCHGISETLSB 0x0309     // LSB = idx & 1
#define NPM_BCHGVTERM 0x030C       // 4 = 4.00 V .. 13 = 4.45 V (50 mV steps)
#define NPM_DIETEMPSTOP 0x0318     // die-temp charge throttle, 10-bit pair
#define NPM_DIETEMPSTOPLSB 0x0319
#define NPM_DIETEMPRESUME 0x031A
#define NPM_DIETEMPRESUMELSB 0x031B
#define NPM_BCHGCHARGESTATUS 0x0334
// The board's NTC pin has a fixed 10 kOhm to GND (no thermistor in the
// battery): the charger reads a constant ~25 C, i.e. temperature limits are
// effectively bypassed. 10 kOhm is the type we must tell the ADC about.
#define NPM_ADCNTCRSEL 0x050A // 1 = 10k NTC

// MAIN event system (base 0x00) — routed out on GPIO0 as an IRQ line
#define NPM_EVENTSVBUSIN0SET 0x0016 // b0 = VBUS detected, b1 = VBUS removed
#define NPM_EVENTSVBUSIN0CLR 0x0017
#define NPM_INTENEVENTSVBUSIN0SET 0x0018

// GPIO group (base 0x06) — GPIO0 is wired to the MCU's PA04
#define NPM_GPIOMODE0 0x0600
#define NPM_GPIOMODE_GPOIRQ 5 // GPIO drives high while unmasked events pend

// SHIP group (base 0x0B)
#define NPM_TASKENTERSHIPMODE 0x0B02 // battery cut from VSYS; SHPHLD/USB wakes

// ADC group (base 0x05)
#define NPM_TASKVBATMEASURE 0x0500
#define NPM_TASKTEMPMEASURE 0x0502 // die temperature
#define NPM_ADCIBATSTATUS 0x0510   // 0x04 discharge, 0x0C/0x0D/0x0F charging
#define NPM_ADCVBATRESULTMSB 0x0511
#define NPM_ADCTEMPRESULTMSB 0x0513
#define NPM_ADCGP0RESULTLSBS 0x0515 // 2-bit LSBs: VBAT [1:0], TEMP [5:4]
#define NPM_ADCIBATRESULTMSB 0x0518
#define NPM_ADCGP1RESULTLSBS 0x0519 // 2-bit LSBs: IBAT [5:4]
#define NPM_ADCIBATMEASEN 0x0524    // 1 = measure IBAT with every VBAT task

// IBAT full-scale references (per the nPM1300 spec / Zephyr driver):
// charging = charge setpoint * 1.25; discharging = discharge limit * 1.12.
// We never touch the discharge limiter, so the chip default (1000 mA) applies.
#define NPM_DISCHG_LIMIT_MA 1000

// Charge config: the setpoint follows the detected input budget —
// 400 mA on a 500 mA source, 800 mA (charger max) on a 1.5/3 A source.
// Both are gentle for the LG HG2 (rated 1.5 A standard charge): 0.13C and
// 0.27C. The setpoint is a MAXIMUM: the nPM1300 prioritizes system load
// in hardware and gives charging whatever remains of the budget, and the
// 65/55 C die thermostat caps the thermal side (~1 W at 800 mA — watch
// the Battery screen). Termination 4.10 V (user choice: the clock lives
// on the charger; undercharging markedly extends cell life).
#define NPM_CHARGE_MA_500BUDGET 400
#define NPM_CHARGE_MA_HIPOWER 800

// Die-temp charge throttle (linear charger burns (VSYS-VBAT)*I in the die):
// pause charging at 80 C, resume at 70 C (chip defaults: 110/100; bench-
// tuned upward 55/45 -> 80/70 as real charge rates grew to 800 mA — the
// case stays barely warm, confirming a healthy die-to-case gradient).
// Thermostat behavior is intended: past the limit the
// enclosure cool ("paused (hot)" + die temp are live on the Battery
// screen). Same encoding as the die-temp ADC readout:
// K = (394670 - T_mdegC) / 792.6.
#define NPM_DIETEMP_STOP_C 80
#define NPM_DIETEMP_RESUME_C 70
#define NPM_DIETEMP_CODE(tC) ((uint16_t)((394670L - (tC)*1000L) * 10 / 7926))
#define NPM_VTERM_MV 4100
// BCHGVTERM: index 4 = 4.00 V, 50 mV steps (4.10 V -> 6)
#define NPM_VTERM_IDX (4 + (NPM_VTERM_MV - 4000) / 50)

static bool s_present;
static bool s_limitOk;
static uint16_t s_limitMa = 500;
static uint32_t s_reconfAtMs;   // pending delayed reconfigure (0 = none)
static bool s_reconfSecond;     // a second, late pass is still due
static uint16_t s_chargeMa = NPM_CHARGE_MA_500BUDGET;

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

void pmic_vbus_reconfigure() {
  if (!s_present)
    return;
  // Pick the input limit from the USB-C CC advertisement (the PMIC's
  // comparators read the source's Rp resistor — no PD protocol involved):
  // Default USB / A-to-C cable / PC port -> 500 mA (the hard PC-port rule);
  // a 1.5 A or 3 A source -> 1500 mA (chip max), so charging + alarm peaks
  // never touch the battery on a real charger.
  int cc = npm_read(NPM_USBCDETECTSTATUS);
  uint8_t cc1 = cc < 0 ? 0 : (cc & 0x03);
  uint8_t cc2 = cc < 0 ? 0 : ((cc >> 2) & 0x03);
  uint8_t best = cc1 > cc2 ? cc1 : cc2;
  uint8_t ilim = (best >= 2) ? NPM_ILIM_1500MA : NPM_ILIM_500MA;
  s_limitMa = (best >= 2) ? 1500 : 500;
  s_limitOk = npm_write(NPM_VBUSINILIM0, ilim) &&
              npm_write(NPM_TASKUPDATEILIMSW, 1) &&
              npm_read(NPM_VBUSINILIM0) == ilim;

  // Charge setpoint follows the budget. Swap it with the charger briefly
  // disabled (clean ISET handover); IBAT full-scale tracks s_chargeMa.
  uint16_t ma = (best >= 2) ? NPM_CHARGE_MA_HIPOWER : NPM_CHARGE_MA_500BUDGET;
  if (ma != s_chargeMa) {
    s_chargeMa = ma;
    uint16_t idx = s_chargeMa / 2;
    npm_write(NPM_BCHGENABLECLR, 1);
    npm_write(NPM_BCHGISETMSB, (uint8_t)(idx / 2));
    npm_write(NPM_BCHGISETLSB, (uint8_t)(idx & 1));
    npm_write(NPM_BCHGENABLESET, 1);
  }
}

void pmic_begin() {
  Wire.beginTransmission(NPM_ADDR);
  s_present = (Wire.endTransmission() == 0);
  if (!s_present)
    return; // v1 board: LTC3226 + supercaps, nothing to configure

  // 1) Unlock the USB budget FIRST — the 100 mA power-up default browns the
  // board out on any real load. Re-applied every boot AND on every VBUS
  // re-attach (the limit resets to 100 mA with VBUS).
  pmic_vbus_reconfigure();

  // 2) Charger: current + termination BEFORE enabling.
  uint16_t idx = s_chargeMa / 2;
  npm_write(NPM_ADCNTCRSEL, 1); // 10k on the NTC pin (fixed resistor)
  npm_write(NPM_BCHGISETMSB, (uint8_t)(idx / 2));
  npm_write(NPM_BCHGISETLSB, (uint8_t)(idx & 1));
  npm_write(NPM_BCHGVTERM, NPM_VTERM_IDX);
  uint16_t kStop = NPM_DIETEMP_CODE(NPM_DIETEMP_STOP_C);
  uint16_t kResume = NPM_DIETEMP_CODE(NPM_DIETEMP_RESUME_C);
  npm_write(NPM_DIETEMPSTOP, (uint8_t)(kStop >> 2));
  npm_write(NPM_DIETEMPSTOPLSB, (uint8_t)(kStop & 3));
  npm_write(NPM_DIETEMPRESUME, (uint8_t)(kResume >> 2));
  npm_write(NPM_DIETEMPRESUMELSB, (uint8_t)(kResume & 3));
  npm_write(NPM_BCHGENABLESET, 1);
  npm_write(NPM_ADCIBATMEASEN, 1); // piggyback IBAT on every VBAT measurement

  // Interrupt line to the MCU: GPIO0 (-> PA04) goes high on VBUS attach or
  // removal, so the input-limit reconfiguration is immediate instead of
  // polled. main.cpp watches the pin and calls pmic_handle_irq().
  npm_write(NPM_EVENTSVBUSIN0CLR, 0x03); // drop stale events
  npm_write(NPM_INTENEVENTSVBUSIN0SET, 0x03);
  npm_write(NPM_GPIOMODE0, NPM_GPIOMODE_GPOIRQ);
}

void pmic_handle_irq() {
  if (!s_present)
    return;
  int ev = npm_read(NPM_EVENTSVBUSIN0SET);
  npm_write(NPM_EVENTSVBUSIN0CLR, 0xFF); // ack; the GPIO0 line drops
  if (ev > 0 && (ev & 0x01)) {
    // VBUS attached. Do NOT reconfigure here: the PMIC's CC classification
    // and register defaults are still settling right after attach —
    // reconfiguring immediately raced them (CC read as "default" and the
    // ILIM read-back mismatched: the Battery screen's "RAISE FAILED").
    // Schedule a pass at +300 ms and a second at +1.5 s (late CC settle);
    // pmic_task() runs them without blocking the main loop.
    s_reconfAtMs = millis() + 300;
    s_reconfSecond = true;
  }
}

void pmic_task() {
  if (!s_present || s_reconfAtMs == 0)
    return;
  if ((int32_t)(millis() - s_reconfAtMs) < 0)
    return;
  pmic_vbus_reconfigure();
  if (s_reconfSecond || !s_limitOk) {
    s_reconfSecond = false;
    s_reconfAtMs = millis() + 1500; // one more pass (or retry on failure)
  } else {
    s_reconfAtMs = 0;
  }
}

void pmic_enter_ship_mode() {
  if (!s_present)
    return;
  npm_write(NPM_TASKENTERSHIPMODE, 1);
  // With VBUS absent the power vanishes "immediately" (datasheet). With VBUS
  // PRESENT the nPM1300 refuses ship entry — spinning forever here hung the
  // device with a frozen screen (seen live: charger plugged during the LOW
  // BATTERY boot gate). Still executing after the grace period means a
  // charger is holding VSYS up: reboot into a normal charging boot instead.
  uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < 300) {
  }
  NVIC_SystemReset();
}

bool pmic_present() { return s_present; }
bool pmic_vbus_limit_ok() { return s_limitOk; }
uint16_t pmic_vbus_limit_ma() { return s_limitMa; }
uint16_t pmic_charge_current_ma() { return s_chargeMa; }
uint16_t pmic_vterm_mv() { return NPM_VTERM_MV; }

int pmic_soc_percent(uint16_t vbatMv, int16_t ibatMa) {
  // Rough SoC from voltage, linear 3.5 V .. the configured termination
  // voltage — but IR-COMPENSATED back to open-circuit voltage using the
  // measured battery current: charging lifts the terminal voltage by
  // I*R_int (the bare estimate jumped 0->8% the instant the charger was
  // plugged; that 56 mV lift at ~350 mA calibrated R_int to ~160 mOhm),
  // and discharge sags it the same way. Signed current handles both.
  int vOcv = (int)vbatMv - ((int)ibatMa * 160) / 1000;
  int span = (int)NPM_VTERM_MV - 3500;
  int pct = (vOcv - 3500) * 100 / (span > 0 ? span : 700);
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
  int ibatStat = npm_read(NPM_ADCIBATSTATUS);
  int ibatMsb = npm_read(NPM_ADCIBATRESULTMSB);
  int lsbsB = npm_read(NPM_ADCGP1RESULTLSBS);
  int chg = npm_read(NPM_BCHGCHARGESTATUS);
  int vbus = npm_read(NPM_VBUSINSTATUS);
  if (vbatMsb < 0 || tempMsb < 0 || lsbs < 0 || ibatStat < 0 || ibatMsb < 0 ||
      lsbsB < 0 || chg < 0 || vbus < 0)
    return false;

  uint16_t vbatCode = (uint16_t)((vbatMsb << 2) | (lsbs & 0x03));
  uint16_t tempCode = (uint16_t)((tempMsb << 2) | ((lsbs >> 4) & 0x03));
  out.vbatMv = (uint16_t)(((uint32_t)vbatCode * 5000u) / 1024u);
  // die temp: mdegC = 394670 - code * 792.6
  out.dieTempCx10 =
      (int16_t)((394670 - ((int32_t)tempCode * 7926) / 10) / 100);

  // Battery current: 10-bit code against a status-dependent full scale.
  // Positive = into the battery (charging), negative = out (discharging).
  int32_t ibatCode = (ibatMsb << 2) | ((lsbsB >> 4) & 0x03);
  switch (ibatStat & 0x0F) {
  case 0x0C: // trickle
  case 0x0D: // cool (temperature-limited)
  case 0x0F: // constant current/voltage
    out.ibatMa = (int16_t)((ibatCode * ((long)s_chargeMa * 125 / 100)) / 1023);
    break;
  case 0x04: // discharging
    out.ibatMa =
        (int16_t)(-(ibatCode * (NPM_DISCHG_LIMIT_MA * 112L / 100)) / 1023);
    break;
  default:
    out.ibatMa = 0;
    break;
  }

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
