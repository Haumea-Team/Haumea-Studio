# SPDX-License-Identifier: GPL-2.0-or-later
"""Bulk world conversion remains one complete undo beyond the history limit."""
from pathlib import Path
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
from haumea_test_env import editor_env, run_editor
from test_ui_scripts import start_xvfb

BINARY = os.environ.get('HAUMEA_BINARY') or next(iter(sorted(
    glob.glob(str(ROOT / 'bin/*/Release/haumea')))), None)


@unittest.skipUnless(BINARY and shutil.which('Xvfb'), 'needs the editor and Xvfb')
class WorldConversionTests(unittest.TestCase):
    def test_bulk_conversion_undo_and_reload(self):
        with tempfile.TemporaryDirectory(prefix='haumea-world-conversion-') as tmp:
            work = Path(tmp)
            game = work / 'game'
            subprocess.run([sys.executable, str(HERE / 'fixture/make_game.py'), str(game)],
                           check=True, capture_output=True)
            # 150 additional originals plus the fixture's original Pershing pool.
            ipl = game / 'data/maps/fixture.ipl'
            extra = ''.join(f'4206, PershingPool_LAn, 0, {1000 + i * 3}, -1900, 10, 0, 0, 0, 1, -1\n'
                            for i in range(150))
            ipl.write_text(ipl.read_text().replace('end\n', extra + 'end\n', 1))
            source = work / 'map.pwn'
            source.write_text('stock Init() { CreateObject(1271, 2000, -3000, 5, 0, 0, 0); }\n')
            server = work / 'server'
            (server / 'gamemodes').mkdir(parents=True)
            (server / 'models').mkdir()
            script = work / 'bulk.uis'
            script.write_text('''open "$TEST_MAP"
click_world 1479.554688 -1639.609375 13.648438 right
click_item '*' 'Find instances'
wait 5
assert discovery_count 4206 == 151
click_item 'Asset Browser' 'Select all'
wait 5
assert selected == 151
assert selected_world == 151
assert window 'Object Info'
click_item 'Object Info' 'Convert to map object'
wait 5
assert pawn_count 'RemoveBuildingForPlayer(playerid, 4206' == 151
assert pawn_count 'CreateObject(4206,' == 151
move 800 400
key Ctrl+Z
wait 5
assert pawn_count 'RemoveBuildingForPlayer(playerid, 4206' == 0
assert pawn_count 'CreateObject(4206,' == 0
key Ctrl+Y
wait 5
assert pawn_count 'RemoveBuildingForPlayer(playerid, 4206' == 151
assert pawn_count 'CreateObject(4206,' == 151
key Ctrl+S
wait 5
reopen
wait 8
assert pawn_count 'RemoveBuildingForPlayer(playerid, 4206' == 151
assert pawn_count 'CreateObject(4206,' == 151
assert objects == 1
''')
            env = editor_env(work, virtual_display=True)
            env['TEST_MAP'] = str(source)
            config = Path(env['XDG_CONFIG_HOME'], 'haumea')
            config.mkdir()
            (config / 'savesettings.txt').write_text(
                'settings_version 8\nshow_browser_window 0\nshow_instance_window 0\n')
            xvfb, env['DISPLAY'] = start_xvfb()
            env.pop('XAUTHORITY', None)
            try:
                result = run_editor([BINARY, '--game', str(game), '--workspace', str(server),
                                     '--ui-script', str(script)], cwd=work, env=env)
            finally:
                xvfb.terminate()
                xvfb.wait(10)
            log = '\n'.join(line for line in result.stdout.splitlines() if line.startswith('[ui-script]'))
            self.assertEqual(result.returncode, 0, log or result.stdout[-4000:])
            xml = Path(str(source) + '.xml').read_text()
            self.assertEqual(xml.count('forceWorldReplacement="true"'), 151)
