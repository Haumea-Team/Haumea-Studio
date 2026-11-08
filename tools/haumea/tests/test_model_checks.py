# SPDX-License-Identifier: GPL-2.0-or-later
"""Pawn arithmetic on import, and what Map check says about trees and explosive models.

On the synthetic game (fixture/make_game.py: tree_hipoly06 728 has the IDE
tree flag 0x2000, and object.dat says barrel4 1225 causes an explosion), one
editor opens a map over the agent socket whose arguments are expressions over
#defines and consts, then reads back where the objects stand and runs Map check.
A tree's X/Y tilt is lost in game: CEntity::ModifyMatrixForTreeInWind
(gta-reversed Entity.cpp) rewrites its up axis every frame.
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

MAP = '''#define BASE_X 2000.0
#define BASE_Z (10.0 + 2.5)
#define MACRO(%0) (%0 * 2)
const Float:STEP = 4.0 * 2;

public OnGameModeInit()
{
	CreateObject(1225, BASE_X + STEP, -3000.0 - 0.5, BASE_Z, 0.0, 0.0, 90.0 / 2);
	CreateObject(728, BASE_X, -3000.0 + 0x10, BASE_Z * 2, 30.0, 0.0, 0.0);
	CreateObject(728, BASE_X + 20.0, -3000.0, 10.0, 0.0, 0.0, 135.0);
	new guard = CreateActor(280, 2030.0, -3000.0, 10.0, 0.0);
	ApplyActorAnimation(guard, "BD_FIRE", "BD_Fire1", 4.1, 1, 0, 0, 0, 0);
	new jumper = CreateActor(280, 2040.0, -3000.0, 10.0, 0.0);
	ApplyActorAnimation(jumper, "PARACHUTE", "PARA_decel_O", 4.1, 1, 0, 0, 0, 0);
	new walker = CreateActor(280, 2050.0, -3000.0, 10.0, 0.0);
	ApplyActorAnimation(walker, "PED", "WALK_civi", 4.1, 1, 0, 0, 0, 0);
	return 1;
}
'''


def close(a, b, tolerance=0.01):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


@unittest.skipUnless(BINARY and XVFB and sys.platform.startswith('linux'), 'needs the editor and xvfb-run on Linux')
class ModelCheckTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='hmc-', dir='/tmp'))
        game = cls.tmp / 'game'
        subprocess.run([sys.executable, str(HERE / 'fixture' / 'make_game.py'), str(game)],
                       check=True, capture_output=True)
        (cls.tmp / 'checks.pwn').write_text(MAP)
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
        cls.client.command('map_open', str(cls.tmp / 'checks.pwn'))
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

    def find(self, model, position):
        for item in self.items:
            if item['model_id'] == model and close(item['position'], position):
                return item
        self.fail('no %d at %s in %s' % (model, position, self.items))

    def test_expressions_import(self):
        # BASE_X + STEP; -3000.0 - 0.5; (10.0 + 2.5); 0x10 is 16; BASE_Z * 2.
        self.find(1225, [2008.0, -3000.5, 12.5])
        self.find(728, [2000.0, -2984.0, 25.0])

    def test_tilted_tree_warns(self):
        tilted = self.find(728, [2000.0, -2984.0, 25.0])
        upright = self.find(728, [2020.0, -3000.0, 10.0])
        items = self.client.command('map_check')['items']
        trees = [i for i in items if 'tree(s) or palm(s)' in i['text']]
        self.assertEqual(len(trees), 1, items)
        self.assertEqual(trees[0]['severity'], 'warning')
        self.assertEqual(trees[0]['instance_ids'], [tilted['instance_id']])
        self.assertNotIn(upright['instance_id'], trees[0]['instance_ids'])

    def test_explosive_noted(self):
        barrel = self.find(1225, [2008.0, -3000.5, 12.5])
        items = self.client.command('map_check')['items']
        boom = [i for i in items if 'explode when shot' in i['text']]
        self.assertEqual(len(boom), 1, items)
        self.assertEqual(boom[0]['instance_ids'], [barrel['instance_id']])

    def test_animations_a_skin_cannot_play(self):
        items = self.client.command('map_check')['items']
        texts = [i['text'] for i in items]
        self.assertTrue(any('BD_FIRE / BD_Fire1' in t and "object's parts" in t for t in texts), texts)
        self.assertTrue(any('PARACHUTE / PARA_decel_O' in t and 'twists the skin' in t for t in texts), texts)
        self.assertFalse(any('WALK_civi' in t for t in texts), texts)

    def test_tiling_textures(self):
        result = self.client.command('texture_search', 'trafficcone', '20')
        found = [t for t in result['textures'] if t['texture'].lower() == 'trafficcone']
        self.assertTrue(found, result)
        self.assertEqual(found[0].get('tiles'), 'both', found)

    def test_duplicate_with_offset(self):
        tree = self.find(728, [2020.0, -3000.0, 10.0])
        self.client.command('session_begin', 'duplicate test')
        self.client.command('maptool', 'duplicate', 'ids=%d' % tree['instance_id'], 'offset=0,5,1', 'rotate=0,0,90')
        self.client.command('session_commit')
        items = self.client.command('list')['instances']
        # Turned around the centre of its box, so its pivot moves a little too.
        copies = [i for i in items if i['model_id'] == 728 and close(i['position'], [2020.0, -2995.0, 11.0], 1.5)]
        self.assertEqual(len(copies), 1, items)
        self.assertAlmostEqual(copies[0]['rotation'][2] % 360, 225.0, places=1)
        self.assertTrue(close(self.find(728, [2020.0, -3000.0, 10.0])['position'], [2020.0, -3000.0, 10.0]))

    def test_model_filter_explosive(self):
        result = self.client.command('model_filter', 'type=explosive')
        ids = [m['model'] for m in result['models']]
        self.assertIn(1225, ids, result)
        # Moves when hit, but doesn't blow up.
        self.assertNotIn(754, ids, result)
        meta = self.client.command('model_meta', '754')['models'][0]
        self.assertIn('dynamic', meta['flags'])
        self.assertNotIn('explosive', meta['flags'])


if __name__ == '__main__':
    unittest.main()
