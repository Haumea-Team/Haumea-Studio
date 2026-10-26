# SPDX-License-Identifier: GPL-2.0-or-later
"""Multiplayer editing: two editors on one map (multiplayer.cpp).

One editor opens every_element.pwn and hosts it, the other joins with the
session code. The joined map must save as the same Pawn file, byte for byte;
then edits on either side (moves, properties, new and deleted elements, a
removed GTA building) must show up on the other, two people moving one
element at once must end in the same place, and each must see the other's
selection. A wrong code is refused, and stopping the session leaves both
with their map.

Each editor runs in its own Xvfb with a private config, on the synthetic game
(fixture/make_game.py) unless HAUMEA_GAME_DIR is set, talking over loopback.
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
GAME_DIR = os.environ.get('HAUMEA_GAME_DIR')
FIXTURE = HERE / 'fixtures' / 'every_element.pwn'


def wait_for(condition, what, timeout=30):
    end = time.time() + timeout
    while True:
        value = condition()
        if value:
            return value
        if time.time() > end:
            raise AssertionError('timed out waiting for ' + what)
        time.sleep(0.2)


@unittest.skipUnless(BINARY and XVFB and sys.platform.startswith('linux'), 'needs the editor and xvfb-run on Linux')
class MultiplayerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='hmp-', dir='/tmp'))
        cls.game = GAME_DIR
        if not cls.game:
            cls.game = str(cls.tmp / 'game')
            subprocess.run([sys.executable, str(HERE / 'fixture' / 'make_game.py'), cls.game],
                           check=True, capture_output=True)
        cls.editors = []
        cls.host = cls.editor('host')
        cls.guest = cls.editor('guest')

    @classmethod
    def tearDownClass(cls):
        for process, log in cls.editors:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
            log.close()
        if not os.environ.get('HAUMEA_KEEP_TMP'):
            shutil.rmtree(cls.tmp, ignore_errors=True)

    @classmethod
    def editor(cls, name):
        from haumea_ipc import HaumeaClient
        work = cls.tmp / name
        work.mkdir()
        env = editor_env(str(work), virtual_display=True)
        env['HAUMEA_TEST_XVFB'] = '1'
        socket = cls.tmp / (name + '.sock')
        log = open(work / 'out.txt', 'w')
        process = subprocess.Popen([XVFB, '-a', '-s', '-screen 0 1280x800x24', BINARY, '--game', cls.game,
                                    '--agent-socket', str(socket)],
                                   cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        cls.editors.append((process, log))
        client = HaumeaClient(socket, timeout=300)
        end = time.time() + 600
        while time.time() < end:
            try:
                if client.command('ping').get('ok'):
                    client.work = work
                    return client
            except Exception:
                pass
            time.sleep(1)
        raise AssertionError(name + ' never answered: ' + (work / 'out.txt').read_text()[-2000:])

    def save(self, client, name):
        folder = client.work / name
        folder.mkdir(exist_ok=True)
        path = folder / 'map.pwn'	# one name: the exporter names its functions after the file
        self.assertTrue(client.command('save', str(path)).get('ok'))
        return path.read_text()

    def items(self, client):
        return client.command('list')['instances']

    def find(self, client, x, y):
        for item in self.items(client):
            if abs(item['position'][0] - x) < 0.01 and abs(item['position'][1] - y) < 0.01:
                return item
        return None

    def same_map(self, name):
        return self.save(self.host, name) == self.save(self.guest, name)

    def test_session(self):
        host, guest = self.host, self.guest
        source = self.tmp / 'every_element.pwn'
        source.write_text(FIXTURE.read_text())
        self.assertTrue(host.command('map_open', str(source), 1).get('ok'))
        started = host.command('multiplayer_host', 0, 'Ana')
        self.assertTrue(started.get('ok'), started)
        port, code = started['port'], started['code']
        self.assertEqual(len(code), 6)

        # A wrong code is refused, and the guest keeps its own map.
        self.assertTrue(guest.command('multiplayer_join', '127.0.0.1', port, 'WRONG1', 'Bia').get('ok'))
        refused = wait_for(lambda: (lambda s: s if s['state'] == 'off' else None)(
            guest.command('multiplayer_status')), 'the refusal')
        self.assertIn('wrong session code', refused['status'])

        joined = guest.command('multiplayer_join', '127.0.0.1', port, code.lower(), 'Bia')
        self.assertTrue(joined.get('ok'), joined)
        wait_for(lambda: guest.command('multiplayer_status')['state'] == 'joined', 'the guest to join')
        wait_for(lambda: host.command('multiplayer_status')['peers'], 'the host to see the guest')
        self.assertEqual(host.command('multiplayer_status')['peers'][0]['name'], 'Bia')
        self.assertEqual(guest.command('multiplayer_status')['peers'][0]['name'], 'Ana')

        # The joined map is the host's, in its format, with every kind of
        # element, the removed world building and the map's textdraws.
        host_map = self.save(host, 'joined')
        guest_map = self.save(guest, 'joined')
        for part in ('#include <streamer>', 'RemoveBuildingForPlayer(playerid, 3563', 'NPC_StartPlayback',
                     'AttachDynamicObjectToObject', 'SetDynamicObjectMaterialText', 'new Text:Title'):
            self.assertIn(part, guest_map)
        self.assertEqual(host_map, guest_map)

        self.assertTrue(guest.command('session_begin').get('ok'))
        self.assertTrue(host.command('session_begin').get('ok'))

        # The guest moves the pickup; the host sees it.
        pickup = self.find(guest, 1600.0, -1900.0)
        self.assertTrue(guest.command('transform', pickup['instance_id'], 1601.5, -1901.5, 13.5, 0, 0).get('ok'))
        wait_for(lambda: self.find(host, 1601.5, -1901.5), 'the move on the host')

        # The host renames the label and retexts it; the guest sees both.
        label = self.find(host, 1602.0, -1900.0)
        self.assertTrue(host.command('properties', label['instance_id'], 'name=gWelcome',
                                     'text=Hello from Ana').get('ok'))
        wait_for(lambda: (self.find(guest, 1602.0, -1900.0) or {}).get('properties', {}).get('text') ==
                 'Hello from Ana', 'the label on the guest')

        # New elements both ways, and a deletion.
        placed = guest.command('place_element', 'object', 1337, 1590.0, -1930.0, 13.5, 30, 0)
        self.assertTrue(placed.get('ok'), placed)
        wait_for(lambda: self.find(host, 1590.0, -1930.0), 'the guest\'s object on the host')
        placed = host.command('place_element', 'vehicle', 411, 1585.0, -1930.0, 13.5, 90, 0, 'color1=3')
        self.assertTrue(placed.get('ok'), placed)
        wait_for(lambda: (self.find(guest, 1585.0, -1930.0) or {}).get('properties', {}).get('color1') == 3,
                 'the host\'s vehicle on the guest')
        actor = self.find(guest, 1607.0, -1900.0)
        self.assertTrue(guest.command('delete', actor['instance_id']).get('ok'))
        wait_for(lambda: self.find(host, 1607.0, -1900.0) is None, 'the deletion on the host')

        # A GTA building deleted on one side is removed on the other.
        def pool(client):
            objects = client.command('inspect_zone', 1479.55, -1639.6, 5)['objects']
            return next((o for o in objects if o['model_id'] == 4206), None)
        self.assertIsNotNone(pool(host))
        self.assertTrue(guest.command('delete', pool(guest)['instance_id']).get('ok'))
        wait_for(lambda: pool(host) is None, 'the building removal on the host')

        # Both move one object at once: they end in the same place.
        crate_host = self.find(host, 1600.0, -1890.0)
        crate_guest = self.find(guest, 1600.0, -1890.0)
        for step in range(10):
            host.command('transform', crate_host['instance_id'], 1570.0 + step, -1890.0, 14.0, 0, 0)
            guest.command('transform', crate_guest['instance_id'], 1550.0 - step, -1890.0, 14.0, 0, 0)
        time.sleep(2)
        position = lambda c, i: next(item['position'] for item in self.items(c) if item['instance_id'] == i)
        wait_for(lambda: position(host, crate_host['instance_id']) == position(guest, crate_guest['instance_id']),
                 'both to agree on the object')

        # Each sees what the other selected.
        self.assertTrue(guest.command('select', crate_guest['instance_id']).get('ok'))
        wait_for(lambda: host.command('multiplayer_status')['peers'][0]['selection'] ==
                 [crate_host['instance_id']], 'the guest\'s selection on the host')

        self.assertTrue(guest.command('session_commit').get('ok'))
        self.assertTrue(host.command('session_commit').get('ok'))
        wait_for(lambda: self.same_map('edited'), 'the same map after the edits', timeout=10)
        self.assertIn('RemoveBuildingForPlayer(playerid, 4206', self.save(host, 'edited'))

        # Stopping: the guest is told, and both keep the map.
        self.assertTrue(host.command('multiplayer_leave').get('ok'))
        wait_for(lambda: guest.command('multiplayer_status')['state'] == 'off', 'the guest to be told')
        self.assertTrue(self.same_map('after'))


if __name__ == '__main__':
    unittest.main()
