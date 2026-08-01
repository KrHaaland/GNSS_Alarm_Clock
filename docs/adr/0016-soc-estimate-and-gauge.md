# 0016 — Battery SoC: measured-only estimate behind an approximate gauge

Date: 2026-08-02 · Status: **Accepted** · Related: [0014](0014-npm1300-power-management.md)

## Context
The clock face needs a battery indicator. The nPM1300 has no hardware
fuel gauge — it provides raw VBAT/IBAT/die-temp. Nordic's nRF Fuel Gauge
library was evaluated and **rejected on principle: it is a closed-source
precompiled binary** (also: it wants a characterized battery model and a
battery-temperature input the board doesn't have). The firmware stays
fully open source.

A naive linear voltage→SoC map mis-read badly around charging: plugging a
charger lifted the terminal voltage (I·R + polarization), showing an
instant +8 % at 350 mA and +20 % at 800 mA.

## Decision
1. **SoC = IR-compensated voltage, nothing else.**
   `V_ocv ≈ V_meas − I_meas × 160 mΩ`, linear 3.5 V → V_term. The 160 mΩ
   is the whole battery path as seen from the PMIC's VBAT pin (LG HG2
   cell ~25 mΩ; the rest is holder contacts + wiring) and was calibrated
   from the observed plug-in jump itself. Every displayed value derives
   from a present-moment measurement — no history, no stored state.
2. **A coulomb-counting tracker was built, shipped, and deliberately
   reverted the same day.** Integrating measured IBAT against the cell
   capacity fixed the plug-in jump but replaced one measured artifact
   with layered guesses: the anchor error propagates, real capacity at a
   4.10 V termination is unknown (and ages), the estimate seams visibly
   when switching models at unplug, and a reboot mid-charge re-anchors
   wrong. Verdict (user): honest-but-disturbed beats smooth-but-guessed.
3. **The clock face shows no percent number.** False precision confuses;
   instead a **drawn battery gauge** (white 2 px frame + nub — matching
   the true-black/white aesthetic) with a fill bar whose width tracks the
   estimate and whose color grades **red < 20 % / orange < 50 % / green**.
   A charge bolt appears while charging — which is also the honest signal
   that the reading is elevated (~15–20 % at 800 mA; known, accepted).
   The Battery menu screen keeps full numeric detail (V, signed I, state,
   die temp, active input limit) for diagnostics.
4. Status symbols live in a flex row lifted above the clock label in
   z-order: the true-black theme (`blacken()`) gives every object an
   opaque black background, and the big digits' box otherwise clips
   overlapping symbols.

## Consequences
- SoC accuracy is honest-approximate (±10–15 %, worse while charging) —
  the gauge presentation makes that acceptable; nothing pretends to know
  better than the measurement.
- The 160 mΩ constant is board-specific (mostly holder contacts): a new
  holder or battery type means re-checking the plug-in jump.
- If real accuracy is ever wanted: characterize the cell and implement a
  proper (open-source) model — not more layered heuristics.
