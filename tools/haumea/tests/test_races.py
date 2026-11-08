# SPDX-License-Identifier: GPL-2.0-or-later
"""Races: circuits, laps, automatic types and playable races for haumea_races.inc.

One editor opens a map of two marker races (CreateDynamicRaceCP chained by
their next points) over the agent socket, makes one a playable circuit of 3
laps, saves it as streamer and native code, compiles both (open.mp with the
streamer, SA-MP 0.3.7 without) with haumea_races.inc, and opens the native
save again without its XML, so the races come back from MapRace_Create.
"""
from pathlib import Path
import glob
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / 'tools' / 'agent'))
from haumea_test_env import editor_env  # noqa: E402

BINARY = os.environ.get('HAUMEA_BINARY') or next(iter(sorted(
    glob.glob(str(ROOT / 'bin' / '*' / 'Release' / 'haumea')))), None)
XVFB = shutil.which('xvfb-run')

import re

PAWN = HERE / 'pawn'
TOOLCHAIN = Path(os.environ.get('PAWN_TOOLCHAIN', PAWN / '.toolchain'))

MAP = '''#include <a_samp>
#include <streamer>

public OnGameModeInit()
{
	CreateDynamicRaceCP(0, 2000.0, -3000.0, 10.0, 2050.0, -3000.0, 10.0, 6.0);
	CreateDynamicRaceCP(0, 2050.0, -3000.0, 10.0, 2050.0, -2950.0, 10.0, 6.0);
	CreateDynamicRaceCP(1, 2050.0, -2950.0, 10.0, 0.0, 0.0, 0.0, 6.0);
	CreateDynamicRaceCP(0, 1900.0, -3100.0, 10.0, 1950.0, -3100.0, 10.0, 4.0);
	CreateDynamicRaceCP(1, 1950.0, -3100.0, 10.0, 0.0, 0.0, 0.0, 4.0);
	return 1;
}
'''


