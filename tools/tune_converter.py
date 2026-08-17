#!/usr/bin/env python3
"""Tune Converter — turn MP3 (or any audio) into a TUNES-ready WAV.

GUI tool for the GNSS Alarm Clock: loads any audio file ffmpeg can read,
shows the waveform, lets you drag out the section you want, and writes a
WAV in the format the firmware plays (PCM 16-bit, mono, 8-48 kHz,
filename <= 31 chars). Optional peak normalization (the amp's AGC levels
loudness anyway, but a full-scale file gives the best SNR into the DAC).

Runs on Linux and Windows. Requirements:
  - Python 3.8+ with tkinter (included in the python.org Windows installer;
    Linux: sudo apt install python3-tk)
  - ffmpeg + ffplay on PATH (Linux: sudo apt install ffmpeg;
    Windows: https://www.gyan.dev/ffmpeg/builds/ -> add bin/ to PATH)

Usage: python3 tools/tune_converter.py
Note: the firmware high-passes playback at 200 Hz (ADR-0010), so don't
worry about bass content — pick tunes with mid/high energy.
"""

import array
import os
import re
import shutil
import struct
import subprocess
import sys
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

PEAK_RATE = 4000  # decode rate for the waveform overview (Hz)
MAX_NAME = 31     # firmware filename limit (chars incl. ".wav")
RATES = ["44100", "32000", "22050", "16000", "11025", "8000"]


def die(msg):
    messagebox.showerror("Tune Converter", msg)
    sys.exit(1)


