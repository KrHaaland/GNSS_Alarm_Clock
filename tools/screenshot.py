#!/usr/bin/env python3
"""Grab a screenshot from the clock over USB serial.

Navigate the clock to the screen you want, then:
    python3 tools/screenshot.py [-p /dev/ttyACM0] [-o out.png]

Sends '~' to the firmware, which streams the next rendered frame as raw
RGB565-LE strips (SHOT-BEGIN/AREA/END protocol in DisplayNV3007.cpp), and
writes a PNG (pure stdlib — no PIL needed). Uses pyserial when installed;
falls back to raw POSIX tty handling on Linux otherwise.
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
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    ap.add_argument("-o", "--out", default="screenshot.png")
    args = ap.parse_args()

    port = Port(args.port)
    port.write(b"~")
    print("sent '~', waiting for frame...")

    fb = None
    w = h = 0
    deadline = time.time() + 15
    while time.time() < deadline:
        line = port.read_line()
        if line is None:
            sys.exit("timeout — is the firmware running and the port right?")
        if line.startswith(b"SHOT-BEGIN"):
            _, ws, hs = line.split()
            w, h = int(ws), int(hs)
            fb = bytearray(w * h * 2)
            print(f"frame {w}x{h}")
        elif line.startswith(b"SHOT-AREA") and fb is not None:
            _, x1, y1, x2, y2 = line.split()
            x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)
            sw = x2 - x1 + 1
            data = port.read((x2 - x1 + 1) * (y2 - y1 + 1) * 2)
            for row, y in enumerate(range(y1, y2 + 1)):
                dst = (y * w + x1) * 2
                fb[dst:dst + sw * 2] = data[row * sw * 2:(row + 1) * sw * 2]
        elif line.startswith(b"SHOT-END") and fb is not None:
            write_png(args.out, w, h, rgb565_to_rgb888(bytes(fb)))
            print(f"wrote {args.out} ({w}x{h})")
            return
    sys.exit("gave up waiting for SHOT-END")


if __name__ == "__main__":
    main()
