# SPDX-License-Identifier: GPL-2.0-or-later
"""Namaka reaches the rest of the editor: the commands for its windows and menus.

One editor opens a small map over the agent socket and is driven through the
bridge commands the MCP tools call (world, clipboard, history, layout_path,
textdraw_tool, nodes, interiors, export, pawn_code...), each checked for what
it changed and, for edits, that rolling back the edit session puts it back.
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
	CreateObject(1225, 2000.0, -3000.0, 10.0, 0.0, 0.0, 0.0);
	CreateObject(1225, 2000.0, -3000.0, 10.0, 0.0, 0.0, 0.0);
	CreateObject(1225, 2010.0, -3000.0, 10.0, 0.0, 0.0, 0.0);
	CreateObject(1225, 2020.0, -3006.0, 10.0, 0.0, 0.0, 0.0);
	CreateVehicle(411, 2030.0, -3000.0, 10.0, 0.0, -1, -1, 100);
	return 1;
}

public OnPlayerConnect(playerid)
{
	RemoveBuildingForPlayer(playerid, 4206, 1479.5547, -1639.6094, 13.648438, 0.25);
	return 1;
}
'''


@unittest.skipUnless(BINARY and XVFB and sys.platform.startswith('linux'), 'needs the editor and xvfb-run on Linux')
class AgentEditorToolsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='het-', dir='/tmp'))
        game = cls.tmp / 'game'
        subprocess.run([sys.executable, str(HERE / 'fixture' / 'make_game.py'), str(game)],
                       check=True, capture_output=True)
        (cls.tmp / 'tools.pwn').write_text(MAP)
        cls.socket = cls.tmp / 'a.sock'
        env = editor_env(str(cls.tmp), virtual_display=True)
        env['HAUMEA_TEST_XVFB'] = '1'
        cls.log = open(cls.tmp / 'out.txt', 'w')
        cls.editor = subprocess.Popen(
            [XVFB, '-a', '-s', '-screen 0 1280x800x24', BINARY, '--game', str(game),
             '--agent-socket', str(cls.socket)],
            cwd=cls.tmp, env=env, stdout=cls.log, stderr=subprocess.STDOUT, start_new_session=True)
        from haumea_ipc import HaumeaClient, HaumeaError
        cls.error = HaumeaError
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
        cls.client.command('map_open', str(cls.tmp / 'tools.pwn'))

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

    def tearDown(self):
        if self.client.command('session_status').get('active'):
            self.client.command('session_rollback')

    def cmd(self, *args):
        return self.client.command(*args)

    def items(self, model=None):
        found = self.cmd('list')['instances']
        return [i for i in found if model is None or i['model_id'] == model]

    def barrel(self, x):
        for item in self.items(1225):
            if abs(item['position'][0] - x) < 0.01:
                return item
        self.fail('no barrel at x=%s' % x)

    def test_vehicle_static(self):
        car = self.items(411)[0]
        self.assertFalse(car['properties']['static'])
        self.cmd('session_begin', 'static')
        changed = self.cmd('properties', car['instance_id'], 'static=true')
        self.assertTrue(changed['object']['properties']['static'], changed)
        with self.assertRaises(self.error):
            self.cmd('properties', self.barrel(2010.0)['instance_id'], 'static=true')
        self.cmd('session_rollback')
        self.assertFalse(self.items(411)[0]['properties']['static'])

    def test_world_buildings(self):
        removed = self.cmd('world', 'removed')
        self.assertEqual([r['model_id'] for r in removed['removed']], [4206], removed)
        pool = removed['removed'][0]['instance_id']
        where = self.cmd('world', 'model_instances', 'model=3555')
        self.assertEqual(where['total'], 1, where)
        house = where['instances'][0]
        self.assertTrue(house['world_building'])
        self.cmd('session_begin', 'world')
        self.assertEqual(self.cmd('world', 'restore', f'ids={pool}')['restored'], 1)
        self.assertEqual(self.cmd('world', 'removed')['total'], 0)
        # The Changes for Pawn Export window no longer lists it as removed.
        self.assertNotIn(pool, [c['instance_id'] for c in self.cmd('history', 'changes')['changed']])
        self.assertEqual(self.cmd('world', 'convert', f'ids={house["instance_id"]}')['converted'], 1)
        with self.assertRaises(self.error):
            self.cmd('world', 'convert', f'ids={self.barrel(2010.0)["instance_id"]}')
        self.cmd('session_rollback')
        self.assertEqual(self.cmd('world', 'removed')['total'], 1)
        changes = {c['instance_id']: c['changes'] for c in self.cmd('history', 'changes')['changed']}
        self.assertEqual(changes.get(pool), ['deleted'], changes)

    def test_clipboard(self):
        before = len(self.items(1225))
        source = self.barrel(2010.0)['instance_id']
        self.cmd('clipboard', 'copy', f'ids={source}')
        self.cmd('session_begin', 'clipboard')
        pasted = self.cmd('clipboard', 'paste_in_place')
        self.assertEqual(len(pasted['ids']), 1, pasted)
        copy = next(i for i in self.items(1225) if i['instance_id'] == pasted['ids'][0])
        self.assertAlmostEqual(copy['position'][0], 2010.0, places=2)
        self.cmd('session_rollback')
        self.assertEqual(len(self.items(1225)), before)

    def test_duplicates(self):
        found = self.cmd('maptool', 'duplicates')
        self.assertEqual(found['copies'], 1, found)
        self.cmd('session_begin', 'duplicates')
        removed = self.cmd('maptool', 'duplicates', 'mode=remove')
        self.assertTrue(removed['removed'])
        self.assertEqual(len([i for i in self.items(1225) if abs(i['position'][0] - 2000.0) < 0.01]), 1)
        self.cmd('session_rollback')
        self.assertEqual(self.cmd('maptool', 'duplicates')['copies'], 1)

    def test_align_to_guide_and_history(self):
        a, b = self.barrel(2010.0)['instance_id'], self.barrel(2020.0)['instance_id']
        self.cmd('session_begin', 'guide')
        done = self.cmd('maptool', 'align_to_guide', f'ids={a},{b}', 'start=2010,-3010,10', 'end=2030,-3010,10',
                        'distribute=1', 'axes=xy', 'anchor=origin')
        self.assertEqual(done['changed'], 2, done)
        xs = sorted(i['position'][0] for i in self.items(1225) if i['instance_id'] in (a, b))
        ys = [i['position'][1] for i in self.items(1225) if i['instance_id'] in (a, b)]
        self.assertAlmostEqual(xs[0], 2010.0, places=2)
        self.assertAlmostEqual(xs[1], 2030.0, places=2)
        self.assertTrue(all(abs(y + 3010.0) < 0.01 for y in ys), ys)
        steps = self.cmd('history', 'list')
        self.assertGreaterEqual(steps['done'], 1, steps)
        self.cmd('history', 'goto', f'done={steps["done"] - 1}')
        self.assertAlmostEqual(self.barrel(2010.0)['position'][1], -3000.0, places=2)

    def test_coordinate_pin(self):
        # Namaka points at a spot on the user's map: a pin, then a ruler, then clear.
        self.cmd('coordinate_pin', 'clear', 'all=1')
        self.assertIsNone(self.cmd('coordinate_pin')['pin'])
        pin = self.cmd('coordinate_pin', 'set', 'position=2010,-3010,10')
        self.assertEqual(pin['pin'], [2010.0, -3010.0, 10.0], pin)
        ruler = self.cmd('coordinate_pin', 'set', 'position=2000,-3000,10', 'end=2030,-3000,10')
        self.assertEqual(ruler['ruler_end'], [2030.0, -3000.0, 10.0], ruler)
        self.assertEqual(ruler['pins'], 2, ruler)
        self.assertEqual(self.cmd('coordinate_pin', 'clear')['pins'], 1)
        self.assertEqual(self.cmd('coordinate_pin', 'clear', 'all=1')['pins'], 0)
        with self.assertRaises(self.error):
            self.cmd('coordinate_pin', 'set', 'position=nowhere')

    def test_layout_path(self):
        self.cmd('session_begin', 'paths')
        made = self.cmd('layout_path', 'new', 'shape=line', 'centre=2000,-2950,10')
        index = made['path']['index']
        self.assertEqual(made['path']['points'], 3, made)
        self.cmd('layout_path', 'set', f'index={index}', 'model=1228', 'spacing=5', 'name=Fence line')
        self.cmd('layout_path', 'add_point', f'index={index}', 'position=2040,-2950,10')
        built = self.cmd('layout_path', 'generate', f'index={index}')['path']
        self.assertTrue(built['live'])
        self.assertGreater(len(built['objects']), 3, built)
        self.assertEqual(built['name'], 'Fence line')
        with self.assertRaises(self.error):
            self.cmd('layout_path', 'set', f'index={index}', 'model=1225')
        self.cmd('session_rollback')
        self.assertFalse(self.items(1228))

    def test_textdraw_tools(self):
        self.cmd('session_begin', 'textdraws')
        shape = self.cmd('textdraw_tool', 'shape', 'kind=rounded', 'w=100', 'h=40', 'colour=0x112233FF')
        self.assertTrue(shape['selected'], shape)
        self.cmd('textdraw_add', 'x=50', 'y=50', 'text=One')
        self.cmd('textdraw_add', 'x=80', 'y=90', 'text=Two')
        count = len(self.cmd('textdraws')['textdraws'])
        arranged = self.cmd('textdraw_tool', 'arrange', f'ids={count - 2},{count - 1}', 'command=left')
        tds = self.cmd('textdraws')['textdraws']
        self.assertAlmostEqual(tds[count - 1]['x'], tds[count - 2]['x'], places=2)
        self.assertEqual(arranged['selected'], [count - 2, count - 1])
        self.cmd('textdraw_tool', 'board_new', 'name=Shop')
        self.cmd('textdraw_set', count - 1, 'board=Shop')
        boards = self.cmd('textdraw_tool', 'boards')['boards']
        self.assertIn({'name': 'Shop', 'textdraws': 1}, boards)
        self.cmd('textdraw_tool', 'guide_layout', 'board=Main', 'columns=1', 'num_columns=2', 'clear=1')
        self.assertTrue(self.cmd('textdraw_tool', 'guides')['guides'])
        imported = self.cmd('textdraw_tool', 'import', 'source=new Text:t = TextDrawCreate(10.0, 20.0, "Hi");')
        self.assertEqual(imported['imported'], 1, imported)
        self.assertEqual(self.cmd('textdraw_tool', 'code_target', 'target=samp')['code_target'], 'samp')
        self.cmd('session_rollback')
        self.assertEqual(len(self.cmd('textdraws')['textdraws']), 0)

    def test_nodes(self):
        first = self.cmd('nodes_add', 2100, -2900, 10, 1)
        second = self.cmd('nodes_add', 2120, -2900, 10, 1, first['area'], first['point'])
        node = self.cmd('nodes', 'get', f'area={second["area"]}', f'point={second["point"]}')['node']
        self.assertIn([first['area'], first['point']], node['links'], node)
        moved = self.cmd('nodes', 'move', f'area={second["area"]}', f'point={second["point"]}',
                         'position=2125,-2900,10')['node']
        self.assertAlmostEqual(moved['position'][0], 2125.0, places=1)
        near = self.cmd('nodes', 'list', 'x=2110', 'y=-2900', 'radius=30')
        self.assertGreaterEqual(near['total'], 2, near)
        self.cmd('nodes', 'unlink', f'area={second["area"]}', f'point={second["point"]}',
                 f'to_area={first["area"]}', f'to_point={first["point"]}')
        node = self.cmd('nodes', 'get', f'area={second["area"]}', f'point={second["point"]}')['node']
        self.assertNotIn([first['area'], first['point']], node['links'])
        self.cmd('nodes', 'delete', f'area={second["area"]}', f'point={second["point"]}')
        with self.assertRaises(self.error):
            self.cmd('nodes', 'load', f'directory={self.tmp}')

    def test_interiors_and_view(self):
        found = self.cmd('interiors', 'list', 'filter=1')
        self.assertGreaterEqual(found['total'], 1, found)
        pawn = self.cmd('interiors', 'pawn', f'index={found["interiors"][0]["index"]}')['pawn']
        self.assertIn('SetPlayerInterior(playerid, 1);', pawn)
        went = self.cmd('interiors', 'goto', f'index={found["interiors"][0]["index"]}')
        self.assertEqual(went['interior'], 1, went)
        view = self.cmd('view', 'set', 'all_hours=1', 'all_interiors=1')
        self.assertTrue(view['all_hours'] and view['all_interiors'], view)
        view = self.cmd('view', 'set', 'all_hours=0', 'all_interiors=0')
        self.assertFalse(view['all_hours'] or view['all_interiors'], view)
        self.assertEqual(self.cmd('radar_layers', 'list')['layers'], [])

    def test_export_and_pawn_code(self):
        out = self.tmp / 'part.pwn'
        a = self.barrel(2010.0)['instance_id']
        done = self.cmd('export', 'pawn', f'path={out}', f'ids={a}', 'format=streamer', 'centred=1')
        self.assertGreaterEqual(done['exported'], 1, done)
        self.assertIn('CreateDynamicObject(1225, 0.0, 0.0, 0.0', out.read_text())
        races = self.tmp / 'haumea_races.inc'
        self.cmd('export', 'races', f'path={races}')
        self.assertTrue(races.exists())
        code = self.cmd('pawn_code', 'get')
        self.assertIn('CreateObject(1225', code['text'])
        self.cmd('pawn_code', 'set', 'text=// mine\\nmain(){}')
        with self.assertRaises(self.error):
            self.cmd('pawn_code', 'refresh')
        script = self.tmp / 'script.pwn'
        self.cmd('pawn_code', 'save', f'path={script}')
        self.assertEqual(script.read_text(), '// mine\nmain(){}')
        self.assertIn('CreateObject', self.cmd('pawn_code', 'refresh', 'discard_changes=1') and
                      self.cmd('pawn_code', 'get')['text'])

    def test_selection_sets_favourites_workspace(self):
        a = self.barrel(2010.0)['instance_id']
        self.cmd('selection_sets', 'save', 'name=Gate', f'ids={a}')
        sets = self.cmd('selection_sets', 'list')['sets']
        self.assertIn({'name': 'Gate', 'ids': [a]}, sets)
        self.assertEqual(self.cmd('selection_sets', 'apply', 'name=Gate')['selected_count'], 1)
        self.cmd('select')
        self.cmd('selection_sets', 'delete', 'name=Gate')
        self.assertFalse(self.cmd('selection_sets', 'list')['sets'])
        star = self.cmd('favourites', 'set', 'models=1225', 'on=1')['favourites']
        self.assertEqual(star, [{'model': 1225, 'favourite': True}])
        self.cmd('favourites', 'set', 'models=1225', 'on=0')
        info = self.cmd('workspace', 'get')
        for key in ('workspace', 'game', 'recent_workspaces', 'recent_maps', 'autosaves'):
            self.assertIn(key, info)
        with self.assertRaises(self.error):
            self.cmd('test_in_game', 'launch')

    def test_mcp_tools(self):
        """The MCP tools format their fields the way the bridge reads them."""
        private = dict(HAUMEA_ENGINE_SOCKET=str(self.socket), HAUMEA_AGENT_STATE_DIR=str(self.tmp / 'state'),
                       HAUMEA_ASSET_DB=str(self.tmp / 'assets.sqlite3'),
                       HAUMEA_DISCOVERY_DIR=str(self.tmp / 'discoveries'))
        saved = {name: os.environ.get(name) for name in private}
        os.environ.update(private)
        try:
            try:
                import haumea_mcp
            except ImportError:
                self.skipTest('needs the mcp package (the Namaka venv)')
        finally:
            for name, value in saved.items():
                if value is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = value
        import asyncio
        import json
        from service import HaumeaService
        own = haumea_mcp.service
        haumea_mcp.service = HaumeaService(self.socket, self.tmp / 'assets.sqlite3',
                                           discovery_dir=self.tmp / 'discoveries', state_dir=self.tmp / 'state')

        def call(tool, **args):
            # namaka='editor': this editor, not a background one.
            result = asyncio.run(haumea_mcp.mcp.call_tool(tool, dict(args, namaka='editor')))
            content = result[0] if isinstance(result, tuple) else result
            if getattr(content, 'isError', False):
                raise AssertionError(content.content[0].text)
            blocks = getattr(content, 'content', content)
            return json.loads(blocks[0].text)

        try:
            self.assertEqual(call('world_buildings', action='removed')['total'], 1)
            path = call('live_paths', action='new', shape='line', centre=[2000, -2900, 10],
                        settings={'name': 'Rail', 'model': 1228,
                                  'points': [[2000, -2900, 10], [2020, -2900, 10], [2040, -2905, 10]]})
            self.assertEqual(path['path']['name'], 'Rail')
            self.assertEqual(path['path']['knots'][2], [2040, -2905, 10])
            shape = call('textdraw_layout', action='shape', kind='circle', shape={'w': 30, 'h': 30, 'colour': 0xFF0000FF})
            self.assertTrue(shape['selected'])
            found = call('map_tool', op='duplicates')
            self.assertEqual(found['copies'], 1)
            out = self.tmp / 'mcp.pwn'
            call('export_files', action='pawn', path=str(out))
            self.assertIn('CreateObject(1225', out.read_text())
            call('pawn_code', action='set', text='a\nb "c"', discard_changes=True)
            self.assertEqual(call('pawn_code')['text'], 'a\nb "c"')
            call('pawn_code', action='refresh', discard_changes=True)
            self.assertEqual(call('interiors', filter='1')['interiors'][0]['interior'], 1)
            self.assertIn('all_hours', call('editor_view', radar_layers=True))
        finally:
            haumea_mcp.service = own
            if self.client.command('session_status').get('active'):
                self.client.command('session_rollback')


if __name__ == '__main__':
    unittest.main()
