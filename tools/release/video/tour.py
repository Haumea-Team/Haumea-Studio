#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Records the Haumea Studio tour video: one continuous take, narrated.

tour.uis plays the whole tour in one editor session on the Grove Street
scene (docs/screenshots), against the real game, in its own Xvfb, in the
UI's language, with the script runner's drawn, gliding cursor. Every frame
between record_start and record_stop is a 30th of a second of video, however
slowly it draws. Each part of the tour starts with mark <id> and ends with
hold_for {<id>}, which becomes the length of that part's line (tour.json),
voiced first by Kokoro; the lines are then laid onto the take where their
parts start, and captioned:

    build/video/haumea-tour.<lang>.mp4 (and .srt)

    tour.py [--game DIR] [--lang en]

Kokoro (and Piper) live in a venv in ~/.cache/haumea-video (see setup()).
Besides the usual UI script commands, tour.uis may use
fly X Y Z TX TY TZ -> X Y Z TX TY TZ FRAMES, a smooth camera move.
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SHOTS = ROOT / 'docs' / 'screenshots'
OUT = ROOT / 'build' / 'video'
sys.path.insert(0, str(ROOT / 'tools' / 'release'))
sys.path.insert(0, str(ROOT / 'tools' / 'haumea' / 'tests'))
from screenshots import WIDTH, HEIGHT, game_dir, binary	# noqa: E402
from haumea_test_env import editor_env	# noqa: E402
from test_ui_scripts import start_xvfb	# noqa: E402

CACHE = Path.home() / '.cache' / 'haumea-video'
PYTHON = CACHE / 'venv' / 'bin' / 'python'
KOKORO = CACHE / 'kokoro'
TOOLCHAIN = ROOT / 'tools' / 'haumea' / 'tests' / 'pawn' / '.toolchain'
KOKORO_FILES = 'https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/'
# lang -> (Kokoro voice, Kokoro language, speed)
VOICES = {'en': ('af_heart', 'en-us', 1.0)}
# Captions keep the names as written; the voice says them in full.
SPOKEN = [('open.mp', 'Open Multiplayer'), ('SA-MP', 'San Andreas Multiplayer')]
FPS = 30
LEAD = 0.5	# seconds into a part before its line starts
TAIL = 0.9	# and after it ends, before the next part


