# Hardware v2 — firmware-facing reference

Extracted systematically from `HARDWARE/GNSS_Alarm_Clock.kicad_pcb` (the
netlist is authoritative for the built board). v1 design lives in
`HARDWARE_OLD/`. This file is the ground truth for firmware work on v2 —
update it when the board changes.

## Power tree

```
USB-C (J1, CC1/CC2 -> PMIC)          Li-ion (J7, 2-pin JST-PH)
        |                                   |
      VUSB ──> nPM1300 (U3, I2C 0x6B) <── VBAT ──> RV-3028 VBACKUP (U5.6)
                   |  charger + power path + USB-C detection
                 VSYS  (= VBUS when present, else battery, ideal diode)
                   ├──> LM3671 (U14)  -> +3.3V   (EN: R13 100k -> VSYS, always on)
                   └──> TPS61023 (U6) -> +5V     (EN: R4 100k -> VSYS, always on;
                        FB R5 732k / R12 100k -> ~5.0 V, boosts from battery too)
+3.3V: MCU, display (J4.7), L86 VCC+V_BCKP, RTC VDD, LIS3DH, QSPI flash,
       amp VDD (U1.13), I2C pullups (R6/R7 4.7k)
+5V:   all 34 LEDs (80R each) + TPA2016 PVCC
```

- **The nPM1300's bucks are unused** (VOUT1/VOUT2/SW unconnected): the PMIC
  does charging, power path and USB-C only; regulation is external.
- **VBUS input current limit resets to 100 mA on every VBUS plug/reset** —
  `pmic_begin()` must raise it to 500 mA early in every boot, or any LED
  load browns out the board. The 500 mA limit is the hard USB-port
  protection; charging + system share it (supplement mode: the battery
  covers bursts beyond it).
- Charger config (firmware): 200 mA, terminate 4.10 V (user choice, cell
  longevity), NTC pin has a fixed 10k to GND (R16) — reads a constant
  ~25 °C, temperature limits effectively bypassed. No thermistor in the
  battery (J7 is 2-pin).
- **RTC backup is the battery** (VBAT -> U5.6): time survives power-off as
  long as a battery is fitted. Supercaps and LTC3226 are gone.
- L86 **V_BCKP is on +3.3V** (U4.5), *not* VBAT — GNSS still cold-starts
  after a full power-off (v1 HW-review #10 persists on v2).

## MCU pin map (U18, SAMD51J19A — v2 is still the J19!)

Identical to v1 (`include/pins.h`) except:

| Pin | v1 | v2 |
|---|---|---|
| PA04 (A3) | CAPGOOD (LTC3226, HIGH = caps charged) | **PMIC_GPIO0** (nPM1300 GPIO0; unconfigured = reads LOW) |
| PB16 (D3) | Buzzer MOSFET gate (J5) | **Unconnected — the escalation buzzer does not exist on v2** |
| PB01 | 24LC512 write protect | Unconnected (EEPROM gone) |
| PB30/SWO | — | SWO to SWD connector J3.6 |

Everything else verified identical net-for-net: display PA16-19 + SERCOM2
SPI (J4 8-pin XH header: BL,CS,DC,RST,MOSI,SCK,3V3,GND — no MISO to the
panel), buttons PB12-15, LED gates PA21/PA20/PA06, GNSS PA22/PA23 +
PB00/PB31, DAC PA02 -> C27 -> TPA2016 INR-, QSPI PA08-11+PB10/11 (U8 =
W25Q128, 16 MB), I2C PB02/PB03.

## Buttons

- SW2-SW4 = BUTTON2-4, active low, 1M pullups (as v1).
- **BUTTON1 is special on v2**: the physical button is SW1, which drives
  the nPM1300's **SHPHLD** pin directly and reaches PB14 through a
  74LVC1G17 buffer (U9). Idle high (PMIC internal pull), pressed = LOW —
  same polarity as v1. Consequences:
  - Pressing BUTTON1 wakes the PMIC from ship/hibernate mode (battery boot).
  - A very long press (~10 s, PMIC long-press function) power-cycles the
    whole board in hardware — independent of firmware.
- SW5 = MCU reset (SWD_RST), on the 10-pin SWD header J3 side.

## LED sections (unchanged counts, better gates)

10 left / 14 bottom / 10 right (LED1-10 / LED11-24 / LED25-34, OR-PL020W,
80R each from +5V). Gates now have 470R series + 10k pulldowns
(R15/R57, R54/R55, R56/R58) — v1's floating-gate-at-boot quirk is fixed.

## I2C bus (100 kHz, pullups 4.7k)

| Addr | Device | Note |
|---|---|---|
| 0x18/0x19 | LIS3DH (U7) | v2 straps SA0 low (R18 10k->GND) and CS high (R63 10k->3.3V), so 0x18 is the *design* address — but the first v2 board answers at **0x19**, meaning SA0 doesn't see its pulldown (suspect solder at U7 or R18 unfitted). Harmless: FW probes both. |
| 0x52 | RV-3028 RTC | battery-backed via VBAT |
| 0x58 | TPA2016 amp | PVCC now +5V boost (works on battery) |
| 0x6B | nPM1300 PMIC | see power tree |

## GNSS (L86, U4)

Wiring identical to v1: pad1->McuTX(PA22), pad2->McuRX(PA23), VCC+V_BCKP
on +3.3V, FORCE_ON PB00, RESET_N PB31. **1PPS (U4.6) and AADET_N (U4.8)
remain unrouted** (v1 findings persist). V_BCKP not battery-backed (above).

## Other

- USB ESD: U2 PRTR5V0U2X on USB_P/N/VUSB (+C2/R1 network on pin 1).
- 32.768 kHz crystal X1 on PA00/PA01 with 10 pF load caps (v1 finding
  about wanting ~18-22 pF persists).
- Speaker: J6 from TPA2016 OUTR± (right channel only, as v1).
- nPM1300 GPIO1-4 and LED0-2 pads unconnected; GPIO0 -> PA04 (candidate:
  configure as charge-status output so FW can read charging without I2C).

## Firmware consequences (status)

- [x] `pmic_begin()` raises VBUS ILIM at every boot + configures charger
- [x] `supercaps_ready()` returns true when PMIC present (until battery UI)
- [x] Accelerometer probes 0x18 and 0x19
- [x] Battery/PMIC screen in the menu
- [ ] Escalation buzzer is a silent no-op on v2 (PB16 unrouted) — gate the
      escalation stage on `pmic_present()` or repurpose (max volume + LEDs)
- [ ] Battery icon/% on the clock screen; low-battery behavior
- [ ] Optional: nPM1300 GPIO0 as charge indicator on PA04
