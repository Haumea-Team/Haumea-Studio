"""Trains stand where the SA-MP client puts them, with its carriages.

On the synthetic game's tracks (fixture/make_game.py: tracks.dat a 400 x 300 m
loop with a node every 10 m, tracks3.dat a small square, and a decoy
tracks2.dat that SA-MP leaves out), one editor opens a map of trains over the
agent socket and reads back each engine's "train": the cars the client makes
and where, as CTrain::CreateMissionTrain (0x6F7550, types 5, 3 and 9 from the
table at 0x8D44F8), FindClosestTrackNode (0x6F6BD0),
FindPositionOnTrackFromCoors (0x6F6CC0) and CTrain::ProcessControl place them
(SA-MP's ConstructVehicleSlot, 0x100b83d0: clockwise = angle > 180).
The fixture's cars are boxes: engines 18 m long, Streak coaches 20 m, flat
freights 16 m, the tram 14 m, with handling lower limits -0.1 (Streak and its
coaches), 0.25 (Freight), 0.0 (flat) and -1.0 (tram).
Then Map check and the export.
"""
from pathlib import Path
import glob
import json
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
	new streak = AddStaticVehicle(538, 2000.0, -3098.0, 10.0, 0.0, 1, 1);
	new freight = AddStaticVehicle(537, 2000.0, -3098.0, 10.0, 270.0, 1, 1);
	new tram = AddStaticVehicleEx(449, -2000.0, 52.0, 30.0, 0.0, 1, 1, 100);
	new decoy = AddStaticVehicle(538, 2000.0, -2952.0, 11.0, 0.0, 1, 1);
	new backwards = AddStaticVehicle(537, 1900.0, -3098.0, 10.0, -90.0, 1, 1);
	new dynamic = CreateVehicle(538, 1900.0, -2802.0, 12.0, 0.0, 1, 1, 100);
	new coach = AddStaticVehicle(570, 2100.0, -2900.0, 10.0, 0.0, 1, 1);
	return 1;
}
'''


def close(a, b, tolerance=0.02):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


@unittest.skipUnless(BINARY and XVFB and sys.platform.startswith('linux'), 'needs the editor and xvfb-run on Linux')
class TrainTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='htr-', dir='/tmp'))
        game = cls.tmp / 'game'
        # Always the synthetic game: its tracks and car sizes are what the numbers expect.
        subprocess.run([sys.executable, str(HERE / 'fixture' / 'make_game.py'), str(game)],
                       check=True, capture_output=True)
        (cls.tmp / 'trains.pwn').write_text(MAP)
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
        cls.client.command('map_open', str(cls.tmp / 'trains.pwn'))
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

    def vehicle(self, model, x, y):
        for item in self.items:
            if item['model_id'] == model and close(item['position'][:2], [x, y], 0.01):
                return item
        self.fail('no %d at %s, %s in %s' % (model, x, y, json.dumps(self.items)[:2000]))

    def test_tracks_loaded(self):
        self.assertIn('Trains: tracks.dat 140 nodes, tracks3.dat 40 nodes', self.output())

    def test_brown_streak_has_three_coaches_behind(self):
        # Not clockwise (0): the engine faces down the track (-x here) and the
        # coaches follow on, each the previous car's length further.
        train = self.vehicle(538, 2000, -3098)['train']
        self.assertEqual(train['vehicle_ids'], 4)
        self.assertFalse(train['clockwise'])
        cars = train['cars']
        self.assertEqual([c['model'] for c in cars], [538, 570, 570, 570])
        # 2 m off the track: the segment ending at x 2000 takes it, centred there.
        z = 10 - 0.1 + 2.0
        for car, x in zip(cars, (2000, 2019, 2039, 2059)):
            self.assertTrue(close(car['position'], [x, -3100, z]), car)
            self.assertTrue(close(car['forward'], [-1, 0, 0]), car)
        self.assertAlmostEqual(train['off_track'], 2.0, delta=0.02)

    def test_freight_clockwise_runs_the_other_way(self):
        # Over 180: clockwise, facing +x, each flat freight its own length back.
        train = self.vehicle(537, 2000, -3098)['train']
        self.assertTrue(train['clockwise'])
        cars = train['cars']
        self.assertEqual([c['model'] for c in cars], [537, 569, 569, 569])
        self.assertTrue(close(cars[0]['position'], [2000, -3100, 10 + 0.25 + 2.0]), cars[0])
        for car, x in zip(cars[1:], (1983, 1967, 1951)):
            self.assertTrue(close(car['position'], [x, -3100, 10 + 0.0 + 1.0]), car)
            self.assertTrue(close(car['forward'], [1, 0, 0]), car)

    def test_tram_is_alone_on_its_own_tracks(self):
        train = self.vehicle(449, -2000, 52)['train']
        self.assertEqual(train['vehicle_ids'], 1)
        self.assertEqual([c['model'] for c in train['cars']], [449])
        self.assertTrue(close(train['cars'][0]['position'], [-2000, 50, 30 - 1.0 + 1.75]), train)

    def test_tracks2_is_left_out(self):
        # 2 m from tracks2.dat, which SA-MP doesn't use: it goes to the
        # nearest node of tracks.dat instead (x 2000 on the south side), 148 m away.
        train = self.vehicle(538, 2000, -2952)['train']
        self.assertTrue(close(train['cars'][0]['position'][:2], [2009, -3100]), train)
        self.assertGreater(train['off_track'], 140)

    def test_angle_keeps_the_direction(self):
        # -90 is not over 180 in game, so it stays not clockwise.
        self.assertFalse(self.vehicle(537, 1900, -3098)['train']['clockwise'])

    def test_map_check(self):
        items = self.client.command('map_check')['items']
        texts = [i['text'] for i in items if i.get('category') == 'Trains']
        self.assertTrue(any('only made with AddStaticVehicle' in t for t in texts), texts)
        self.assertTrue(any('m off the track' in t and '148' in t for t in texts), texts)
        self.assertTrue(any('bad train carriages' in t for t in texts), texts)
        # The others are within the 3 m the client moves a train onto its track.
        self.assertEqual(sum('m off the track' in t for t in texts), 1, texts)

    def test_export_writes_the_engine_only(self):
        out = self.tmp / 'saved.pwn'
        self.client.command('save', str(out))
        text = out.read_text()
        self.assertEqual(text.count('(538,'), 3, text)
        self.assertEqual(text.count('(537,'), 2, text)
        self.assertNotIn('(569,', text)
        self.assertEqual(text.count('(570,'), 1, text)	# the lone coach, as written
        self.assertIn('AddStaticVehicle(537, 2000.0, -3098.0, 10.0, 270.0', text)
        self.assertIn('AddStaticVehicle(537, 1900.0, -3098.0, 10.0, 90.0', text)


if __name__ == '__main__':
    unittest.main()
