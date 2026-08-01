# 0014 — v2 power management: nPM1300 strategy

Date: 2026-08-01 · Status: **Accepted**

## Context
The v2 board replaces v1's LTC3226 + supercaps with a **nPM1300 PMIC**
(I2C 0x6B): Li-ion charging, USB-C input with CC detection, VBUS→VSYS power
path with battery supplement. Regulation stays external (LM3671 3.3 V,
TPS61023 5 V boost — both VSYS-fed). Hard requirement from the user: the
device must never draw more than **500 mA from a PC USB port**. Battery:
2000–3000 mAh Li-ion, 2-pin (no thermistor; the PMIC's NTC pin has a fixed
10k to GND = constant ~25 °C reading).

The PMIC powers up with a **100 mA VBUS limit** (OTP default, resets on
every VBUS plug) and its charger **disabled** until the host configures it.

## Decision
1. **`pmic_begin()` runs at every boot, as early as possible**: raises the
   VBUS input limit to 500 mA (`VBUSINILIM0=5` + `TASKUPDATEILIMSW`), with
   read-back verification. 500 mA is the hard port-protection budget; the
   PMIC enforces it in hardware, and supplement mode lets the battery cover
   bursts beyond it.
2. **Boot-window load clamping**: before the I2C transactions (~2–3 ms),
   firmware's first instructions clamp every reachable load — L86 held in
   reset (it hangs directly on 3.3 V, no enable pin, bursts ~100 mA
   acquiring), TPA2016 forced off (R68 pulls it ON by default), display
   backlight driven low (some modules strap BL with a pull-up: a floating
   PA19 means full backlight through the whole bootloader phase). The
   backlight then **soft-starts** (PWM ramp ~100 ms) in display_init().
3. **Charger config**: 200 mA charge current (0.07–0.1C — gentle, and
   charging + system stay inside the 500 mA budget), terminate at
   **4.10 V** (user choice: the clock lives on the charger; undercharging
   markedly extends cell life), NTC type 10k. Charger enabled only after
   current + termination are set.
4. **SoC estimate**: linear voltage map 3.5 V → V_term via
   `pmic_soc_percent()` — one shared implementation for the clock-face
   battery row and the Battery screen. Reads high while charging; good
   enough for a status glance, no fuel-gauge pretensions.
5. **MCU core runs on the internal buck regulator** (`SUPC->VREG.SEL=1`,
   first instruction in `setup()`): the Arduino core defaults to the LDO,
   but the board carries the VSW inductor (L4). Saves ~4–6 mA at 3.3 V
   continuously and trims the MCU's draw inside the 100 mA power-up
   window; becomes proportionally more valuable when a sleep/night mode
   is introduced.
6. **Battery-less boot is out of scope on USB-A**: the UF2 bootloader phase
   (~0.5–1 s) runs before any application code, inside the 100 mA window,
   with the un-clampable base load (MCU + L86 + pulled-on amp + panel) at
   ~100–120 mA. Remedies, in order of practicality: keep a battery fitted
   (the design assumption); use a USB-C→C cable from a 1.5/3 A source (the
   PMIC reads the CC advertisement autonomously, before any code); bake the
   ILIM write into the planned custom uf2-samdx1 bootloader; add inrush
   limiting/load switches on the next board spin.

## Consequences
- Any future code that adds a load at boot must either run after
  `pmic_begin()` or be added to the clamp block.
- The 500 mA write also caps charging from strong wall chargers; if faster
  charging is ever wanted, read the PMIC's CC-detection status and raise
  the limit only on 1.5/3 A sources.
- The fixed-10k NTC means no real temperature protection — acceptable for
  a stationary indoor clock at 200 mA charge rate.
- VBUS replug always reverts to 100 mA until `pmic_begin()` runs — device
  behavior right after plug-in (before boot completes) stays budget-bound.
