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
                   |   BATTERY BUDGET: nPM1300 IBATLIM caps discharge at
                   |   1000 mA — exceeding it drops VSYS below VSYSPOF and
                   |   RESETS the board. On battery, everything below must
                   |   fit in ~3.5 W. (On USB, VBUS adds its 0.5/1.5 A.)
                   ├──> LM3671 (U14)  -> +3.3V   (EN: R13 100k -> VSYS, always on)
                   └──> TPS61023 (U6) -> +5V     (EN: R4 100k -> VSYS, always on;
                        FB R5 732k / R12 100k -> ~5.0 V, boosts from battery too)
+3.3V: MCU, display (J4.7), L86 VCC+V_BCKP, RTC VDD, LIS3DH, QSPI flash,
       amp VDD (U1.13), I2C pullups (R6/R7 4.7k)
+5V:   all 34 LEDs (100R each, R20-R53) + TPA2016 PVCC
```

- **The nPM1300's bucks are unused** (VOUT1/VOUT2/SW unconnected): the PMIC
  does charging, power path and USB-C only; regulation is external.
- **The MCU's core (VDDCORE) runs on its internal buck** via inductor L4
  (firmware selects it — the Arduino core defaults to the lossier LDO);
  ~4–6 mA saved at 3.3 V (ADR-0014).
- **VBUS input current limit resets to 100 mA on every VBUS plug/reset** —
  firmware re-applies it at boot AND on every attach (GPIO0 IRQ), set from
  the CC advertisement: 500 mA on Default USB/PC ports, 1500 mA on 1.5/3 A
  sources. Charging + system share the budget (supplement mode: the
  battery covers bursts beyond it).
- Charger config (firmware): 400 mA setpoint (the PMIC gives charging
  whatever remains of the input budget), terminate 4.10 V (user choice,
  cell longevity), die-temp thermostat 65/55 °C. NTC pin has a fixed 10k
  to GND (R16) — reads a constant ~25 °C, real temperature limiting is
  the die thermostat. No thermistor in the battery (J7 is 2-pin).
- **RTC backup is the battery** (VBAT -> U5.6): time survives power-off as
  long as a battery is fitted. Supercaps and LTC3226 are gone.
- **Battery: generic 505060 LiPo pouch, 3.7 V 2000 mAh, WITH PCM**
  (fitted 2026-08-10, replacing the bring-up LG HG2 18650 3000 mAh
  unprotected power cell). Battery-to-PCB leads: **100 mm, 20 AWG**
  (to J7/B2B-PH-K-S) — ~7 mOhm round trip, negligible next to the
  cell+PCM source impedance. 26 AWG was tried first and REJECTED:
  too much voltage drop under load on the bench (nominal ~27 mOhm
  round trip; real crimped wire measured worse) — and with the
  loaded-voltage 3.40 V cutoff, lead drop eats usable capacity
  directly. The PCM backstops overcharge/overdischarge/
  overcurrent, but the firmware's 3.40 V ship-mode cutoff still acts
  first (PCM floors are typically 2.5-3.0 V). Expect notably higher
  source impedance than the HG2 (generic pouch ~50-150 mOhm + PCM
  ~30-50 vs the HG2's ~25): the battery-path constant used for the
  IR-compensated SoC estimate (was ~160 mOhm total with the HG2) MUST
  be re-calibrated from the charger plug-in jump — and loaded-voltage
  sag at ~1 A is correspondingly larger (re-run the worst-case audio
  bench test; low-battery watchdog may need IR compensation).
- L86 **V_BCKP is on +3.3V** (U4.5), *not* VBAT — GNSS still cold-starts
  after a full power-off (v1 HW-review #10 persists on v2).

## MCU pin map (U18 — fitted chip is a **SAMD51J20A**)

The schematic symbol says ATSAMD51J19A, but the fitted part reports
`ATSAMD51x20` via `bossac -i` (1 MB flash / 256 KB RAM) — same stale-symbol
story as the flash chip; tidy the symbol on the next revision. J19A and
J20A are pin-identical in TQFP64, so the pin map below applies unchanged.
Build with `pio run -e metro_m4_j20` (the project default).

Complete map of all 64 pads of U18, TQFP64. Columns:

- **Pad** — TQFP64 pad number (from the ATSAMD51J19A symbol in
  `GNSS_Alarm_Clock.kicad_sch`).
- **PORT** — the SAMD51 PORT pin the symbol assigns to that pad.
- **v2 net** — net name on the pad in `GNSS_Alarm_Clock.kicad_pcb`. `—` for
  power/ground; `(unconnected)` for a genuinely floating GPIO.
- **Function** — what firmware does with it.
- **Arduino** — pin number in the `adafruit_metro_m4` variant
  (`variant.cpp`, `g_APinDescription` order). `DIRECT PORT` = the PORT pin
  is not exposed by that variant, so firmware must touch the PORT
  registers. `—` = power/ground/reset, no Arduino pin.

| Pad | PORT | v2 net | Function | Arduino |
|----:|------|--------|----------|---------|
| 1 | PA00 | XIN32 (X1.2) | 32.768 kHz crystal (XOSC32K, RTC/clock) | DIRECT PORT (crystal, not GPIO) |
| 2 | PA01 | XOUT32 (X1.1) | 32.768 kHz crystal | DIRECT PORT (crystal, not GPIO) |
| 3 | PA02 | Analog | Audio DAC out → C27 → TPA2016 INR- | D14 / A0 / DAC0 |
| 4 | PA03 | +3.3V | VREFA tied to +3.3V (analog reference, not a GPIO) | — |
| 5 | PB04 | (unconnected) | free | DIRECT PORT |
| 6 | PB05 | (unconnected) | free | DIRECT PORT |
| 7 | GNDANA | GND | analog ground | — |
| 8 | VDDANA | +3.3V | analog supply | — |
| 9 | PB06 | (unconnected) | free (was RXLED on Metro) | D27 |
| 10 | PB07 | (unconnected) | free (was USB Host EN on Metro) | D29 |
| 11 | PB08 | LIS3DH_INT1 | Accelerometer INT1 (U7.11) | D18 / A4 |
| 12 | PB09 | (unconnected) | free | D19 / A5 |
| 13 | PA04 | PMIC_GPIO0 | nPM1300 GPIO0 (U3.7); unconfigured reads LOW | D17 / A3 |
| 14 | PA05 | (unconnected) | free | D15 / A1 / DAC1 |
| 15 | PA06 | LedsR-S | Right LED section gate → R56 470R → U12 | D16 / A2 |
| 16 | PA07 | (unconnected) | free | DIRECT PORT |
| 17 | PA08 | FlashIO0 | QSPI flash DATA0 (U8.5) | D43 |
| 18 | PA09 | FlashIO1 | QSPI flash DATA1 (U8.2) | D44 |
| 19 | PA10 | FlashIO2 | QSPI flash DATA2 (U8.3) | D45 |
| 20 | PA11 | FlashIO3 | QSPI flash DATA3 (U8.7) | D46 |
| 21 | VDDIOB | +3.3V | I/O supply | — |
| 22 | GND | GND | ground | — |
| 23 | PB10 | FlashSCK | QSPI flash SCK (U8.6) | D41 |
| 24 | PB11 | FlashCS | QSPI flash CS (U8.1) | D42 |
| 25 | PB12 | BUTTON3 | SW3, active-low, 1M pull-up (R9) | D7 |
| 26 | PB13 | BUTTON2 | SW2, active-low, 1M pull-up (R8) | D4 |
| 27 | PB14 | BUTTON1 | 74LVC1G17 (U9.4) output, not a raw switch | D5 |
| 28 | PB15 | BUTTON4 | SW4, active-low, 1M pull-up (R10) | D6 |
| 29 | PA12 | SPI_MOSI | Display SPI MOSI (SERCOM2), J4.5 | D26 / MOSI |
| 30 | PA13 | SPI_SCK | Display SPI SCK (SERCOM2), J4.6 | D25 / SCK |
| 31 | PA14 | SPI_MISO | SPI MISO (SERCOM2) — not routed to J4 panel | D24 / MISO |
| 32 | PA15 | (unconnected) | free | DIRECT PORT |
| 33 | GND | GND | ground | — |
| 34 | VDDIO | +3.3V | I/O supply | — |
| 35 | PA16 | DISP_RST | Display reset (active low), J4.4 | D13 |
| 36 | PA17 | DISP_DC | Display data/command, J4.3 | D12 |
| 37 | PA18 | DISP_CS | Display chip select (active low), J4.2 | D10 |
| 38 | PA19 | DISP_BL | Display backlight/aux, J4.1 | D11 |
| 39 | PB16 | (unconnected) | **buzzer removed on v2** (was SPEAKER) | D3 |
| 40 | PB17 | AmpShutdown | TPA2016 ~SD (U1.18), 10k pull-up R68, HIGH=on | D2 |
| 41 | PA20 | LedsB-S | Bottom LED section gate → R54 470R → U11 | D9 |
| 42 | PA21 | LedsL-S | Left LED section gate → R15 470R → U10 | D8 |
| 43 | PA22 | McuTX | Serial1 TX (SERCOM3) → L86 RX (U4.1) | D1 |
| 44 | PA23 | McuRX | Serial1 RX (SERCOM3) ← L86 TX (U4.2) | D0 |
| 45 | PA24 | USB_N | USB D- (via U2 ESD) | D30 |
| 46 | PA25 | USB_P | USB D+ (via U2 ESD) | D31 |
| 47 | GND | GND | ground | — |
| 48 | VDDIO | +3.3V | I/O supply | — |
| 49 | PB22 | (unconnected) | free (was NEOPIX on Metro) | D40 |
| 50 | PB23 | (unconnected) | free | DIRECT PORT |
| 51 | PA27 | (unconnected) | free (was TXLED on Metro) | D28 |
| 52 | RESETN | SWD_RST | MCU reset (SW5 + J3 SWD, 100k pull-up R67) | — |
| 53 | VDDCORE | (core reg) | 1.2 V core LDO decoupling | — |
| 54 | GND | GND | ground | — |
| 55 | VSW | (core reg) | core regulator switch node | — |
| 56 | VDDIO | +3.3V | I/O supply | — |
| 57 | PA30 | SWD_CLK | SWD clock, J3 (100k pull-up R66) | DIRECT PORT |
| 58 | PA31 | SWD_IO | SWD data, J3 | DIRECT PORT |
| 59 | PB30 | SWO | SWO trace, J3.6 | DIRECT PORT |
| 60 | PB31 | GNSS_Reset | L86 RESET_N (U4.10), active low | DIRECT PORT |
| 61 | PB00 | GNSS_FOn | L86 FORCE_ON (U4.7) | DIRECT PORT |
| 62 | PB01 | (unconnected) | **24LC512 removed on v2** (was EEPROM WP) | DIRECT PORT |
| 63 | PB02 | I2C_SDA | I2C SDA (SERCOM), 4.7k pull-up R6 | D22 / SDA |
| 64 | PB03 | I2C_SCL | I2C SCL (SERCOM), 4.7k pull-up R7 | D23 / SCL |

Delta vs v1 (`include/pins.h`), verified net-for-net:

| Pin | v1 | v2 |
|---|---|---|
| PA04 (A3) | CAPGOOD (LTC3226, HIGH = caps charged) | **PMIC_GPIO0** (nPM1300 GPIO0; unconfigured = reads LOW) |
| PB16 (D3) | Buzzer MOSFET gate (J5) | **Unconnected — the escalation buzzer does not exist on v2** |
| PB01 | 24LC512 write protect | Unconnected (EEPROM gone) |
| PB14 (D5, BUTTON1) | raw switch, active low | driven by 74LVC1G17 (U9), see Buttons |
| PB30/SWO | — | SWO to SWD connector J3.6 |

Everything else is identical net-for-net: display PA16-19 + SERCOM2 SPI (J4
8-pin XH header: BL,CS,DC,RST,MOSI,SCK,3V3,GND — no MISO to the panel),
buttons PB12-15, LED gates PA21/PA20/PA06, GNSS PA22/PA23 + PB00/PB31, DAC
PA02 → C27 → TPA2016 INR-, QSPI PA08-11 + PB10/11, I2C PB02/PB03.

**U8 tune flash — WRONG PART FITTED on the first v2 board:** JEDEC probe
reads `EF 60 18` = **W25Q128FW**, Winbond's **1.8 V** variant, running out
of spec on the 3.3 V rail (abs max ~2.5 V). It answers JEDEC but has no
library profile, so `flash.begin()` fails and the TUNES drive/storage is
disabled (firmware degrades gracefully). **Replace with W25Q128JVSIQ**
(3 V, JEDEC `EF 40 18`, in the library's builtin list, same SOIC-8) or an
MX25L3233F (4 MB, matches the schematic symbol + our explicit descriptor).
The schematic symbol says MX25L3233F and the BOM Value says "25Q128" —
tidy both on the next revision.

## Display module gotcha: BL strap polarity

Off-the-shelf NV3007 modules differ in how the backlight enable is strapped
on the module: with a **pull-up**, a floating DISP_BL (PA19 — floats through
the whole UF2-bootloader phase) means the backlight burns at FULL power
inside the PMIC's 100 mA power-up window → a battery-less board boot-loops
before any code can help (and shows a ghost of the previous frame, since
the panel GRAM survives the shallow brownouts). Diagnosed by tying BL to
GND, which let it boot. Modules with a **pull-down** are safe. Firmware
drives PA19 low as its first instruction and soft-starts the backlight,
which closes the app-phase window but cannot reach the bootloader phase.

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
100R each from +5V, R20-R53). Gates now have 470R series + 10k pulldowns
(left PA21→R15/R57→U10, bottom PA20→R54/R55→U11, right PA06→R56/R58→U12) —
v1's floating-gate-at-boot quirk is fixed.

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

## Next board revision (v3) checklist

Collected findings from the v2 bring-up, in rough priority order:

- [ ] **R68: amp ~SD pull-up → pull-DOWN.** Today the TPA2016 is enabled
      through the whole bootloader phase (~1 s) before firmware can kill
      it: wastes 5–15 mA in the battery-less boot window and audibly
      amplifies the undriven DAC node (boot noise, user-confirmed).
      Pull-down = off until firmware enables.
- [ ] **L86 load switch** (enable in front of its 3.3 V): the module is
      the largest uncontrollable boot-window load (~100 mA acquisition
      bursts) — firmware can only hold it in reset today.
- [ ] **L86 V_BCKP → VBAT** (carried over from v1 review #10): GNSS
      cold-starts after every full power-off; battery-backing it gives
      warm starts. Route 1PPS to a spare GPIO while at it.
- [ ] **VBACKUP buffer cap (U5.6)**: ~100 nF–1 µF from the RV-3028's
      VBACKUP pin to GND. In ship mode the RTC runs from VBACKUP alone
      (nA domain, high impedance) and a hot-plug ground transient — e.g.
      a floating charger's Y-cap leakage equalizing through the board
      while SWD pins board GND to PC earth — can POR it and lose the
      time (observed 2026-08-02). A local cap rides the chip through.
- [ ] **LIS3DH SA0 strap**: R18 (10k pulldown) is not effective on the
      built board (chip answers 0x19, not 0x18) — inspect/repair the
      strap or route it harder. FW probes both addresses regardless.
- [ ] **Schematic symbol tidy-up**: U18 says J19A (fitted: J20A), U8 says
      MX25L3233F with Value "25Q128" (correct part: W25Q128**JV**SIQ 3 V —
      the FW suffix cost a bricked-storage detour).
- [ ] **Display BL strap**: specify/verify pull-DOWN modules only (a
      pull-up module burns the backlight through the bootloader phase and
      boot-loops a battery-less board).
- [ ] Optional: inrush limiting on the panel rail; keep the nPM1300
      GPIO0→PA04 IRQ line (it earned its keep).

## Firmware consequences (status)

- [x] `pmic_begin()` raises VBUS ILIM at every boot + configures charger
- [x] `supercaps_ready()` returns true when PMIC present (until battery UI)
- [x] Accelerometer probes 0x18 and 0x19
- [x] Battery/PMIC screen in the menu
- [ ] Escalation buzzer is a silent no-op on v2 (PB16 unrouted) — gate the
      escalation stage on `pmic_present()` or repurpose (max volume + LEDs)
- [x] Battery gauge on the clock screen; low-battery ship-mode policy
- [x] nPM1300 GPIO0 -> PA04 used as IRQ line (VBUS attach/removal)
