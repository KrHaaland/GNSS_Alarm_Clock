# Hardware Design Review — GNSS Alarm Clock (SAMD51J19A board)

> ## Display change (2026-07): SH1122 → ST7789
> The board now uses an **ST7789 color TFT** (native 76×284, driven landscape
> **284×76**) in place of the SH1122 256×64 grayscale OLED. Firmware driver:
> `src/DisplayST7789.cpp`.
> - Same 4-wire SPI on J3: CS/DC/RST + MOSI/SCK (SERCOM2).
> - **Backlight (BLK):** the ST7789 needs a backlight supply the OLED did not.
>   Per finding #14 below, **J3 has no backlight line**, so BLK must be wired
>   separately — currently assumed on **D11 / PA19** (the old `OLED_BL`,
>   PWM-capable via TCC1 for dimming). *Confirm/adjust the actual BLK pin.*
> - Also confirm the **flash BOM substitution** (16 MB Winbond W25Q128, see the
>   TuneStorage note) when you revise the BOM.
> - Panel-specific ST7789 params still being tuned on the bench: MADCTL
>   orientation, display inversion, and the column/row window offsets.


Reviewed from `HARDWARE/production/netlist.ipc` (IPC-356 connectivity),
`HARDWARE/production/bom.csv`, and `HARDWARE/samd51_gps_alarm_clock.kicad_sch`.
Findings were produced by 8 domain reviewers and each **adversarially
verified** by an independent skeptic. 19 confirmed, 7 refuted.

> ⚠️ Reality check on the critical finding: the assembled board **does** power
> up and run (MCU, USB, display all work across many flash cycles). That is in
> tension with a fully-broken 3.3 V regulator. **Measure the 3.3 V rail and
> U14 temperature** to decide whether your unit is affected/was reworked, or
> whether the KiCad symbol pin mapping actually differs from what the netlist
> text suggests. Treat the LM3671 item as "verify on the bench first."

---

## 🔴 Critical

### 1. LM3671 (U14) 3.3 V buck — VIN and SW swapped in the KiCad symbol
- **Evidence:** schematic symbol `LM3671MFX-3.3_NOPB` declares pin **1 = "VIN"**,
  pin **5 = "SW"** (pin2=GND, pin3=EN, pin4=FB). Real LM3671MF SOT-23-5 pinout
  (TI SNVS482) is **pin1=SW, pin2=GND, pin3=EN, pin4=FB, pin5=VIN**. Netlist
  copper follows the (wrong) symbol: U14 pad1=+5V, pad4=+3.3V (FB), pad5=NET-(U14-SW),
  and L1 joins pad5 ↔ +3.3V. Footprint pads are standard SOT-23-5 (no compensating renumber).
- **Why wrong:** on real silicon +5 V lands on the switch node and VIN is fed
  only through inductor L1 from the 3.3 V output → the buck cannot regulate; the
  whole +3.3 V rail (MCU, GNSS, RTC, EEPROM, accel, QSPI flash, OLED) is affected
  and the SW pin is abused by 5 V.
- **Fix:** correct the library symbol so pin 1 = SW and pin 5 = VIN (or swap the
  copper so +5 V lands on pad5 and the L1/SW node on pad1). FB→+3.3 V is already
  correct for the fixed-output variant.

---

## 🟠 Major

### 2. Buzzer/speaker flyback diode D3 wired the wrong way (found by 3 reviewers)
- **Evidence:** D3 cathode (NET-(D3-K)) = U13 drain = J5-2; D3 anode = GND.
  J5-1 = ALARMPOWER (via 0 Ω R64). U13 is a low-side N-FET (source=GND).
- **Why wrong:** for a low-side switch on a supply-referenced inductive load
  (speaker/buzzer coil), the freewheel diode must be **anode→drain, cathode→ALARMPOWER**
  so the turn-off spike recirculates into the supply. As built it parallels the
  MOSFET body diode and clamps only negative undershoot → no protection; the FET
  can avalanche on the positive spike.
- **Fix:** reorient D3 across the coil — cathode to ALARMPOWER (J5-1 / NET-(C21-PAD1)),
  anode to the drain (NET-(D3-K)).

### 3. C30 (VDDCORE decoupling) tied to +3.3 V instead of GND
- **Evidence:** C30 pad1=NET-(U18-VDDCORE), pad2=**+3.3V**. Siblings C26/C29 are
  correctly VDDCORE↔GND. BOM groups C26+C30 as identical 1 µF → C30 is meant to be
  the 3rd VDDCORE bypass cap.
- **Why wrong:** VDDCORE is the SAMD51 internal ~1.2 V core rail; its bypass must
  reference GND. Tying it to +3.3 V injects the 3.3 V ramp/switching transient into
  the core node.
- **Fix:** move C30 pad2 from +3.3 V to GND.

### 4. 32.768 kHz crystal load caps wrong (C24/C25 = 10 pF for a 12.5 pF-CL xtal)
- **Evidence:** X1 = Epson FC-135 (CL = 12.5 pF per schematic description); C24/C25 = 10 pF.
- **Why wrong:** required load caps ≈ 2·(CL − Cstray) ≈ 18–22 pF. 10 pF presents
  ~7–8 pF effective load → oscillator runs fast (~+50…100 ppm, several s/day) and
  startup margin is reduced.