def run(*args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def setup():
    """Kokoro in a Python 3.12 venv (onnxruntime has no wheels for newer ones) and its model."""
    if not PYTHON.exists():
        run('uv', 'venv', '-q', '--python', '3.12', CACHE / 'venv')
    if subprocess.run([str(PYTHON), '-c', 'import kokoro_onnx'], capture_output=True).returncode:
        run('uv', 'pip', 'install', '-q', '--python', PYTHON, 'kokoro-onnx', 'piper-tts')
    if not (TOOLCHAIN / 'omp' / 'Server' / 'omp-server').exists():
        run(TOOLCHAIN.parent / 'fetch_toolchain.sh', TOOLCHAIN)
    KOKORO.mkdir(parents=True, exist_ok=True)
    for name in ('kokoro-v1.0.onnx', 'voices-v1.0.bin'):
        if not (KOKORO / name).exists():
            run('curl', '-sfL', '-o', KOKORO / name, KOKORO_FILES + name)


def duration(path):
    out = run('ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', path,
              capture_output=True, text=True).stdout
    return float(out.strip())


def speak(text, lang, wav):
    for written, said in SPOKEN:
        text = text.replace(written, said)
    voice, code, speed = VOICES[lang]
    run(PYTHON, HERE / 'narrate.py', KOKORO, voice, code, speed, wav, input=text, text=True, capture_output=True)


def expand(script, holds):
    """fly lines into a camera line a frame, daylapse H M -> H M FRAMES into a
    time line a frame; {id} into the part's length."""
    out = []
    for line in script.splitlines():
        line = re.sub(r'\{(\w+)\}', lambda m: f'{holds[m[1]]:.2f}', line)
        d = re.match(r'\s*daylapse\s+(\d+)\s+(\d+)\s*->\s*(\d+)\s+(\d+)\s+(\d+)\s*$', line)
        if d:
            start, end, frames = int(d[1]) * 60 + int(d[2]), int(d[3]) * 60 + int(d[4]), int(d[5])
            for f in range(frames + 1):
                t = f / frames
                minute = round(start + (end - start) * t * t * (3 - 2 * t))
                out.append(f'time {minute // 60} {minute % 60}')
            continue
        m = re.match(r'\s*fly\s+(.*?)\s*->\s*(.*?)\s+(\d+)\s*$', line)
        if not m:
            out.append(line)
            continue
        a, b, frames = [float(v) for v in m[1].split()], [float(v) for v in m[2].split()], int(m[3])
        for f in range(frames + 1):
            t = f / frames
            t = t * t * (3 - 2 * t)	# smoothstep: eases in and out
            out.append('camera ' + ' '.join(f'{x + (y - x) * t:.3f}' for x, y in zip(a, b)))
    return '\n'.join(out) + '\n'


def record(lang, game, holds, video):
    """Plays tour.uis into video: the frame each part starts at and each click's, or None."""
    script_text = (HERE / 'tour.uis').read_text()
    with tempfile.TemporaryDirectory(prefix='haumea-tour-') as work:
        env = editor_env(work, virtual_display=True)
        config = Path(env['XDG_CONFIG_HOME'], 'haumea')
        config.mkdir(parents=True, exist_ok=True)
        settings = ['settings_version 13', f'window_width {WIDTH}', f'window_height {HEIGHT}',
                    'window_maximized 0']
        if lang != 'en':
            settings.append(f'language {lang}')
        (config / 'savesettings.txt').write_text('\n'.join(settings) + '\n')
        # Test in game's downloaded server, from the Pawn toolchain: no wait for it.
        server = config / 'testserver' / 'download'
        shutil.copytree(TOOLCHAIN / 'omp' / 'Server', server)
        (server / 'plugins').mkdir(exist_ok=True)
        shutil.copy(TOOLCHAIN / 'streamer' / 'streamer.so', server / 'plugins')
        shutil.copy(TOOLCHAIN / 'streamer' / 'streamer.inc', server / 'qawno' / 'include')
        stage = Path(work, 'scene')
        shutil.copytree(SHOTS, stage)
        if '# with: hud' in script_text:
            with open(stage / 'grove.pwn', 'a') as f:
                f.write((SHOTS / 'hud.pwn.part').read_text().split('\n', 1)[1])
        script = Path(work, 'tour.uis')
        script.write_text(expand(script_text, holds))
        env['SCENE'] = str(stage)
        env['CLIP'] = str(video)
        env['HAUMEA_UI_SCRIPT_PRESENT'] = '1'
        xvfb, env['DISPLAY'] = start_xvfb()
        env.pop('XAUTHORITY', None)
        try:
            result = subprocess.run([binary(), '--game', game, '--workspace', str(stage), '--ui-script', str(script)],
                                    cwd=work, env=env, capture_output=True, text=True, timeout=3600)
        finally:
            xvfb.terminate()
            xvfb.wait(10)
            stop_servers(work)
    log = [l for l in result.stdout.splitlines() if l.startswith('[ui-script]')]
    failed = [l for l in log if ': FAIL' in l]
    if failed or result.returncode != 0 or not video.exists():
        print('\n'.join(failed or log[-20:]) or result.stdout[-2000:] + result.stderr[-2000:])
        return None
    marks = {m[1]: int(m[2]) for m in (re.search(r'\] mark (\w+) frame (\d+)', l) for l in log) if m}
    clicks = [int(m[1]) for m in (re.search(r'\] click frame (\d+)', l) for l in log) if m]
    return marks, clicks


def stop_servers(work):
    """The test server the take started, which outlives the editor's exit."""
    for proc in Path('/proc').iterdir():
        try:
            if proc.name.isdigit() and os.readlink(proc / 'cwd').startswith(str(Path(work).resolve())):
                os.kill(int(proc.name), signal.SIGKILL)
        except OSError:
            pass


def srt_time(t):
    ms = int(round(t * 1000))
    return f'{ms // 3600000:02}:{ms // 60000 % 60:02}:{ms // 1000 % 60:02},{ms % 1000:03}'


CARD_IN, CARD_OUT, FADE = 3.0, 4.0, 0.6	# seconds: the title, the end card, their crossfades
URL = 'github.com/Haumea-Team/Haumea-Studio'


def cards(tmp):
    """The title and end cards: the social preview, its edges filled with a blur of itself."""
    art = ROOT / 'docs' / 'images' / 'social-preview.png'
    font = ROOT / 'fonts' / 'Inter-Regular.ttf'
    size = f'{WIDTH - 16}x{HEIGHT}'
    fill = (f'[0:v]split[a][b];[a]scale={size.replace("x", ":")},boxblur=40[bg];'
            f'[b]scale={WIDTH - 16}:-2[fg];[bg][fg]overlay=0:(H-h)/2')
    title, end = tmp / 'title.png', tmp / 'end.png'
    run('ffmpeg', '-loglevel', 'error', '-y', '-i', art, '-filter_complex', fill, '-frames:v', '1', title)
    run('ffmpeg', '-loglevel', 'error', '-y', '-i', art, '-filter_complex',
        f"{fill},drawtext=fontfile='{font}':text='{URL}':fontsize=34:fontcolor=0xB8C4F0:x=104:y=H*0.70",
        '-frames:v', '1', end)
    return title, end


def assemble(tour, lang, lines, marks, clicks, take, tmp):
    """Title card, the take with its lines and captions, end card; music under it all."""
    length = duration(take)
    shift = CARD_IN - FADE	# where the take starts in the video
    total = shift + length + CARD_OUT - FADE
    order = [s['id'] for s in tour['scenes']]
    title, end = cards(tmp)
    music, ticks = tmp / 'music.wav', tmp / 'clicks.wav'
    run(PYTHON, HERE / 'sfx.py', 'music', music, f'{total:.2f}')
    run(PYTHON, HERE / 'sfx.py', 'clicks', ticks, f'{total:.2f}', *[f'{shift + c / FPS:.3f}' for c in clicks])
    name = OUT / f'haumea-tour.{lang}'
    srt = Path(f'{name}.srt')	# times in the take
    srt.write_text(''.join(
        f'{n}\n{srt_time(marks[i] / FPS + LEAD)} --> {srt_time(marks[i] / FPS + LEAD + lines[i][1])}\n{lines[i][2]}\n\n'
        for n, i in enumerate(order, 1)))
    style = ('FontName=Inter,FontSize=12,PrimaryColour=&H00FFFFFF,OutlineColour=&H80000000,'
             'BorderStyle=3,Outline=6,Shadow=0,MarginV=36')
    zoom = (f"zoompan=z='1+0.00045*on':d=1:x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':"
            f"s={WIDTH - 16}x{HEIGHT}:fps={FPS},format=yuv420p")
    voices = sum((['-i', str(lines[i][0])] for i in order), [])
    first = 5	# inputs: take, title, end, music, clicks, then the lines
    delays = ''.join(f'[{first + n}:a]adelay={int((shift + marks[i] / FPS + LEAD) * 1000)}:all=1[v{n}];'
                     for n, i in enumerate(order))
    graph = (
        f"[0:v]subtitles='{srt}':force_style='{style}',fps={FPS},format=yuv420p,setsar=1[take];"
        f"[1:v]{zoom},trim=duration={CARD_IN},setsar=1[t];"
        f"[2:v]{zoom},trim=duration={CARD_OUT},setsar=1[e];"
        f"[t][take]xfade=transition=fade:duration={FADE}:offset={shift:.3f}[x];"
        f"[x][e]xfade=transition=fade:duration={FADE}:offset={shift + length - FADE:.3f}[v];"
        f"{delays}{''.join(f'[v{n}]' for n in range(len(order)))}amix=inputs={len(order)}:normalize=0,"
        f"apad,atrim=duration={total:.3f},asplit[voice][key];"
        f"[3:a]volume=0.55[bed];[bed][key]sidechaincompress=threshold=0.02:ratio=6:attack=30:release=500[ducked];"
        f"[4:a]volume=0.5[tick];"
        f"[voice][ducked][tick]amix=inputs=3:normalize=0,alimiter=limit=0.95,aresample=48000[a]")
    run('ffmpeg', '-loglevel', 'error', '-y', '-i', take, '-loop', '1', '-i', title, '-loop', '1', '-i', end,
        '-i', music, '-i', ticks, *voices, '-filter_complex', graph, '-map', '[v]', '-map', '[a]',
        '-t', f'{total:.3f}', '-c:v', 'libx264', '-crf', '20', '-preset', 'slow', '-pix_fmt', 'yuv420p',
        '-c:a', 'aac', '-b:a', '192k', '-movflags', '+faststart', f'{name}.mp4')
    print(f'{name}.mp4 ({total:.0f} s)')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--game', default=game_dir())
    parser.add_argument('--lang', default='en')
    args = parser.parse_args()
    if not args.game or not Path(args.game).is_dir():
        sys.exit('no game folder: pass --game or set HAUMEA_GAME_DIR')
    if not binary():
        sys.exit('no bin/*/Release/haumea: build it first')
    setup()
    tour = json.loads((HERE / 'tour.json').read_text())
    OUT.mkdir(parents=True, exist_ok=True)
    ok = True
    for lang in args.lang.split(','):
        with tempfile.TemporaryDirectory(prefix='haumea-tour-audio-') as tmp:
            lines, holds = {}, {}
            for scene in tour['scenes']:
                wav = Path(tmp, f'{scene["id"]}.wav')
                speak(scene[lang], lang, wav)
                lines[scene['id']] = (wav, duration(wav), scene[lang])
                holds[scene['id']] = LEAD + lines[scene['id']][1] + TAIL
            print(f'{lang}: recording ({sum(holds.values()):.0f} s of narration)', flush=True)
            take = Path(tmp, 'take.mp4')
            recorded = record(lang, args.game, holds, take)
            if recorded is None or set(recorded[0]) != set(lines):
                print(f'{lang}: FAILED')
                ok = False
                continue
            marks, clicks = recorded
            assemble(tour, lang, lines, marks, clicks, take, Path(tmp))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