def run(cmd, **kw):
    # No console window flash on Windows
    flags = {}
    if os.name == "nt":
        flags["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    return subprocess.run(cmd, capture_output=True, **flags, **kw)


class App:
    def __init__(self, root):
        self.root = root
        root.title("GNSS Alarm Clock — Tune Converter")
        root.minsize(760, 420)

        self.src = None          # source file path
        self.duration = 0.0      # seconds
        self.peaks = []          # per-bucket abs peak, 0..1
        self.sel = [0.0, 0.0]    # selection start/end, seconds
        self.drag_anchor = None
        self.preview_proc = None

        top = ttk.Frame(root, padding=8)
        top.pack(fill="x")
        ttk.Button(top, text="Open audio…", command=self.open_file).pack(side="left")
        self.lbl_file = ttk.Label(top, text="no file loaded")
        self.lbl_file.pack(side="left", padx=10)

        self.canvas = tk.Canvas(root, height=170, bg="#101418",
                                highlightthickness=0, cursor="crosshair")
        self.canvas.pack(fill="both", expand=True, padx=8)
        self.canvas.bind("<ButtonPress-1>", self.on_press)
        self.canvas.bind("<B1-Motion>", self.on_drag)
        self.canvas.bind("<Configure>", lambda e: self.redraw())

        selrow = ttk.Frame(root, padding=(8, 4))
        selrow.pack(fill="x")
        ttk.Label(selrow, text="Start (s):").pack(side="left")
        self.var_start = tk.StringVar(value="0.0")
        self.var_end = tk.StringVar(value="0.0")
        for var in (self.var_start, self.var_end):
            var.trace_add("write", lambda *a: self.entries_changed())
        ttk.Entry(selrow, textvariable=self.var_start, width=9).pack(side="left", padx=(2, 10))
        ttk.Label(selrow, text="End (s):").pack(side="left")
        ttk.Entry(selrow, textvariable=self.var_end, width=9).pack(side="left", padx=2)
        self.lbl_len = ttk.Label(selrow, text="")
        self.lbl_len.pack(side="left", padx=12)
        ttk.Button(selrow, text="▶ Preview selection", command=self.preview).pack(side="left", padx=8)
        ttk.Button(selrow, text="■ Stop", command=self.stop_preview).pack(side="left")
        ttk.Button(selrow, text="Select all", command=self.select_all).pack(side="left", padx=8)

        opt = ttk.Frame(root, padding=(8, 4))
        opt.pack(fill="x")
        ttk.Label(opt, text="Sample rate:").pack(side="left")
        self.var_rate = tk.StringVar(value="22050")
        ttk.Combobox(opt, textvariable=self.var_rate, values=RATES,
                     width=7, state="readonly").pack(side="left", padx=(2, 14))
        self.var_norm = tk.BooleanVar(value=True)
        ttk.Checkbutton(opt, text="Peak-normalize to −1 dB",
                        variable=self.var_norm).pack(side="left", padx=(0, 14))
        ttk.Label(opt, text="(output is always 16-bit mono PCM — what the clock plays)").pack(side="left")

        out = ttk.Frame(root, padding=(8, 4))
        out.pack(fill="x")
        ttk.Label(out, text="Output name:").pack(side="left")
        self.var_name = tk.StringVar(value="")
        self.var_name.trace_add("write", lambda *a: self.check_name())
        ttk.Entry(out, textvariable=self.var_name, width=34).pack(side="left", padx=2)
        self.lbl_name = ttk.Label(out, text="")
        self.lbl_name.pack(side="left", padx=8)
        ttk.Button(out, text="Convert → WAV…", command=self.convert).pack(side="right")

        self.status = ttk.Label(root, padding=(8, 4), text=
            "Open a file, drag across the waveform to select, convert. "
            "Drop the WAV on the clock's TUNES drive.")
        self.status.pack(fill="x")

    # ---------------------------------------------------------------- load --
    def open_file(self):
        path = filedialog.askopenfilename(title="Open audio", filetypes=[
            ("Audio", "*.mp3 *.wav *.flac *.ogg *.m4a *.aac *.opus *.wma"),
            ("All files", "*.*")])
        if not path:
            return
        self.status.config(text="Decoding waveform…")
        self.root.update_idletasks()
        # Decode to mono s16 at a low rate; peaks drive the overview drawing.
        p = run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1",
                 "-ar", str(PEAK_RATE), "-f", "s16le", "-"])
        if p.returncode != 0 or not p.stdout:
            messagebox.showerror("Tune Converter",
                                 f"ffmpeg could not decode the file:\n"
                                 f"{p.stderr.decode(errors='replace')[-400:]}")
            self.status.config(text="Decode failed.")
            return
        samples = array.array("h")
        samples.frombytes(p.stdout[: len(p.stdout) // 2 * 2])
        self.src = path
        self.duration = len(samples) / PEAK_RATE
        # Bucket to ~2000 peaks; redraw() re-buckets to pixel width from these.
        n_buckets = 2000
        step = max(1, len(samples) // n_buckets)
        self.peaks = []
        for i in range(0, len(samples), step):
            chunk = samples[i:i + step]
            self.peaks.append(max(abs(s) for s in chunk) / 32768.0 if chunk else 0.0)
        self.sel = [0.0, self.duration]
        self.sync_entries()
        base = re.sub(r"[^A-Za-z0-9_\-]", "_", os.path.splitext(os.path.basename(path))[0])
        self.var_name.set((base[: MAX_NAME - 4] + ".wav"))
        self.lbl_file.config(text=f"{os.path.basename(path)}  ({self.fmt_t(self.duration)})")
        self.status.config(text="Loaded. Drag across the waveform to pick the section you want.")
        self.redraw()

    # ------------------------------------------------------------- waveform --
    def fmt_t(self, t):
        return f"{int(t // 60)}:{t % 60:04.1f}"

    def x_to_t(self, x):
        w = max(1, self.canvas.winfo_width())
        return max(0.0, min(self.duration, x / w * self.duration))

    def on_press(self, e):
        if not self.src:
            return
        self.drag_anchor = self.x_to_t(e.x)
        self.sel = [self.drag_anchor, self.drag_anchor]
        self.sync_entries()
        self.redraw()

    def on_drag(self, e):
        if not self.src or self.drag_anchor is None:
            return
        t = self.x_to_t(e.x)
        self.sel = sorted([self.drag_anchor, t])
        self.sync_entries()
        self.redraw()

    def select_all(self):
        if self.src:
            self.sel = [0.0, self.duration]
            self.sync_entries()
            self.redraw()

    def sync_entries(self):
        self._quiet = True
        self.var_start.set(f"{self.sel[0]:.2f}")
        self.var_end.set(f"{self.sel[1]:.2f}")
        self._quiet = False
        self.lbl_len.config(text=f"length {self.fmt_t(max(0, self.sel[1] - self.sel[0]))}")

    def entries_changed(self):
        if getattr(self, "_quiet", False) or not self.src:
            return
        try:
            s = float(self.var_start.get())
            e = float(self.var_end.get())
        except ValueError:
            return
        self.sel = [max(0.0, min(s, self.duration)), max(0.0, min(e, self.duration))]
        self.lbl_len.config(text=f"length {self.fmt_t(max(0, self.sel[1] - self.sel[0]))}")
        self.redraw()

    def redraw(self):
        c = self.canvas
        c.delete("all")
        w, h = c.winfo_width(), c.winfo_height()
        if not self.peaks or w < 4:
            return
        mid = h / 2
        # selection shading
        if self.sel[1] > self.sel[0]:
            x0 = self.sel[0] / self.duration * w
            x1 = self.sel[1] / self.duration * w
            c.create_rectangle(x0, 0, x1, h, fill="#1e3a5f", outline="")
        # waveform: one vertical line per pixel from the peak buckets
        n = len(self.peaks)
        for x in range(w):
            i0 = int(x / w * n)
            i1 = max(i0 + 1, int((x + 1) / w * n))
            pk = max(self.peaks[i0:i1]) if i0 < n else 0.0
            y = pk * (mid - 4)
            c.create_line(x, mid - y, x, mid + y + 1, fill="#4fc3f7")
        # selection edges + time labels
        for t in self.sel:
            x = t / self.duration * w
            c.create_line(x, 0, x, h, fill="#ffb300", width=2)
        c.create_text(4, h - 10, anchor="w", fill="#888",
                      text="0:00.0", font=("TkDefaultFont", 8))
        c.create_text(w - 4, h - 10, anchor="e", fill="#888",
                      text=self.fmt_t(self.duration), font=("TkDefaultFont", 8))

    # -------------------------------------------------------------- preview --
    def preview(self):
        if not self.src or self.sel[1] <= self.sel[0]:
            return
        self.stop_preview()
        cmd = ["ffplay", "-v", "error", "-nodisp", "-autoexit",
               "-ss", f"{self.sel[0]:.3f}", "-t", f"{self.sel[1] - self.sel[0]:.3f}",
               self.src]
        flags = {}
        if os.name == "nt":
            flags["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        self.preview_proc = subprocess.Popen(cmd, **flags)

    def stop_preview(self):
        if self.preview_proc and self.preview_proc.poll() is None:
            self.preview_proc.terminate()
        self.preview_proc = None

    # -------------------------------------------------------------- convert --
    def check_name(self):
        name = self.var_name.get()
        ok = bool(re.fullmatch(r"[A-Za-z0-9_\- ]+\.wav", name)) and len(name) <= MAX_NAME
        self.lbl_name.config(
            text="✓" if ok else f"≤{MAX_NAME} chars, A-Z 0-9 _ - only, .wav",
            foreground="#2e7d32" if ok else "#c62828")
        return ok

    def measure_peak_db(self, start, dur):
        """Max volume of the selection in dBFS via ffmpeg volumedetect."""
        p = run(["ffmpeg", "-v", "info", "-ss", f"{start:.3f}", "-t", f"{dur:.3f}",
                 "-i", self.src, "-af", "volumedetect", "-f", "null",
                 "NUL" if os.name == "nt" else "/dev/null"])
        m = re.search(rb"max_volume:\s*(-?[\d.]+)\s*dB", p.stderr)
        return float(m.group(1)) if m else None

    def convert(self):
        if not self.src:
            return
        if self.sel[1] <= self.sel[0]:
            messagebox.showwarning("Tune Converter", "Empty selection.")
            return
        if not self.check_name():
            messagebox.showwarning("Tune Converter",
                                   f"Output name must be ≤{MAX_NAME} chars "
                                   "(A-Z, 0-9, _ -, ending in .wav) — the "
                                   "firmware's filename limit.")
            return
        out = filedialog.asksaveasfilename(
            title="Save WAV (drop it on the TUNES drive afterwards)",
            initialfile=self.var_name.get(), defaultextension=".wav",
            filetypes=[("WAV", "*.wav")])
        if not out:
            return
        start, dur = self.sel[0], self.sel[1] - self.sel[0]
        af = []
        if self.var_norm.get():
            self.status.config(text="Measuring peak…")
            self.root.update_idletasks()
            peak = self.measure_peak_db(start, dur)
            if peak is not None and peak < -1.05:
                af.append(f"volume={-1.0 - peak:.2f}dB")
        cmd = ["ffmpeg", "-y", "-v", "error",
               "-ss", f"{start:.3f}", "-t", f"{dur:.3f}", "-i", self.src,
               "-ac", "1", "-ar", self.var_rate.get(), "-sample_fmt", "s16"]
        if af:
            cmd += ["-af", ",".join(af)]
        cmd += [out]
        self.status.config(text="Converting…")
        self.root.update_idletasks()
        p = run(cmd)
        if p.returncode != 0:
            messagebox.showerror("Tune Converter",
                                 f"ffmpeg failed:\n{p.stderr.decode(errors='replace')[-400:]}")
            self.status.config(text="Conversion failed.")
            return
        size = os.path.getsize(out)
        self.status.config(text=f"Done: {os.path.basename(out)} — "
                                f"{size / 1024:.0f} KB, {self.var_rate.get()} Hz "
                                f"16-bit mono. Drop it on the TUNES drive.")


def main():
    root = tk.Tk()
    try:
        ttk.Style().theme_use("clam")
    except tk.TclError:
        pass
    if not shutil.which("ffmpeg") or not shutil.which("ffplay"):
        die("ffmpeg/ffplay not found on PATH.\n\n"
            "Linux:   sudo apt install ffmpeg\n"
            "Windows: download from https://www.gyan.dev/ffmpeg/builds/ "
            "and add the bin/ folder to PATH.")
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
