# SPDX-License-Identifier: GPL-2.0-or-later
"""MoveObject: imported onto its object, kept relative to it, saved back in both formats.

One editor opens a map over the agent socket whose objects move with
MoveObject (with and without the turn: the client turns only when all three
angles are given, reSA-MP MoveObjectRuntimeToTarget), moves one with the agent,
saves the map native and streamer, and reads the calls back.
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

MAP = '''public OnGameModeInit()
{
	new door = CreateObject(1225, 2000.0, -3000.0, 10.0, 0.0, 0.0, 0.0);
	MoveObject(door, 2000.0, -3000.0, 13.5, 1.5);
	new gate = CreateObject(1225, 2010.0, -3000.0, 10.0, 0.0, 0.0, 0.0);
	MoveObject(gate, 2014.0, -3000.0, 10.0, 2.0, 0.0, 0.0, 90.0);
	new partial = CreateObject(1225, 2020.0, -3000.0, 10.0, 0.0, 0.0, 0.0);
	MoveObject(partial, 2020.0, -2998.0, 10.0, 1.0, 0.0, -1000.0, 90.0);
	CreateObject(1225, 2030.0, -3000.0, 10.0, 0.0, 0.0, 0.0);
	CreateObject(1225, 2050.0, -3000.0, 10.0, 0.0, 0.0, 0.0);
	return 1;
}
'''


def close(a, b, tolerance=0.01):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


@unittest.skipUnless(BINARY and XVFB and sys.platform.startswith('linux'), 'needs the editor and xvfb-run on Linux')
class MovingObjectTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='hmo-', dir='/tmp'))
        game = cls.tmp / 'game'
        subprocess.run([sys.executable, str(HERE / 'fixture' / 'make_game.py'), str(game)],
                       check=True, capture_output=True)
        (cls.tmp / 'moves.pwn').write_text(MAP)
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
        cls.client.command('map_open', str(cls.tmp / 'moves.pwn'))
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

    def item(self, x):
        for item in self.client.command('list')['instances']:
            if item['model_id'] == 1225 and abs(item['position'][0] - x) < 0.01:
                return item
        self.fail('no barrel at x=%s' % x)

    def test_import(self):
        door = self.item(2000.0)['properties']
        self.assertTrue(close(door['move_offset'], [0, 0, 3.5]), door)
        self.assertAlmostEqual(door['move_speed'], 1.5, places=3)
        self.assertNotIn('move_rotation', door)
        gate = self.item(2010.0)['properties']
        self.assertTrue(close(gate['move_offset'], [4, 0, 0]), gate)
        self.assertTrue(close(gate['move_rotation'], [0, 0, 90]), gate)
        # -1000 on one axis: the client doesn't turn it at all.
        self.assertNotIn('move_rotation', self.item(2020.0)['properties'])
        self.assertNotIn('move_offset', self.item(2030.0)['properties'])

    def test_save_both_formats(self):
        still = self.item(2030.0)
        self.client.command('session_begin', 'move test')
        self.client.command('properties', str(still['instance_id']), 'move_offset=[0, 0, -2]', 'move_speed=4')
        # Moving the object takes its movement along.
        self.client.command('transform', str(still['instance_id']), '2040', '-3000', '10', '0', '0')
        self.client.command('session_commit')
        native = self.tmp / 'native.pwn'
        self.client.command('save', str(native), 'native')
        text = native.read_text()
        self.assertRegex(text, r'MoveObject\((\w+), 2000\.000000, -3000\.000000, 13\.500000, 1\.500000\);')
        self.assertRegex(text, r'MoveObject\(\w+, 2014\.000000, -3000\.000000, 10\.000000, 2\.000000, '
                               r'0\.000000, 0\.000000, 90\.000000\);')
        self.assertRegex(text, r'MoveObject\(\w+, 2040\.000000, -3000\.000000, 8\.000000, 4\.000000\);')
        streamer = self.tmp / 'streamer.pwn'
        self.client.command('save', str(streamer), 'streamer')
        text = streamer.read_text()
        self.assertNotIn('MoveObject(', text)
        self.assertRegex(text, r'MoveDynamicObject\(door, 2000\.000000, -3000\.000000, 13\.500000, 1\.500000\);')
        self.assertRegex(text, r'MoveDynamicObject\(\w+, 2040\.000000, -3000\.000000, 8\.000000, 4\.000000\);')

    def test_prefab_keeps_movement(self):
        gate = self.item(2010.0)
        saved = self.client.command('prefab_save', 'gate_test', '', '', 'first', 'mine', str(gate['instance_id']))
        self.assertTrue(saved.get('saved_count'), saved)
        self.client.command('session_begin', 'prefab test')
        placed = self.client.command('prefab_place', 'gate_test', '2100', '-3000', '10', '90', '0')
        self.client.command('session_commit')
        self.assertEqual(placed.get('created_count'), 1, placed)
        copy = self.item(2100.0)['properties']
        # Turned with the prefab: 4 m along X becomes 4 m along Y, the turn goes on to 180.
        self.assertTrue(close(copy['move_offset'], [0, 4, 0]), copy)
        self.assertTrue(close(copy['move_rotation'], [0, 0, 180]), copy)
        self.assertAlmostEqual(copy['move_speed'], 2.0, places=3)

    def test_map_check_still(self):
        self.client.command('session_begin', 'still test')
        item = self.item(2050.0)
        self.client.command('properties', str(item['instance_id']), 'move_offset=[0, 0, 0]')
        self.client.command('session_commit')
        texts = [i['text'] for i in self.client.command('map_check')['items']]
        self.assertTrue(any("moves 0 m and doesn't turn" in t for t in texts), texts)


if __name__ == '__main__':
    unittest.main()
