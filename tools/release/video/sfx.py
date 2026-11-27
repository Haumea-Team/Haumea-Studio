#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The tour's music bed and click sound, synthesized (no samples, no licence
to keep track of).

    <venv>/bin/python sfx.py music OUT.wav SECONDS
    <venv>/bin/python sfx.py clicks OUT.wav SECONDS T1 T2 ...	(a click at each time)

The music is a calm A minor loop at 84 bpm (Am F C G): a soft detuned pad,
a sub bass, a plucked arpeggio and a light kick and hat, fading in and out.
"""
import sys
import wave

import numpy as np

SR = 48000
BPM = 84
BEAT = 60.0 / BPM
BAR = BEAT * 4


def note(n):
    """MIDI note to Hz."""
    return 440.0 * 2 ** ((n - 69) / 12)


def lowpass(x, cutoff):
    """One-pole low-pass, twice."""
    a = np.exp(-2 * np.pi * cutoff / SR)
    for _ in range(2):
        y = np.empty_like(x)
        acc = 0.0
        for i, v in enumerate(x):
            acc = (1 - a) * v + a * acc
            y[i] = acc
        x = y
    return x


def env(n, attack, release):
    e = np.ones(n)
    a, r = int(attack * SR), int(release * SR)
    if a:
        e[:a] = np.linspace(0, 1, a)
    if r:
        e[-r:] *= np.linspace(1, 0, r)
    return e


def pad(freqs, seconds, rng):
    n = int(seconds * SR)
    t = np.arange(n) / SR
    x = np.zeros(n)
    for f in freqs:
        for detune in (-0.004, 0.0, 0.0045):
            ph = rng.uniform(0, 2 * np.pi)
            ff = f * (1 + detune)
            # a soft saw: the first few harmonics, falling off
            for h in range(1, 7):
                x += np.sin(2 * np.pi * ff * h * t + ph * h) / (h ** 1.6)
    x *= env(n, 1.2, 1.4)
    return x / len(freqs)


def pluck(f, seconds):
    n = int(seconds * SR)
    t = np.arange(n) / SR
    x = (np.sin(2 * np.pi * f * t) + 0.35 * np.sin(2 * np.pi * 2 * f * t) + 0.12 * np.sin(2 * np.pi * 3 * f * t))
    return x * np.exp(-t * 5.5) * env(n, 0.004, 0.05)


def kick():
    n = int(0.35 * SR)
    t = np.arange(n) / SR
    f = 46 + 70 * np.exp(-t * 28)
    return np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 9)


def hat(rng):
    n = int(0.06 * SR)
    x = rng.standard_normal(n)
    x = x - lowpass(x, 6000)	# keep the top
    return x * np.exp(-np.arange(n) / SR * 70)


def add(track, at, x, gain=1.0):
    i = int(at * SR)
    j = min(len(track), i + len(x))
    if i < len(track):
        track[i:j] += x[:j - i] * gain


def music(seconds):
    rng = np.random.default_rng(84)
    n = int(seconds * SR)
    out = np.zeros(n)
    chords = [(57, [57, 60, 64]), (53, [53, 57, 60]), (48, [48, 52, 55]), (55, [55, 59, 62])]	# Am F C G
    arp = [0, 1, 2, 1, 2, 0, 1, 2]
    pads = {}
    bar = 0
    while bar * BAR < seconds:
        root, chord = chords[bar % len(chords)]
        at = bar * BAR
        key = tuple(chord)
        if key not in pads:
            pads[key] = lowpass(pad([note(c) for c in chord], BAR + 1.4, rng), 1400)
        add(out, at, pads[key], 0.16)
        add(out, at, np.sin(2 * np.pi * note(root - 24) * np.arange(int(BAR * SR)) / SR) * env(int(BAR * SR), 0.05, 0.3), 0.10)
        for k in range(8):	# eighth-note arpeggio, an octave up
            add(out, at + k * BEAT / 2, pluck(note(chord[arp[k]] + 12), 0.9), 0.07)
        if bar >= 2:	# the pulse comes in after the first two bars
            for b in range(4):
                if b in (0, 2):
                    add(out, at + b * BEAT, kick(), 0.22)
                add(out, at + b * BEAT + BEAT / 2, hat(rng), 0.035)
        bar += 1
    out *= env(n, 2.0, 3.0)
    return out / (np.abs(out).max() + 1e-9) * 0.8


def click():
    rng = np.random.default_rng(7)
    n = int(0.03 * SR)
    t = np.arange(n) / SR
    x = np.sin(2 * np.pi * 2300 * t) * np.exp(-t * 260) + rng.standard_normal(n) * np.exp(-t * 400) * 0.25
    return x / np.abs(x).max() * 0.5


def write(path, x):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())


if __name__ == '__main__':
    kind, out = sys.argv[1], sys.argv[2]
    if kind == 'music':
        write(out, music(float(sys.argv[3])))
    else:
        track = np.zeros(int(float(sys.argv[3]) * SR))
        c = click()
        for t in sys.argv[4:]:
            add(track, float(t), c)
        write(out, track)
