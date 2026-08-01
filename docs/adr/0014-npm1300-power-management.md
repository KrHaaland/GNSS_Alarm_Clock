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
1. **`pmic_begin()` runs at every boot, as early as possible**, and sets
   the VBUS input limit from the **USB-C CC advertisement**
   (USBCDETECTSTATUS): Default USB / A-to-C cable / PC port → **500 mA**
   (the hard port-protection rule); a detected 1.5/3 A source → **1500 mA**.
   Read-back verified. The PMIC enforces the limit in hardware and
   supplement mode lets the battery cover bursts beyond it. Because the
   limit resets to 100 mA on every VBUS attach, the PMIC's **GPIO0 (wired
   to PA04) is configured as an IRQ output** for VBUS events; the main loop
   services it and re-applies the CC-based limit immediately on replug.
2. **Boot-window load clamping**: before the I2C transactions (~2–3 ms),
   firmware's first instructions clamp every reachable load — L86 held in
   reset (it hangs directly on 3.3 V, no enable pin, bursts ~100 mA
   acquiring), TPA2016 forced off (R68 pulls it ON by default), display
   backlight driven low (some modules strap BL with a pull-up: a floating
   PA19 means full backlight through the whole bootloader phase). The
   backlight then **soft-starts** (PWM ramp ~100 ms) in display_init().
3. **Charger config**: 400 mA charge *setpoint* — the PMIC prioritizes
   system load in hardware and gives charging whatever remains of the
   input budget (~325–350 mA at the measured ~165 mA system draw), so the
   setpoint is a max, not a demand. Terminate at **4.10 V** (user choice:
   the clock lives on the charger; undercharging markedly extends cell
   life), NTC type 10k. A **die-temperature thermostat** pauses charging
   at 65 °C and resumes at 55 °C (chip default 110/100; tuned up from a
   first 55/45 try on the bench) — tight enough that the enclosure stays
   cool and the charge rate self-regulates. Charger enabled
   only after current + termination + thresholds are set.
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

7. **Low-battery policy (ship mode)**: on battery only — never on a
   charger, and never while an alarm is Ringing/Snoozed (the LED+speaker
   load sags VBAT 50–100 mV and would false-trigger; waking someone beats
   the last percent of battery) — three consecutive 10 s readings under
   3.40 V enter ship mode (<500 nA, battery cut from VSYS, RTC keeps time
   on VBAT). Woken below 3.45 V without a charger: fullscreen
   "LOW BATTERY" for 4 s, then back to ship mode. A menu "Shutdown" item
   (OK-confirmed, refused on USB power) enters the same state manually.
   Wake is BUTTON1 (=SHPHLD) or USB attach.
8. **Battery telemetry**: IBAT measurement rides on every VBAT ADC round;
   the Battery screen shows signed current, the active input limit, charge
   state and die temperature — the bench multimeter lives on-screen.

## Consequences
- Any future code that adds a load at boot must either run after
  `pmic_begin()` or be added to the clamp block.
- On 1.5/3 A sources the limit is 1500 mA, so charging + alarm peaks are
  fully USB-fed; the 400 mA charge setpoint (not the input limit) is then
  the charging bottleneck, by choice.
- The fixed-10k NTC means no real temperature protection — acceptable for
  a stationary indoor clock at a 400 mA setpoint with the 55 C thermostat.
- VBUS replug always reverts to 100 mA until `pmic_begin()` runs — device
  behavior right after plug-in (before boot completes) stays budget-bound.
