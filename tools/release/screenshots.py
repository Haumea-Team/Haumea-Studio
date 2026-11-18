#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Retakes docs/images' editor screenshots.

Each shot is a UI script (docs/screenshots/<name>.uis) played in its own Xvfb
with a fresh config, against the real game, on the Grove Street scene
(docs/screenshots/grove.pwn); the script ends with grab "$OUT", which
becomes docs/images/<name>.png (or <name>.pt-BR.png with --lang pt-BR).

    screenshots.py [--game DIR] [--lang LANG] [--out DIR] [name ...]

The game folder defaults to HAUMEA_GAME_DIR, then the one Haumea last used.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHOTS = ROOT / 'docs' / 'screenshots'
sys.path.insert(0, str(ROOT / 'tools' / 'haumea' / 'tests'))
from haumea_test_env import editor_env	# noqa: E402
from test_ui_scripts import start_xvfb	# noqa: E402

WIDTH, HEIGHT = 1600, 900


def game_dir():
    if os.environ.get('HAUMEA_GAME_DIR'):
        return os.environ['HAUMEA_GAME_DIR']
    cfg = Path.home() / '.config' / 'haumea' / 'workspace.cfg'
    if cfg.exists():
        for line in cfg.read_text().splitlines():
            if line.startswith('game '):
                return line[5:]
    return None


def binary():
    found = os.environ.get('HAUMEA_BINARY') or next(iter(sorted(ROOT.glob('bin/*/Release/haumea'))), None)
    return str(found) if found else None


def shoot(name, game, lang, out):
    with tempfile.TemporaryDirectory(prefix='haumea-shot-') as work:
        env = editor_env(work, virtual_display=True)
        config = Path(env['XDG_CONFIG_HOME'], 'haumea')
        config.mkdir(parents=True, exist_ok=True)
        settings = [
            'settings_version 13',
            f'window_width {WIDTH}', f'window_height {HEIGHT}', 'window_maximized 0',
        ]
        if lang:
            settings.append(f'language {lang}')
        (config / 'savesettings.txt').write_text('\n'.join(settings) + '\n')
        scene = Path(work, 'scene')
        shutil.copytree(SHOTS, scene)
        # "# with: hud" adds hud.pwn.part's textdraws to the map.
        if '# with: hud' in (SHOTS / f'{name}.uis').read_text():
            with open(scene / 'grove.pwn', 'a') as f:
                f.write((SHOTS / 'hud.pwn.part').read_text().split('\n', 1)[1])
        env['SCENE'] = str(scene)
        target = out / (f'{name}.{lang}.png' if lang else f'{name}.png')
        env['OUT'] = str(target)
        xvfb, env['DISPLAY'] = start_xvfb()
        env.pop('XAUTHORITY', None)
        try:
            result = subprocess.run([binary(), '--game', game, '--workspace', str(scene),
                                     '--ui-script', str(SHOTS / f'{name}.uis')],
                                    cwd=work, env=env, capture_output=True, text=True, timeout=600)
        finally:
            xvfb.terminate()
            xvfb.wait(10)
        log = [line for line in result.stdout.splitlines() if line.startswith('[ui-script]')]
        failed = result.returncode != 0 or not target.exists()
        if failed:
            print('\n'.join([l for l in log if ': FAIL' in l] + log[-15:]) or result.stdout[-3000:] + result.stderr[-2000:])
        print(f'{"FAIL" if failed else "ok  "} {target.relative_to(ROOT) if target.is_relative_to(ROOT) else target}')
        return not failed


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('names', nargs='*', help='shots to take (default: all)')
    parser.add_argument('--game', default=game_dir())
    parser.add_argument('--lang', default='', help='e.g. pt-BR: the UI language, and <name>.<lang>.png')
    parser.add_argument('--out', type=Path, default=ROOT / 'docs' / 'images')
    args = parser.parse_args()
    if not args.game or not Path(args.game).is_dir():
        sys.exit('no game folder: pass --game or set HAUMEA_GAME_DIR')
    if not binary():
        sys.exit('no bin/*/Release/haumea: build it first')
    names = args.names or sorted(p.stem for p in SHOTS.glob('*.uis'))
    ok = all([shoot(name, args.game, args.lang, args.out.resolve()) for name in names])
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
