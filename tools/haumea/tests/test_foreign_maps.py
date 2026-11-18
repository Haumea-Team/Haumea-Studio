# SPDX-License-Identifier: GPL-2.0-or-later
"""Open an MTA:SA .map and an IPL text file in the real editor and check the Pawn.

Needs a built editor, a GTA San Andreas directory with SA-MP installed and a
display: set HAUMEA_GAME_DIR (and HAUMEA_BINARY if it is not in bin/).
The editor runs with a private XDG_CONFIG_HOME so the user's settings stay put.
"""
from pathlib import Path
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from haumea_test_env import editor_env, run_editor  # noqa: E402

ROOT = Path(__file__).resolve().parents[3]
FIXTURES = Path(__file__).resolve().parent / 'fixtures'
GAME_DIR = os.environ.get('HAUMEA_GAME_DIR')
BINARY = os.environ.get('HAUMEA_BINARY') or next(iter(sorted(
    glob.glob(str(ROOT / 'bin' / '*' / 'Release' / 'haumea')))), None)
HAVE_DISPLAY = bool(os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY'))


@unittest.skipUnless(GAME_DIR and BINARY and HAVE_DISPLAY,
                     'set HAUMEA_GAME_DIR and build the editor to run')
class ForeignMapTests(unittest.TestCase):
    def roundtrip(self, name):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp, name)
            shutil.copy(FIXTURES / name, source)
            target = Path(tmp, 'out.pwn')
            env = editor_env(tmp)
            result = run_editor([BINARY, '--game', GAME_DIR, '--pawn-roundtrip', str(source), str(target)],
                                    cwd=GAME_DIR, capture_output=True, text=True, env=env)
            self.assertIn('0 skipped on save', result.stdout, result.stdout[-2000:])
            self.assertEqual(source.read_bytes(), (FIXTURES / name).read_bytes(), 'the source must not be written')
            return target.read_text(), result.stdout

    def test_mta_map(self):
        out, log = self.roundtrip('mta_sample.map')
        # MTA and SA-MP turn objects the same way: the angles carry over.
        self.assertIn('CreateObject(1634, 2490.0, -1670.0, 13.3, 10.0, 20.0, 90.0);', out)
        self.assertIn('CreateObject(1271, 2494.0, -1666.0, 13.8, 0.0, 0.0, 45.0);', out)
        # Vehicles: heading, palette or RGB colours, paintjob, parts and plate.
        self.assertRegex(out, r'new (\w+) = CreateVehicle\(560, 2488\.0, -1680\.0, 13\.1, 90\.0, 37, 1, -1\);')
        self.assertIn('ChangeVehiclePaintjob(', out)
        self.assertIn(', 1010);', out)
        self.assertIn('SetVehicleNumberPlate(', out)
        self.assertRegex(out, r'CreateVehicle\(522, [^\n]*180\.0, \d+, \d+, -1\);')
        self.assertIn('CreateActor(280, 2486.0, -1672.0, 13.5, -90.0);', out)
        self.assertIn('CreatePickup(1240, 2,', out)
        self.assertIn('CreatePickup(1242, 2,', out)
        # removeWorldObject removes the model and its LOD.
        self.assertIn('RemoveBuildingForPlayer(playerid, 3563, 2063.1328, -1731.7812, 15.273438, 0.25);', out)
        self.assertIn('RemoveBuildingForPlayer(playerid, 3555, 2063.1328, -1731.7812, 15.273438, 0.25);', out)
        # What SA-MP can't show is reported.
        for dropped in ('object scale', 'object alpha', 'double-sided', 'marker', 'repair',
                        'vehicle tilt', '<spawnpoint>'):
            self.assertIn(dropped, log)

    def test_ipl_text(self):
        out, log = self.roundtrip('ipl_sample.ipl')
        self.assertIn('#include <streamer>', out)
        self.assertIn('CreateDynamicObject(14720, 216.55469, 1292.2188, 1081.1172, 0.0, 0.0, 0.0, -1, 1, -1);', out)
        # The IPL's stored quaternion (z = -0.707) is a quarter turn.
        self.assertIn('CreateDynamicObject(2524, 219.67188, 1072.9922, 1083.1641, 0.0, 0.0, 90.0, -1, 6, -1);', out)
        # Interior flags (257 = 1 | 256) are dropped; LOD instances are left out.
        self.assertIn('CreateDynamicObject(1502, 219.07031, 1287.0312, 1081.1328, 0.0, 0.0, 90.0, -1, 1, -1);', out)
        self.assertEqual(out.count('CreateDynamicObject(1502,'), 1)
        self.assertIn('LOD instance', log)


if __name__ == '__main__':
    unittest.main()
