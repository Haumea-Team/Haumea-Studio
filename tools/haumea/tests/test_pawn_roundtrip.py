# SPDX-License-Identifier: GPL-2.0-or-later
"""Open a mixed Pawn map in the real editor, save it twice, and check the result.

Needs a built editor, a GTA San Andreas directory with SA-MP installed and a
display: set HAUMEA_GAME_DIR (and HAUMEA_BINARY if it is not in bin/).
"""
from pathlib import Path
import glob
import os
import re
import shutil
import subprocess
import tempfile
import unittest

import sys

# The helper sits next to the tests: found as a script and with python -m unittest.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from haumea_test_env import editor_env, game_slot, is_real_game, run_editor  # noqa: E402

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = Path(__file__).resolve().parent / 'fixtures' / 'pawn_roundtrip.pwn'
GAME_DIR = os.environ.get('HAUMEA_GAME_DIR')
BINARY = os.environ.get('HAUMEA_BINARY') or next(iter(sorted(
    glob.glob(str(ROOT / 'bin' / '*' / 'Release' / 'haumea')))), None)
HAVE_DISPLAY = bool(os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY'))


@unittest.skipUnless(GAME_DIR and BINARY and HAVE_DISPLAY,
                     'set HAUMEA_GAME_DIR and build the editor to run')
class PawnRoundtripTests(unittest.TestCase):
    def roundtrip(self, source, target):
        # Haumea runs from anywhere: the game folder comes from --game.
        result = run_editor([BINARY, '--game', GAME_DIR, '--pawn-roundtrip', str(source), str(target)],
                                cwd=target.parent, env=editor_env(target.parent),
                                capture_output=True, text=True)
        self.assertIn('pawn-roundtrip:', result.stdout, result.stdout[-2000:])
        return target.read_text()

    def test_area_removal_is_kept_as_written(self):
        # Model -1 removes every building in the sphere (the hospital and its
        # LOD here) and is saved back as that one line, not one per building;
        # a model removal still removes only that model.
        with tempfile.TemporaryDirectory() as tmp:
            out = self.roundtrip(Path(__file__).resolve().parent / 'pawn' / 'maps' / 'hv_removals.pwn', Path(tmp, 'hv_removals.pwn'))
        self.assertIn('RemoveBuildingForPlayer(playerid, -1, 2063.0, -1731.0, 15.0, 5.0);', out)
        self.assertIn('RemoveBuildingForPlayer(playerid, 4206, 1479.5547, -1639.6094, 13.648438, 0.25);', out)
        self.assertNotIn('RemoveBuildingForPlayer(playerid, 3563,', out)
        self.assertNotIn('RemoveBuildingForPlayer(playerid, 3555,', out)

    @unittest.skipUnless(is_real_game(GAME_DIR), 'needs the real game (the skatepark in lae_stream0.ipl)')
    def test_area_removals_clear_the_skatepark(self):
        # A beta tester's map: two model -1 spheres over the Glen Park skatepark
        # must remove its ground 5390 (and that one's LOD 5575) and the ramp 5415.
        # Removed buildings drop out of the agent's zone listing. It never lists
        # LODs, so 5575 isn't checked here (hv_removals.pwn covers an HD and its LOD).
        import shutil
        import signal
        import time
        sys.path.insert(0, str(ROOT / 'tools' / 'agent'))
        from haumea_ipc import HaumeaClient
        skatepark = {5390, 5415}
        tmp = Path(tempfile.mkdtemp(prefix='hsk-', dir='/tmp'))	# short: AF_UNIX paths
        self.addCleanup(shutil.rmtree, tmp, True)
        (tmp / 'skatepark.pwn').write_text(
            'public OnPlayerConnect(playerid)\n{\n'
            '\tRemoveBuildingForPlayer(playerid, -1, 1952.531, -1394.023, 13.789, 26.292);\n'
            '\tRemoveBuildingForPlayer(playerid, -1, 1916.328, -1385.699, 16.973, 15.558);\n'
            '\treturn 1;\n}\n')
        socket_path = tmp / 'a.sock'
        with game_slot(GAME_DIR), open(tmp / 'out.txt', 'w') as log:
            editor = subprocess.Popen([BINARY, '--game', GAME_DIR, '--agent-socket', str(socket_path)],
                                      cwd=tmp, env=editor_env(str(tmp)), stdout=log, stderr=subprocess.STDOUT,
                                      start_new_session=True)
            try:
                client = HaumeaClient(str(socket_path))
                end = time.time() + 300
                while time.time() < end and editor.poll() is None:
                    try:
                        if client.command('ping').get('result') == 'pong':
                            break
                    except Exception:
                        time.sleep(1)
                else:
                    self.fail('the editor did not answer: ' + (tmp / 'out.txt').read_text()[-2000:])

                def models():
                    found, offset = set(), 0
                    while offset is not None:
                        page = client.command('inspect_zone_page', 1935, -1390, 45, offset, 256)
                        found |= {item['model_id'] for item in page['items']}
                        offset = page['page']['next_offset']
                    return found
                self.assertLessEqual(skatepark, models(), 'the skatepark is not where the test expects it')
                client.command('map_open', str(tmp / 'skatepark.pwn'), 'discard')
                self.assertFalse(skatepark & models(), 'still there: %s' % sorted(skatepark & models()))
            finally:
                os.killpg(editor.pid, signal.SIGKILL)
                editor.wait()

    def test_streamer_interiors_come_back_as_written(self):
        # The streamer's interiorid 0 means only interior 0, -1 every interior: both stay as they were.
        source = (
            '#include <a_samp>\n#include <streamer>\n\nstock Interiors()\n{\n'
            '\tCreateDynamicObject(1271, 1600.0, -1900.0, 14.0, 0.0, 0.0, 0.0, -1, 0, -1, 300.0, 300.0);\n'
            '\tCreateDynamicObject(1271, 1602.0, -1900.0, 14.0, 0.0, 0.0, 0.0, -1, -1, -1, 300.0, 300.0);\n'
            '\tCreateDynamicObject(1271, 1604.0, -1900.0, 14.0, 0.0, 0.0, 0.0, -1, 5, -1, 300.0, 300.0);\n'
            '\tCreateDynamicPickup(1240, 2, 1606.0, -1900.0, 14.0, -1, 0);\n'
            '\tnew const zone = CreateDynamicCube(1590.0, -1910.0, 10.0, 1610.0, -1890.0, 20.0, -1, 0);\n'
            '\tCreateDynamicObject(1271, 1608.0, -1900.0, 14.0, 0.0, 0.0, 0.0, -1, 0, -1, 300.0, 300.0, zone);\n'
            '}\n')
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, 'in.pwn').write_text(source)
            first = self.roundtrip(Path(tmp, 'in.pwn'), Path(tmp, 'map.pwn'))
            Path(tmp, 'copy.pwn').write_text(first)
            second = self.roundtrip(Path(tmp, 'copy.pwn'), Path(tmp, 'map.pwn'))
        self.assertEqual(first, second, 'saving an opened map must not change it')
        objects = {x: (world, interior) for x, world, interior in re.findall(
            r'CreateDynamicObject\(1271, (16\d\d)\.0, [^\n]*?0\.0, (-?\d+), (-?\d+), -1,', first)}
        self.assertEqual(objects, {'1600': ('-1', '0'), '1602': ('-1', '-1'), '1604': ('-1', '5'),
                                   '1608': ('-1', '0')}, first)
        self.assertRegex(first, r'CreateDynamicPickup\(1240, 2, 1606\.0, -1900\.0, 14\.0, -1, 0, -1,')
        self.assertRegex(first, r'new const zone = CreateDynamicCube\([^\n]*, -1, 0\);')

    def test_open_save_is_faithful_and_stable(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = self.roundtrip(FIXTURE, Path(tmp, 'map.pwn'))
            copy = Path(tmp, 'copy.pwn')
            copy.write_text(first)
            shutil.copyfile(Path(tmp, 'map.pwn.xml'), Path(tmp, 'copy.pwn.xml'))
            second = self.roundtrip(copy, Path(tmp, 'map.pwn'))
            self.assertEqual(first, second, 'saving an opened map must not change it')

        # RemoveBuildingForPlayer removes exactly the listed model (no LOD cascade).
        self.assertEqual(len(re.findall(r'RemoveBuildingForPlayer\(playerid, 3563,', first)), 1)
        self.assertNotIn('RemoveBuildingForPlayer(playerid, 3555,', first)
        # Texture Studio's reused tmpobjid: each object keeps its own material.
        self.assertRegex(first, r'1600\.0[^\n]*\n[^\n]*"white", 0xFFFF0000')
        self.assertRegex(first, r'1604\.0[^\n]*\n[^\n]*"white", 0xFF00FF00')
        # Variables as optional arguments keep the object (defaults are used).
        self.assertRegex(first, r'CreateDynamicObject\(3578, 1620\.0[^\n]*-1, -1, -1, 300\.0, 300\.0\)')
        self.assertIn('"a51", "concretewall22_256", 0x00000000', first)
        # CreateDynamicObjectEx stream/draw distances.
        self.assertRegex(first, r'1608\.0[^\n]*, 150\.0, 120\.0\);')
        # Streamer world/interior/distances, names, groups and notes.
        self.assertIn('new sign = CreateDynamicObject(19353, 1612.0, -1890.0, 14.0, '
                      '10.0, 20.0, 30.0, 5, 3, -1, 200.0, 150.0); '
                      '// wall001', first)
        # Streamer material text puts the index before the text.
        self.assertIn('SetDynamicObjectMaterialText(sign, 1, "OPEN"', first)
        # The attached crate sits on its parent and the attachment is kept.
        self.assertIn('new crate = CreateDynamicObject(1271, 1616.0, -1890.0, 14.3, '
                      '15.0, 25.0, 80.0', first)
        self.assertIn('AttachDynamicObjectToObject(crate, base, 0.0, 0.0, 0.8, '
                      '15.0, 25.0, 35.0, true);', first)
        # Vehicle paintjob, parts (one per slot, only fitting ones) and plate.
        self.assertRegex(first, r'new (\w+) = AddStaticVehicleEx\(562, 1600\.0, -1900\.0, '
                         r'13\.2, 90\.0, 140, 201, 300\);[^\n]*\n'
                         r'\tChangeVehiclePaintjob\(\1, 1\);\n'
                         r'\tAddVehicleComponent\(\1, 1036\);\n'
                         r'\tAddVehicleComponent\(\1, 1009\);\n'
                         r'\tAddVehicleComponent\(\1, 1080\);\n'
                         r'\tAddVehicleComponent\(\1, 1171\);\n'
                         r'\tSetVehicleNumberPlate\(\1, "NEU \\"FOX\\""\);\n')
        self.assertNotIn('1010)', first)
        self.assertNotIn('1100)', first)
        # Sound places: the stream goes to _ShowForPlayer, the game sound to this
        # map's own _AddSound helper with a streamer sphere behind it.
        self.assertIn('PlayAudioStreamForPlayer(playerid, "http://example.com/radio.pls", 1630.0, '
                      '-1890.0, 13.5, 60.0, true); // sound', first)
        self.assertRegex(first, r'\tHaumea_\w+_AddSound\(1142, 1634\.0, -1890\.0, 13\.5, '
                         r'40\.0, false, 3, 1\); // sound')
        self.assertIn('CreateDynamicSphere(x, y, z, radius, worldid, interiorid);', first)
        self.assertNotIn('Haumea_old_', first)
        # A hand-written name nothing else uses keeps its name and is marked
        # used, so pawncc does not warn (204).
        self.assertRegex(first, r'\tnew lamp = CreateDynamicObject\(1215, [^\n]*\n\t#pragma unused lamp\n')
        self.assertRegex(first, r'\tnew bike = AddStaticVehicleEx\(468, [^\n]*\n\t#pragma unused bike\n')
        self.assertEqual(first.count('#pragma unused'), 2)
        # Checkpoints (streamer only): a race is chained by its next points,
        # written in order; the XML retains its name.
        self.assertIn('CreateDynamicCP(1644.0, -1890.0, 13.5, 3.0, -1, -1, -1, '
                      'STREAMER_CP_SD); // checkpoint', first)
        self.assertRegex(first, r'CreateDynamicRaceCP\(0, 1650\.0, -1890\.0, 13\.5, '
                         r'1660\.0, -1890\.0, 13\.5, 8\.0, [^\n]*// checkpoint\n'
                         r'\tCreateDynamicRaceCP\(1, 1660\.0, -1890\.0, 13\.5, '
                         r'0\.0, 0\.0, 0\.0, 8\.0, [^\n]*// checkpoint\n')


if __name__ == '__main__':
    unittest.main()