- **Fix:** fit ~18–22 pF (start 18 pF), tune on-board; or swap X1 for a 7 pF-CL crystal.
- *Note:* affects the SAMD51's own 32 kHz clock accuracy; GNSS/RTC still discipline wall-clock time.

### 5. USB ESD array (U2 PRTR5V0U2X) ground pin not grounded (found by 2 reviewers)
- **Evidence:** U2 GND pin is tied to a 1 MΩ (R1) / 1 µF (C2) node, not the GND plane.
- **Why wrong:** the ESD diode array shunts surge current into its GND pin; through
  1 MΩ (DC) / 1 µF (useless at ESD timescales) there is no low-impedance path to
  ground → ESD protection on D+/D- is defeated.
- **Fix:** route U2 pin 1 directly to GND; remove/relocate R1/C2.

### 6. Audio amp input INR+ hard-grounded instead of AC-coupled
- **Evidence:** DAC0 (PA02, net ANALOG) → C27 → INR-; INR+ tied directly to GND.
- **Why wrong:** TPA2016 differential inputs are internally biased; the standard
  single-ended hookup AC-couples both inputs (signal into one, a matching cap to
  GND on the other). Hard-grounding INR+ imbalances the bias → DC offset / turn-on pop.
- **Fix:** ground INR+ through a cap matching C27 (e.g. 100 nF).

### 7. Amp PVCC (ALARMPOWER) lacks local decoupling at U1
- **Evidence:** bulk supercaps C12/C13 are remote; C18 (220 µF)/C19 are on other rails.
- **Why wrong:** class-D PVCC needs a local low-ESR bulk + HF ceramic at the amp to
  supply pulsed switching current; remote high-ESR caps let PVCC sag/ring → distortion.
- **Fix:** add ≥10 µF (ideally the 220 µF) + 100 nF from ALARMPOWER to GND right at
  U1 pins 4/5 and 11/12; verify C18's intended rail.

### 8. RTC alarm interrupt (U5 ~INT, pin 2) not routed to the MCU
- **Evidence:** RV-3028 ~INT is a dead-end net.
- **Why wrong:** the RTC's open-drain ~INT is meant to wake the MCU on alarm/timer;
  unconnected, alarms can only be caught by continuous I²C polling → MCU can't sleep
  (defeats the low-power/backup-timekeeping design intent).
- **Fix:** route ~INT to a wake-capable GPIO with a pull-up to +3.3 V; configure it as
  an external interrupt in firmware.

### 9. VREFA (PA03) left floating
- **Evidence:** PA03 (external ADC reference) unrouted, undecoupled.
- **Why wrong:** if firmware ever selects the external reference the ADC reads garbage.
- **Fix:** either drive VREFA from a reference + add 100 nF/1 µF, or document that only
  the internal/VDDANA reference may be used. *(Current firmware uses internal ref → low practical impact.)*

---

## 🟡 Minor

### 10. GNSS V_BCKP (U4 pin 5) on main +3.3 V instead of a persistent backup rail
- **Why it matters:** V_BCKP holds the L86's ephemeris/almanac + last fix. On +3.3 V it
  dies with main power → **full cold start every boot** (TTFF ~30 s+). **This is the root
  cause of the firmware "year-2080 / no-fix" cold-start behavior.**
- **Fix:** route V_BCKP to the persistent supercap rail (as the RTC's backup already is),
  or a coin cell / diode-OR'd backup.

### 11. VDDANA not isolated from digital 3.3 V (no ferrite) + missing HF decoupling
- Add a ferrite from +3.3 V to VDDANA with 100 nF + 1 µF at the pin.

### 12. I²C / peripheral ICs have only 10 µF bulk, no 100 nF HF bypass
- Add a 100 nF close to VDD of RV-3028, 24LC512, LIS3DH (VDD & VDD_IO), TPA2016 (keep the 10 µF).

### 13. Unused amp left channel inputs (INL+/INL-) left floating
- Tie INL+/INL- to GND (or 100 nF each). Leaving OUTL open is fine.

### 14. OLED_BL toggled in firmware but not routed to J3
- The backlight/aux control terminates at the MCU only. Harmless for the SH1122 (no backlight);
  either add a J3 pin or drop OLED_BL from firmware.

### 15. LTC3226 (U15) regulated backup output (VOUT) + control pins unused
- The load runs off the raw charge-pump/supercap node → ALARMPOWER is unregulated (sags as caps
  discharge). Intentional if the direct-off-supercap topology was chosen; otherwise wire the
  PowerPath PFET on GATE, route load to VOUT, set LDO_FB/RST_FB.

### 16. LED array current budget vs LTC3226/supercap (verify, not a wiring error)
- Worst-case simultaneous LED (up to ~0.85 A if all sections on) + amp load vs the supercap
  ESR / charger current. Firmware chases one section at a time, so peak is lower; confirm no
  brownout/dimming. If marginal: raise the 80 Ω, or PWM/stagger sections.

---

## ✅ Checked and correct (refuted candidates)
USB-C CC resistors (R2/R3 = 5.1 kΩ pull-downs to GND), GNSS UART crossover
(MCUTX→L86 RXD, MCURX→L86 TXD), LM3671 feedback (FB→VOUT, correct for fixed
variant), SAMD51 VDDCORE inductor L4, LED/buzzer MOSFET gate resistors +
pull-downs, and the crystal being on the correct XIN32/XOUT32 pins.
