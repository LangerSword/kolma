#!/usr/bin/env python3
"""Deterministic synth bed: 30.5 s, 48 kHz stereo, no voice, no external mix."""
import math, random, struct, wave
from pathlib import Path

OUT = Path(__file__).with_name('bed.wav')
SR = 48000
DUR = 30.5
N = int(SR * DUR)
rng = random.Random(20261124)
BEATS = [3.4, 7.0, 11.2, 16.6, 21.2, 25.6, 30.0]

out = bytearray()
noise_l = noise_r = 0.0
for n in range(N):
    t = n / SR
    # slow breathing drone
    lfo = 0.58 + 0.42 * math.sin(2 * math.pi * t / 7.6)
    drone = (math.sin(2 * math.pi * 55 * t)
             + 0.42 * math.sin(2 * math.pi * 110 * t + 0.4)
             + 0.19 * math.sin(2 * math.pi * 165 * t + 0.9))
    base = 0.085 * lfo * drone
    # beat-edge ticks
    tick = 0.0
    for b in BEATS:
        dt = t - b
        if 0 <= dt < 0.42:
            tick += 0.09 * math.exp(-dt * 11) * math.sin(2 * math.pi * (330 + 110 * (b / 30)) * dt)
    # low-passed deterministic air, decorrelated per channel
    white_l = rng.uniform(-1, 1)
    white_r = rng.uniform(-1, 1)
    noise_l += 0.012 * (white_l - noise_l)
    noise_r += 0.012 * (white_r - noise_r)
    l = base + tick + noise_l
    r = base * 0.97 + tick * 0.94 + noise_r
    peak = max(abs(l), abs(r), 1e-9)
    if peak > 0.48:
        scale = 0.48 / peak
        l *= scale
        r *= scale
    out += struct.pack('<hh', int(max(-1, min(1, l)) * 32767), int(max(-1, min(1, r)) * 32767))

with wave.open(str(OUT), 'wb') as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(bytes(out))
print(OUT)