#!/usr/bin/env python3
"""Generate a logarithmic sine sweep WAV for speaker/power bench testing.

A log sweep spends equal time per octave, which together with the on-screen
IBAT readout (UI_DEV_IBAT) maps the speaker's impedance/current curve in one
pass: current peaks at the impedance minimum, dips at the driver resonance.
Frequency at time t: f = F0 * (F1/F0)^(t/DUR)  (with defaults: 20*10^(t/10)).

Beware: a pure sine has crest factor 1 — the amp's AGC pins it at the
limiter ceiling CONTINUOUSLY, harsher than any music. This is the worst-case
load probe for the nPM1300's battery discharge limit (see ADR-0010/0014).
Start at volume 7-8. Also a magnificently horrible alarm tone.

Usage: python3 tools/gen_sweep.py [out.wav]  (drop the file on TUNES)
"""

import math
import struct
import sys
import wave

RATE = 44100
DUR = 30.0
F0, F1 = 20.0, 20000.0
AMP = 0.9 * 32767
FADE_S = 0.02  # click-free edges

out = sys.argv[1] if len(sys.argv) > 1 else "sweep_20-20k_30s.wav"
lnr = math.log(F1 / F0)
n_total = int(RATE * DUR)
frames = bytearray()
for n in range(n_total):
    t = n / RATE
    phase = 2.0 * math.pi * F0 * DUR / lnr * (math.exp(t / DUR * lnr) - 1.0)
    env = min(1.0, n / (FADE_S * RATE), (n_total - 1 - n) / (FADE_S * RATE))
    frames += struct.pack("<h", int(AMP * env * math.sin(phase)))

w = wave.open(out, "wb")
w.setnchannels(1)
w.setsampwidth(2)
w.setframerate(RATE)
w.writeframes(bytes(frames))
w.close()
print(f"{out}: {n_total} samples, {DUR:.0f} s log sweep {F0:.0f}-{F1:.0f} Hz")
