#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""One narration line as a WAV, read a phrase at a time with a real pause
after each comma and full stop, as LongWayDrivers' cutscene voices do.

    <venv>/bin/python narrate.py MODEL_DIR VOICE LANG SPEED OUT.wav < text

VOICE is a Kokoro voice (af_heart, pf_dora...; MODEL_DIR holds
kokoro-v1.0.onnx and voices-v1.0.bin) or a Piper .onnx path. LANG is
Kokoro's (en-us, pt-br...). SPEED: Kokoro's, 1.0 normal, higher faster.
"""
import re
import sys
import wave
from pathlib import Path

import numpy as np

PAUSE = {'.': 0.34, '!': 0.34, '?': 0.34, ',': 0.14, ':': 0.2, ';': 0.2}


def trim(x, rate, threshold=0.012):
    loud = np.flatnonzero(np.abs(x) > threshold)
    if not len(loud):
        return x
    pad = int(0.03 * rate)
    return x[max(0, loud[0] - pad):loud[-1] + pad]


def main():
    model_dir, voice, lang, speed, out = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), sys.argv[5]
    text = sys.stdin.read().strip()
    if voice.endswith('.onnx'):
        import io
        import piper
        from piper import SynthesisConfig
        engine = piper.PiperVoice.load(voice)

        def say(phrase):
            buf = io.BytesIO()
            with wave.open(buf, 'wb') as w:
                engine.synthesize_wav(phrase, w, syn_config=SynthesisConfig(length_scale=1.0 / speed))
            buf.seek(0)
            with wave.open(buf) as w:
                return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768.0, w.getframerate()
    else:
        from kokoro_onnx import Kokoro
        engine = Kokoro(str(Path(model_dir) / 'kokoro-v1.0.onnx'), str(Path(model_dir) / 'voices-v1.0.bin'))

        def say(phrase):
            samples, rate = engine.create(phrase, voice=voice, speed=speed, lang=lang)
            return np.asarray(samples, np.float64), rate
    phrases = [p for p in re.split(r'(?<=[.,!?:;])\s+', text) if p]
    parts, rate = [], 24000
    for i, phrase in enumerate(phrases):
        x, rate = say(phrase)
        parts.append(trim(x, rate))
        if i + 1 < len(phrases):
            parts.append(np.zeros(int(PAUSE.get(phrase[-1], 0.1) * rate)))
    x = np.concatenate(parts)
    x = x / (np.abs(x).max() + 1e-9) * 0.89
    with wave.open(out, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes((x * 32767).astype(np.int16).tobytes())


if __name__ == '__main__':
    main()