@unittest.skipUnless(BINARY and XVFB and sys.platform.startswith('linux'), 'needs the editor and xvfb-run on Linux')
class RaceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='hra-', dir='/tmp'))
        game = cls.tmp / 'game'
        subprocess.run([sys.executable, str(HERE / 'fixture' / 'make_game.py'), str(game)],
                       check=True, capture_output=True)
        (cls.tmp / 'races.pwn').write_text(MAP)
        cls.socket = cls.tmp / 'a.sock'
        env = editor_env(str(cls.tmp), virtual_display=True)
        env['HAUMEA_TEST_XVFB'] = '1'
        cls.log = open(cls.tmp / 'out.txt', 'w')
        cls.editor = subprocess.Popen(
            [XVFB, '-a', '-s', '-screen 0 1280x800x24', BINARY, '--game', str(game),
             '--agent-socket', str(cls.socket)],
            cwd=cls.tmp, env=env, stdout=cls.log, stderr=subprocess.STDOUT, start_new_session=True)
        from haumea_ipc import HaumeaClient
        cls.client = HaumeaClient(str(cls.socket))
        end = time.time() + 240
        while time.time() < end and cls.editor.poll() is None:
            try:
                if cls.client.command('ping').get('result') == 'pong':
                    break
            except Exception:
                time.sleep(0.5)
        else:
            raise AssertionError('the editor did not answer:\n' + cls.output())
        cls.client.command('map_open', str(cls.tmp / 'races.pwn'))
        cls.items = cls.client.command('list')['instances']

    @classmethod
    def tearDownClass(cls):
        try:
            os.killpg(cls.editor.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        cls.editor.wait()
        cls.log.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def output(cls):
        return (cls.tmp / 'out.txt').read_text(errors='replace')[-3000:]

    def checkpoints(self, race):
        items = [i for i in self.client.command('list')['instances']
                 if i['properties'].get('race') == race]
        return sorted(items, key=lambda i: i['properties']['order'])

    def compile(self, map_path, target):
        gm = map_path.with_name(map_path.stem + '_gm.pwn')
        subprocess.run(['sh', str(PAWN / 'mkgm.sh'), str(map_path), str(gm), target], check=True)
        includes = [str(TOOLCHAIN / ('omp/Server/qawno/include' if target == 'openmp' else 'samp037')),
                    str(TOOLCHAIN / 'streamer'), str(HERE.parent)]
        out = subprocess.run(['sh', str(PAWN / 'cc.sh'), str(gm)] + includes, capture_output=True, text=True,
                             env=dict(os.environ, PAWN_TOOLCHAIN=str(TOOLCHAIN))).stdout
        problems = [l for l in out.splitlines() if re.search(r': (fatal )?(error|warning) [0-9]+', l)]
        self.assertEqual(problems, [], out[-3000:])
        self.assertTrue(gm.with_suffix('.amx').exists(), out[-3000:])

    def test_playable_circuit(self):
        races = sorted({i['properties']['race'] for i in self.client.command('list')['instances']})
        self.assertEqual(len(races), 2, races)
        loop = self.checkpoints(races[0]) if len(self.checkpoints(races[0])) == 3 else self.checkpoints(races[1])
        name = loop[0]['properties']['race']
        self.client.command('session_begin', 'race test')
        self.client.command('properties', str(loop[0]['instance_id']), 'race_mode=circuit', 'race_laps=3',
                            'race_playable=true', 'race_type=9')
        self.client.command('session_commit')
        # The settings reach the race's other checkpoints.
        for item in self.checkpoints(name):
            p = item['properties']
            self.assertEqual((p['race_mode'], p.get('race_laps'), p['race_playable']), ('circuit', 3, True), p)

        streamer = self.tmp / 'race_streamer.pwn'
        self.client.command('save', str(streamer), 'streamer')
        text = streamer.read_text()
        prefix = re.search(r'stock (\w+)_CreateObjects', text).group(1)
        self.assertIn('#tryinclude <haumea_races>', text)
        self.assertIn('#define %s_race_%s (%s_RACE_BASE + 1)' % (prefix, name, prefix), text)
        self.assertIn('MapRace_Create(%s_race_%s, "%s", MAPRACE_CIRCUIT, 3, -1, -1);' % (prefix, name, name), text)
        adds = re.findall(r'MapRace_AddCheckpoint\(\w+, ([-\d.]+), ([-\d.]+), [-\d.]+, ([\d.]+), (\d+)\);', text)
        self.assertEqual([(float(x), float(y), int(t)) for x, y, _, t in adds],
                         [(2000.0, -3000.0, 9), (2050.0, -3000.0, 0), (2050.0, -2950.0, 1)], adds)
        # The other race stays as markers.
        self.assertEqual(text.count('CreateDynamicRaceCP('), 2, text)
        if (TOOLCHAIN / 'bin' / 'pawncc').exists():
            self.compile(streamer, 'openmp')

        native = self.tmp / 'race_native.pwn'
        self.client.command('save', str(native), 'native')
        text = native.read_text()
        self.assertEqual(text.count('MapRace_AddCheckpoint('), 3)
        self.assertNotIn('CreateDynamicRaceCP(', text)
        self.assertNotIn('#include <streamer>', text)
        if (TOOLCHAIN / 'bin' / 'pawncc').exists():
            self.compile(native, 'samp037')

        # Without the XML the races come back from the Pawn.
        (self.tmp / 'race_native.pwn.xml').unlink(missing_ok=True)
        self.client.command('map_open', str(native))
        back = self.checkpoints(name)
        self.assertEqual(len(back), 3, self.client.command('list'))
        self.assertEqual([i['properties']['race_type'] for i in back], [9, 0, 1])
        for item in back:
            p = item['properties']
            self.assertEqual((p['race_mode'], p.get('race_laps'), p['race_playable']), ('circuit', 3, True), p)

    def test_automatic_types_as_markers(self):
        races = sorted({i['properties']['race'] for i in self.client.command('list')['instances']})
        sprint = next(self.checkpoints(r) for r in races if len(self.checkpoints(r)) == 2)
        self.client.command('session_begin', 'auto test')
        for item in sprint:
            self.client.command('properties', str(item['instance_id']), 'race_type=10')
        self.client.command('session_commit')
        out = self.tmp / 'auto.pwn'
        self.client.command('save', str(out), 'streamer')
        types = [int(t) for t, x in re.findall(r'CreateDynamicRaceCP\((\d+), (19[05]0)\.', out.read_text())]
        # Air normal, then air finish on the last.
        self.assertEqual(types, [3, 4])


if __name__ == '__main__':
    unittest.main()
