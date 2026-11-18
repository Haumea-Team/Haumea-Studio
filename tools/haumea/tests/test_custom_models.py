# SPDX-License-Identifier: GPL-2.0-or-later
"""Open a map that uses custom models (open.mp, SA-MP 0.3.DL) in the real editor.

The test builds a server folder with the models listed (artconfig.txt, or
Haumea's own haumea_models.txt) from models in the
game's own gta3.img, so no game files are kept in the repository.
Needs a built editor, a GTA San Andreas directory with SA-MP installed and a
display: set HAUMEA_GAME_DIR (and HAUMEA_BINARY if it is not in bin/).
"""
from pathlib import Path
import glob
import os
import struct
import subprocess
import tempfile
import unittest

import sys

# The helper sits next to the tests: found as a script and with python -m unittest.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from haumea_test_env import editor_env, run_editor  # noqa: E402

ROOT = Path(__file__).resolve().parents[3]
GAME_DIR = os.environ.get('HAUMEA_GAME_DIR')
BINARY = os.environ.get('HAUMEA_BINARY') or next(iter(sorted(
    glob.glob(str(ROOT / 'bin' / '*' / 'Release' / 'haumea')))), None)
HAVE_DISPLAY = bool(os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY'))

MAP = '''#include <a_samp>
stock CustomTest()
{
\tnew wreck = CreateObject(-2000, 2490.0, -1670.0, 13.5, 0.0, 0.0, 90.0);
\tSetObjectMaterial(wreck, 1, -2001, "mybarrel", "redmetal", 0);
\tCreateObject(-2001, 2494.0, -1666.0, 13.0, 0.0, 0.0, 0.0);
\tCreateActor(20001, 2488.0, -1672.0, 13.4, 90.0);
}
'''

ARTCONFIG = '''AddSimpleModel(-1, 3594, -2000, "wreckcar.dff", "wreckcar.txd");
AddSimpleModelTimed(-1, 1225, -2001, "sub/mybarrel.dff", "sub/mybarrel.txd", 6, 22)
AddCharModel(7, 20001, "myskin.dff", "myskin.txd");
'''


def img_entries(path):
    with open(path, 'rb') as f:
        assert f.read(4) == b'VER2'
        count, = struct.unpack('<I', f.read(4))
        entries = {}
        for _ in range(count):
            offset, size, size2, name = struct.unpack('<IHH24s', f.read(32))
            entries[name.split(b'\0')[0].decode().lower()] = (offset * 2048, (size or size2) * 2048)
        return entries


@unittest.skipUnless(GAME_DIR and BINARY and HAVE_DISPLAY,
                     'set HAUMEA_GAME_DIR and build the editor to run')
class CustomModelTests(unittest.TestCase):
    def round_trip(self, list_name):
        """The map through the editor, with the models listed in models/<list_name>."""
        img = Path(GAME_DIR, 'models', 'gta3.img')
        entries = img_entries(img)
        with tempfile.TemporaryDirectory() as tmp:
            server = Path(tmp, 'server')
            (server / 'models' / 'sub').mkdir(parents=True)
            (server / 'scriptfiles').mkdir()
            copies = {'la_fuckcar1.dff': 'WreckCar.dff', 'lafuckar.txd': 'wreckcar.txd',
                      'barrel4.dff': 'sub/mybarrel.dff', 'dynbarrels.txd': 'sub/mybarrel.txd',
                      'male01.dff': 'myskin.dff', 'male01.txd': 'myskin.txd'}
            with open(img, 'rb') as f:
                for source, target in copies.items():
                    offset, size = entries[source]
                    f.seek(offset)
                    (server / 'models' / target).write_bytes(f.read(size))
            (server / 'models' / list_name).write_text(ARTCONFIG)
            source = server / 'scriptfiles' / 'map.pwn'
            source.write_text(MAP)
            target = Path(tmp, 'out.pwn')
            result = run_editor([BINARY, '--workspace', str(server), '--game', GAME_DIR,
                                     '--pawn-roundtrip', str(source), str(target)],
                                    cwd=tmp, env=editor_env(tmp), capture_output=True, text=True)
            self.assertIn('0 skipped on save', result.stdout, result.stdout[-2000:])
            return target.read_text()

    def test_custom_models_round_trip(self):
        # A SA-MP style artconfig.txt registers them: the map doesn't again.
        out = self.round_trip('artconfig.txt')
        self.assertIn('players need the SA-MP 0.3.DL client', out)
        self.assertNotIn('AddSimpleModel', out)
        # Negative IDs stay negative, also as a material's texture source;
        # the file name differs in case from artconfig.txt.
        self.assertIn('new wreck = CreateObject(-2000, 2490.0', out)
        self.assertIn('SetObjectMaterial(wreck, 1, -2001, "mybarrel", "redmetal"', out)
        self.assertIn('CreateObject(-2001, 2494.0', out)
        # AddCharModel skins work as actor skins.
        self.assertIn('CreateActor(20001, 2488.0', out)

    def test_map_registers_its_models(self):
        # Haumea's own list (servers don't read it): the map registers the
        # models it uses in its own code, as open.mp scripts do, before using them.
        out = self.round_trip('haumea_models.txt')
        self.assertIn('AddSimpleModel(-1, 3594, -2000, "WreckCar.dff", "wreckcar.txd");', out)
        self.assertIn('AddSimpleModelTimed(-1, 1225, -2001, "sub/mybarrel.dff", "sub/mybarrel.txd", 6, 22);', out)
        self.assertIn('AddCharModel(7, 20001, "myskin.dff", "myskin.txd");', out)
        self.assertLess(out.index('AddSimpleModel(-1, 3594'), out.index('CreateObject(-2000'))


if __name__ == '__main__':
    unittest.main()
