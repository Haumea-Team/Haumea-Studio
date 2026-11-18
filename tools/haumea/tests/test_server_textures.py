# SPDX-License-Identifier: GPL-2.0-or-later
"""Server artwork configuration and server-only TextDraw sprite sources."""
from pathlib import Path
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
from haumea_test_env import editor_env, run_editor
from test_ui_scripts import BINARY, XVFB, start_xvfb


@unittest.skipUnless(shutil.which('c++'), 'needs a C++ compiler')
class ArtworkPathTests(unittest.TestCase):
    def test_server_configuration(self):
        with tempfile.TemporaryDirectory(prefix='haumea-artwork-path-') as tmp:
            root = Path(tmp)
            cpp, exe = root / 'path.cpp', root / 'path'
            cpp.write_text('#include "serverartwork.h"\n#include <iostream>\n'
                           'int main(int argc,char**argv){std::cout<<ServerArtworkDirectory(argv[1]);}\n')
            subprocess.run(['c++', '-std=c++14', '-I', str(ROOT / 'tools/haumea'), str(cpp), '-o', str(exe)], check=True)
            cases = [
                ('default', None, None, 'models'),
                ('samp', None, 'artpath custom/graphics\n', 'custom/graphics'),
                ('quoted', None, 'artpath "server art"\r\n', 'server art'),
                ('openmp', {'artwork': {'models_path': 'custom/art'}}, None, 'custom/art'),
                ('unicode', {'artwork': {'models_path': '模型/界面'}}, None, '模型/界面'),
                ('precedence', {'artwork': {'models_path': 'current'}}, 'artpath old\n', 'current'),
                ('json_default', {}, 'artpath old\n', 'models'),
                ('flat', {'artwork.models_path': 'flat/art'}, None, 'flat/art'),
                ('absolute', {'artwork': {'models_path': str(root / 'shared art')}}, None, str(root / 'shared art')),
            ]
            for name, config, legacy, expected in cases:
                with self.subTest(name=name):
                    server = root / name
                    server.mkdir()
                    if config is not None: (server / 'config.json').write_text(json.dumps(config))
                    if legacy is not None: (server / 'server.cfg').write_text(legacy)
                    got = subprocess.check_output([exe, server], text=True)
                    self.assertEqual(got, str(server / expected))


def img_file(img, wanted):
    with img.open('rb') as source:
        assert source.read(4) == b'VER2'
        count, = struct.unpack('<I', source.read(4))
        for _ in range(count):
            offset, size, size2, name = struct.unpack('<IHH24s', source.read(32))
            if name.split(b'\0')[0].decode().lower() == wanted.lower():
                source.seek(offset * 2048)
                return source.read((size or size2) * 2048)
    raise AssertionError('fixture has no ' + wanted)


@unittest.skipUnless(BINARY and XVFB, 'needs the editor and Xvfb')
class ServerSpriteTests(unittest.TestCase):
    def test_server_txd_without_model_import(self):
        with tempfile.TemporaryDirectory(prefix='haumea-server-sprites-') as tmp:
            root, game = Path(tmp), Path(tmp) / 'game'
            subprocess.run([sys.executable, HERE / 'fixture/make_game.py', game], check=True, capture_output=True)
            texture = img_file(game / 'models/gta3.img', 'gunbox.txd')
            model = img_file(game / 'models/gta3.img', 'gunbox.dff')
            for kind, registered in [('default', False), ('openmp', False), ('samp', True)]:
                with self.subTest(config=kind, registered=registered):
                    work = root / kind
                    server = work / 'server'
                    artwork = server / ('models' if kind == 'default' else 'custom art')
                    (artwork / 'txd').mkdir(parents=True)
                    (server / 'scriptfiles').mkdir()
                    (artwork / 'txd/UI.TXD').write_bytes(texture)
                    if kind == 'openmp':
                        (server / 'config.json').write_text(json.dumps({'artwork': {'models_path': 'custom art'}}))
                    if kind == 'samp': (server / 'server.cfg').write_text('artpath "custom art"\n')
                    if registered:
                        (artwork / 'object.dff').write_bytes(model)
                        (artwork / 'artconfig.txt').write_text('AddSimpleModel(-1, 1271, -2018, "object.dff", "txd/UI.TXD");\n')
                    env = editor_env(work, virtual_display=True)
                    config = Path(env['XDG_CONFIG_HOME'], 'haumea')
                    config.mkdir(parents=True, exist_ok=True)
                    (config / 'savesettings.txt').write_text('settings_version 8\nlanguage en\nshow_textdraws_hint 0\nshow_code_hint 0\n')
                    output = server / 'scriptfiles/interface.pwn'
                    env['TD_OUTPUT'] = str(output)
                    script = work / 'server.uis'
                    script.write_text('''wait 30
assert objects == 0
click_item '*' '##editor'
wait 2
click_item '*' 'TextDraws'
wait 5
click_item 'TextDraws' 'Sprite'
wait 5
scroll_until 'TextDraws' 'Browse sprites...'
click_item 'TextDraws' 'Browse sprites...'
wait 3
click_item '*' 'Sprite TXD'
wait 3
assert item '*' 'txd/UI.TXD'
click_item '*' 'txd/UI.TXD'
wait 3
''' + ('' if registered else '''scroll_until '*' 'Model ID'
set_item '*' 'Model ID' '-2018' tab
wait 3
''') + '''scroll_until '*' '##s'
click_item '*' '##s'
wait 5
assert textdraw.0.texture_loaded == 1
assert textdraw.0.server_txd 'txd/UI.TXD'
move 900 400
key Ctrl+Z
wait 5
assert textdraw.0.server_txd ''
key Ctrl+Y
wait 5
assert textdraw.0.texture_loaded == 1
assert textdraw.0.server_txd 'txd/UI.TXD'
click_item '*' '##s' right
wait 3
click_item '*' 'Add to favourites'
wait 3
click_item '*' 'Sprite TXD'
wait 3
click_item '*' 'Favourites'
wait 3
scroll_until '*' '##s'
click_item '*' '##s'
wait 3
assert textdraw.0.texture_loaded == 1
assert textdraw.0.server_txd 'txd/UI.TXD'
close_popup
wait 3
shortcut save_map_as
wait 5
set_item 'Save TextDraws As' 'Path' '$TD_OUTPUT'
wait 5
assert file_contains '$TD_OUTPUT' '"mdl-2018:'
assert file_contains '$TD_OUTPUT.xml' 'serverTxd="txd/UI.TXD"'
move 550 200
wheel 100
wait 8
click_item 'TextDraws' 'td_new'
wait 5
assert textdraws == 0
click_item 'TextDraws' 'td_open'
wait 5
set_item 'tdfile' 'Path' '$TD_OUTPUT'
wait 5
assert textdraw.0.texture_loaded == 1
assert textdraw.0.server_txd 'txd/UI.TXD'
assert objects == 0
exit
''')
                    xvfb, env['DISPLAY'] = start_xvfb()
                    env.pop('XAUTHORITY', None)
                    try:
                        result = run_editor([BINARY, '--game', game, '--workspace', server, '--ui-script', script],
                                            cwd=work, env=env, timeout=180)
                    finally:
                        xvfb.terminate()
                        xvfb.wait(10)
                    (work / 'run.log').write_text(result.stdout)
                    log = '\n'.join(line for line in result.stdout.splitlines() if '[ui-script]' in line)
                    self.assertEqual(result.returncode, 0, log)
                    self.assertIn('"mdl-2018:', output.read_text())
                    self.assertEqual((artwork / 'object.dff').exists(), registered)


if __name__ == '__main__':
    unittest.main()
