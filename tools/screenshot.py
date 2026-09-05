#!/usr/bin/env python3
"""Grab screenshots from the clock over USB serial.

    python3 tools/screenshot.py [-p /dev/ttyACM0] [-o base]

Runs an interactive session: navigate the clock to a screen, press Enter
to capture, repeat — each frame is saved as base_01.png, base_02.png, ...
(numbering continues after existing files). 'q' + Enter quits.
Single-shot mode: -1/--single writes exactly one capture and exits.

Each capture sends '~' to the firmware, which streams the next rendered
frame as raw RGB565-LE strips (SHOT-BEGIN/AREA/END protocol in
DisplayNV3007.cpp); the PNG is written with pure stdlib (no PIL). Uses
pyserial when installed; falls back to raw POSIX tty on Linux.
"""

import argparse
import os
import struct
import sys
import time
import zlib


# ------------------------------------------------------------- serial I/O --
class Port:
    def __init__(self, path):
        try:
            import serial  # pyserial

            self.s = serial.Serial(path, 115200, timeout=5)
            self.posix = None
        except ImportError:
            if os.name == "nt":
                sys.exit("pyserial required on Windows: pip install pyserial")
            import termios

            self.s = None
            self.posix = os.open(path, os.O_RDWR | os.O_NOCTTY)
            attrs = termios.tcgetattr(self.posix)
            attrs[0] = attrs[1] = attrs[3] = 0  # raw: no iflag/oflag/lflag
            attrs[2] |= termios.CREAD | termios.CLOCAL  # cflag
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 50  # 5 s read timeout
            termios.tcsetattr(self.posix, termios.TCSANOW, attrs)

    def write(self, b):
        if self.s:
            self.s.write(b)
        else:
            os.write(self.posix, b)

    def read(self, n):
        if self.s:
            return self.s.read(n)
        chunks, deadline = [], time.time() + 6
        got = 0
        while got < n and time.time() < deadline:
            c = os.read(self.posix, n - got)
            if c:
                chunks.append(c)
                got += len(c)
        return b"".join(chunks)

    def read_line(self):
        buf = bytearray()
        while True:
            c = self.read(1)
            if not c:
                return None  # timeout
            if c == b"\n":
                return bytes(buf).strip()
            buf += c


# --------------------------------------------------------------- PNG write --
def write_png(path, w, h, rgb):  # rgb: bytes, 3 per pixel, row-major
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) +
           chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def rgb565_to_rgb888(data):
    out = bytearray(len(data) // 2 * 3)
    for i in range(0, len(data) - 1, 2):
        v = data[i] | (data[i + 1] << 8)  # little-endian
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        j = i // 2 * 3
        out[j] = (r << 3) | (r >> 2)
        out[j + 1] = (g << 2) | (g >> 4)
        out[j + 2] = (b << 3) | (b >> 2)
    return bytes(out)


# --------------------------------------------------------------------- main --
def find_port():
    """The clock's CDC port: match its USB identity (pid.codes 1209:0001),
    else first ACM/USB serial port. ACM numbering shifts with replug order,
    so a hardcoded ttyACM0 default was a trap."""
    try:
        from serial.tools import list_ports

        ports = list(list_ports.comports())
        for p in ports:
            if p.vid == 0x1209 and p.pid == 0x0001:
                return p.device
        for p in ports:
            if "ACM" in p.device or "USB" in p.device:
                return p.device
    except ImportError:
        import glob

        g = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
        if g:
            return g[0]
    return None


def grab(port):
    """Trigger and collect one frame; returns (w, h, fb) or None on timeout."""
    port.write(b"~")
    fb = None
    w = h = 0
    deadline = time.time() + 15
    while time.time() < deadline:
        line = port.read_line()
        if line is None:
            return None
        if line.startswith(b"SHOT-BEGIN"):
            _, ws, hs = line.split()
            w, h = int(ws), int(hs)
            fb = bytearray(w * h * 2)
        elif line.startswith(b"SHOT-AREA") and fb is not None:
            _, x1, y1, x2, y2 = line.split()
            x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)
            sw = x2 - x1 + 1
            data = port.read((x2 - x1 + 1) * (y2 - y1 + 1) * 2)
            for row, y in enumerate(range(y1, y2 + 1)):
                dst = (y * w + x1) * 2
                fb[dst:dst + sw * 2] = data[row * sw * 2:(row + 1) * sw * 2]
        elif line.startswith(b"SHOT-END") and fb is not None:
            return w, h, fb
    return None


def next_name(base):
    n = 1
    while True:
        path = f"{base}_{n:02d}.png"
        if not os.path.exists(path):
            return path
        n += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default=None,
                    help="serial port (default: autodetect the clock)")
    ap.add_argument("-o", "--out", default="screenshot",
                    help="base name; frames saved as <base>_NN.png")
    ap.add_argument("-1", "--single", action="store_true",
                    help="capture one frame and exit")
    args = ap.parse_args()
    base = args.out[:-4] if args.out.lower().endswith(".png") else args.out

    portname = args.port or find_port()
    if not portname:
        sys.exit("no serial port found — is the clock plugged in? (-p to override)")
    print(f"port: {portname}")
    port = Port(portname)
    while True:
        if not args.single:
            try:
                cmd = input("Enter = capture, q+Enter = quit > ").strip().lower()
            except EOFError:
                cmd = "q"
            if cmd == "q":
                return
        print("capturing…")
        shot = grab(port)
        if shot is None:
            sys.exit("timeout — is the firmware running and the port right?")
        w, h, fb = shot
        path = next_name(base)
        write_png(path, w, h, rgb565_to_rgb888(bytes(fb)))
        print(f"wrote {path} ({w}x{h})")
        if args.single:
            return


if __name__ == "__main__":
    main()
