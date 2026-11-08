# SPDX-License-Identifier: GPL-2.0-or-later
"""Multiplayer editing: editors on one map (multiplayer.cpp, mprelay.cpp).

MultiplayerTests: one editor opens every_element.pwn and hosts it, asking
its (fake) router to forward the port, and the other joins with the session
code. The joined map must save as the same Pawn file, byte for byte;
then edits on either side (moves, properties, new and deleted elements, a
removed GTA building) must show up on the other, two people moving one
element at once must end in the same place, and each must see the other's
selection. A wrong code is refused, and stopping the session leaves both
with their map.

ServerTests: the same through haumea-server: the first editor into the empty
session seeds it, with its area removals, radar art, a custom model from its
server folder, its TextDraw file, an alignment pin and a 3D path holding one
of its objects, and a node added with the Nodes tool; what goes over the
wire is sealed; agents in both
editors and two in one editor work at once, and one rolling back or undoing
leaves the others' work alone; a wrong password is refused; the session outlives a restart
of the server through its state file.

Each editor runs in its own Xvfb with a private config, on the synthetic game
(fixture/make_game.py) unless HAUMEA_GAME_DIR is set, talking over loopback.
"""
from pathlib import Path
import glob
import socket
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET

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
# haumea-server is its own repo now: HAUMEA_SERVER_BINARY, or its build beside this checkout.
SERVER = os.environ.get('HAUMEA_SERVER_BINARY') or next(iter(
    p for p in (str(d / 'haumea-server' / 'build' / 'haumea-server') for d in (ROOT.parent, ROOT.parent.parent))
    if os.path.isfile(p)), None)


def wait_for(condition, what, timeout=30):
    end = time.time() + timeout
    while True:
        value = condition()
        if value:
            return value
        if time.time() > end:
            raise AssertionError('timed out waiting for ' + what)
        time.sleep(0.2)


class EditorsMixin:
    @classmethod
    def startEditors(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='hmp-', dir='/tmp'))
        cls.game = GAME_DIR
        if not cls.game:
            cls.game = str(cls.tmp / 'game')
            subprocess.run([sys.executable, str(HERE / 'fixture' / 'make_game.py'), cls.game],
                           check=True, capture_output=True)
        cls.editors = []

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
    def editor(cls, name, extra_env=None, extra_args=()):
        from haumea_ipc import HaumeaClient
        work = cls.tmp / name
        work.mkdir()
        env = editor_env(str(work), virtual_display=True)
        env['HAUMEA_TEST_XVFB'] = '1'
        env.update(extra_env or {})
        socket = cls.tmp / (name + '.sock')
        log = open(work / 'out.txt', 'w')
        process = subprocess.Popen([XVFB, '-a', '-s', '-screen 0 1280x800x24', BINARY, '--game', cls.game,
                                    '--agent-socket', str(socket), *extra_args],
                                   cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        cls.editors.append((process, log))
        client = HaumeaClient(socket, timeout=300)
        end = time.time() + 600
        while time.time() < end:
            try:
                if client.command('ping').get('ok'):
                    client.work = work
                    client.socket_path = socket
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

    def status(self, client):
        return client.command('multiplayer_status')

    def wait_state(self, client, state, what):
        return wait_for(lambda: (lambda s: s if s['state'] == state else None)(self.status(client)), what)


@unittest.skipUnless(BINARY and XVFB and sys.platform.startswith('linux'), 'needs the editor and xvfb-run on Linux')
class MultiplayerTests(EditorsMixin, unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.startEditors()
        # The host's router: a fake one, never the real network's.
        cls.router = FakeRouter()
        cls.host = cls.editor('host', {'HAUMEA_UPNP_LOCATION': cls.router.location})
        cls.guest = cls.editor('guest')

    @classmethod
    def tearDownClass(cls):
        super().tearDownClass()
        cls.router.close()

    def test_session(self):
        host, guest = self.host, self.guest
        source = self.tmp / 'every_element.pwn'
        source.write_text(FIXTURE.read_text())
        self.assertTrue(host.command('map_open', str(source), 1).get('ok'))
        started = host.command('multiplayer_host', 0, 'Ana')
        self.assertTrue(started.get('ok'), started)
        port, code = started['port'], started['code']
        self.assertEqual(len(code), 6)
        # The router was asked to forward the port to this computer.
        upnp = wait_for(lambda: (lambda u: u if u['state'] != 'working' else None)(
            host.command('multiplayer_status')['upnp']), 'the router')
        self.assertEqual(upnp, {'state': 'open', 'external': '203.0.113.7', 'error': ''})
        added = next(body for action, body in self.router.calls if action == 'AddPortMapping')
        self.assertIn('<NewExternalPort>%d</NewExternalPort>' % port, added)
        self.assertIn('<NewInternalClient>127.0.0.1</NewInternalClient>', added)

        # A wrong code is refused, and the guest keeps its own map.
        self.assertTrue(guest.command('multiplayer_join', '127.0.0.1', port, 'WRONG1', 'Bia').get('ok'))
        refused = wait_for(lambda: (lambda s: s if s['state'] == 'off' else None)(
            guest.command('multiplayer_status')), 'the refusal')
        self.assertIn('wrong password', refused['status'])

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

        # An object's MoveObject travels, and a race's settings reach every
        # checkpoint of the race on the other side (one undo step, several elements).
        mover = self.find(guest, 1590.0, -1930.0)
        self.assertTrue(guest.command('properties', mover['instance_id'], 'move_offset=[0, 0, 3]',
                                      'move_speed=2').get('ok'))
        wait_for(lambda: (self.find(host, 1590.0, -1930.0) or {}).get('properties', {}).get('move_speed') == 2,
                 'the object\'s movement on the host')
        def race(client):
            return [i['properties'] for i in self.items(client) if i['properties'].get('race')]
        checkpoint = next(i for i in self.items(host) if i['properties'].get('race'))
        self.assertTrue(host.command('properties', checkpoint['instance_id'], 'race_mode=circuit', 'race_laps=3',
                                     'race_playable=true').get('ok'))
        wait_for(lambda: (lambda r: len(r) == 3 and all(p['race_mode'] == 'circuit' and p.get('race_laps') == 3 and
                                                        p['race_playable'] for p in r))(race(guest)),
                 'the race settings on the guest')

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

        # Stopping: the guest is told, both keep the map, and the router
        # forwards the port no more.
        self.assertTrue(host.command('multiplayer_leave').get('ok'))
        wait_for(lambda: guest.command('multiplayer_status')['state'] == 'off', 'the guest to be told')
        self.assertTrue(self.same_map('after'))
        deleted = next(body for action, body in self.router.calls if action == 'DeletePortMapping')
        self.assertIn('<NewExternalPort>%d</NewExternalPort>' % port, deleted)


def img_entries(path):
    import struct
    with open(path, 'rb') as f:
        assert f.read(4) == b'VER2'
        count, = struct.unpack('<I', f.read(4))
        entries = {}
        for _ in range(count):
            offset, size, size2, name = struct.unpack('<IHH24s', f.read(32))
            entries[name.split(b'\0')[0].decode().lower()] = (offset * 2048, (size or size2) * 2048)
        return entries


class FakeRouter:
    """A UPnP Internet Gateway Device's description and control URL, on loopback."""

    DESCRIPTION = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0"><device>
<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
<deviceList><device><deviceList><device><serviceList><service>
<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
<controlURL>/ctl/IPConn</controlURL>
</service></serviceList></device></deviceList></device></deviceList>
</device></root>"""

    def __init__(self):
        import http.server
        import threading
        router = self
        self.calls = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def reply(self, body, status=200):
                data = body.encode()
                self.send_response(status)
                self.send_header('Content-Type', 'text/xml')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                self.reply(router.DESCRIPTION)

            def do_POST(self):
                body = self.rfile.read(int(self.headers['Content-Length'])).decode()
                action = self.headers['SOAPAction'].strip('"').split('#')[1]
                router.calls.append((action, body))
                extra = '<NewExternalIPAddress>203.0.113.7</NewExternalIPAddress>' \
                    if action == 'GetExternalIPAddress' else ''
                self.reply('<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>'
                           '<u:%sResponse>%s</u:%sResponse></s:Body></s:Envelope>' % (action, extra, action))

        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.location = 'http://127.0.0.1:%d/rootDesc.xml' % self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class Recorder:
    """A TCP proxy to 127.0.0.1:target that keeps every byte both ways."""

    def __init__(self, target):
        import threading
        self.target, self.seen, self.lock = target, bytearray(), threading.Lock()
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(4)
        self.port = self.listener.getsockname()[1]
        self.sockets = [self.listener]
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        import threading
        try:
            while True:
                client, _ = self.listener.accept()
                server = socket.create_connection(('127.0.0.1', self.target))
                self.sockets += [client, server]
                for a, b in ((client, server), (server, client)):
                    threading.Thread(target=self.pipe, args=(a, b), daemon=True).start()
        except OSError:
            pass

    def pipe(self, a, b):
        try:
            while True:
                data = a.recv(65536)
                if not data:
                    break
                with self.lock:
                    self.seen += data
                b.sendall(data)
        except OSError:
            pass
        finally:
            for s in (a, b):
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    def bytes(self):
        with self.lock:
            return bytes(self.seen)

    def close(self):
        for s in self.sockets:
            s.close()


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


@unittest.skipUnless(BINARY and SERVER and XVFB and sys.platform.startswith('linux'),
                     'needs the editor, haumea-server (its own repo: build it, or set HAUMEA_SERVER_BINARY) and xvfb-run')
class ServerTests(EditorsMixin, unittest.TestCase):
    PASSWORD = 'Mapping Crew 7'

    @classmethod
    def setUpClass(cls):
        cls.startEditors()
        cls.server_binary = SERVER
        cls.server = None
        cls.port = free_port()
        cls.state = cls.tmp / 'session.hmps'
        cls.host = cls.editor('ana')
        cls.guest = cls.editor('bia')

    @classmethod
    def tearDownClass(cls):
        cls.stop_server()
        super().tearDownClass()

    @classmethod
    def start_server(cls):
        cls.server_log = open(cls.tmp / 'server.txt', 'a')
        cls.server = subprocess.Popen([cls.server_binary, '--port', str(cls.port), '--password', cls.PASSWORD,
                                       '--state', str(cls.state), '--name', 'Crew server'],
                                      stdout=cls.server_log, stderr=subprocess.STDOUT)
        wait_for(lambda: 'listening' in (cls.tmp / 'server.txt').read_text(), 'the server to listen')

    @classmethod
    def stop_server(cls, how=signal.SIGTERM):
        if cls.server and cls.server.poll() is None:
            cls.server.send_signal(how)
            cls.server.wait(timeout=10)
            cls.server_log.close()
        cls.server = None

    def join(self, client, name, password=None):
        joined = client.command('multiplayer_join', '127.0.0.1', self.port, password or self.PASSWORD, name)
        self.assertTrue(joined.get('ok'), joined)

    def test_server_session(self):
        host, guest = self.host, self.guest
        self.start_server()
        server_log = lambda: (self.tmp / 'server.txt').read_text()
        # every_element.pwn, plus an area removal and radar art: the map's
        # own lists, which travel as channels.
        lines = FIXTURE.read_text().splitlines(True)
        at = next(i for i, line in enumerate(lines) if 'RemoveBuildingForPlayer' in line)
        lines.insert(at + 1, '\tRemoveBuildingForPlayer(playerid, -1, 1500.0, -1700.0, 13.0, 2.0);\n')
        # A 0.3.DL custom model from Ana's server folder, which Bia doesn't have.
        at = next(i for i, line in enumerate(lines) if 'GangZoneCreate' in line)
        lines.insert(at + 1, '\tCreateObject(-2000, 1560.0, -1940.0, 13.5, 0.0, 0.0, 0.0);\n')
        server = self.tmp / 'crew'
        (server / 'models').mkdir(parents=True)
        (server / 'scriptfiles').mkdir()
        img = Path(self.game, 'models', 'gta3.img')
        entries = img_entries(img)
        dff = next(name for name in sorted(entries) if name.endswith('.dff'))
        txd = next(name for name in sorted(entries) if name.endswith('.txd'))
        with open(img, 'rb') as f:
            for name, target in ((dff, 'crewbox.dff'), (txd, 'crewbox.txd')):
                offset, size = entries[name]
                f.seek(offset)
                (server / 'models' / target).write_bytes(f.read(size))
        (server / 'models' / 'artconfig.txt').write_text('AddSimpleModel(-1, 1337, -2000, "crewbox.dff", "crewbox.txd");\n')
        source = server / 'scriptfiles' / 'crew.pwn'
        source.write_text(''.join(lines))
        self.assertTrue(host.command('map_open', str(source), 1).get('ok'))
        # An alignment pin and a 3D path holding the crate, from the map's
        # editor document (they live only there).
        lists = source	# in the server folder still, for its custom models
        self.assertTrue(host.command('save', str(lists)).get('ok'))
        document = ET.parse(str(lists) + '.xml')
        root = document.getroot()
        crate_id = next(o.get('id') for o in root.iter('object')
                        if abs(float(o.get('position.x', 0)) - 1600.0) < .01 and
                        abs(float(o.get('position.y', 0)) + 1890.0) < .01)
        yes, no = root.find('.//alignment').get('show'), 'false' if root.find('.//alignment').get('show') == 'true' else '0'
        vector = lambda name, x, y, z: {name + '.x': str(x), name + '.y': str(y), name + '.z': str(z)}
        ET.SubElement(root.find('.//alignment'), 'pin', {'id': '1', 'area': '0', 'measured': no, 'end-pin': '0',
                      **vector('position', 1555.5, -1955.5, 13.5), **vector('end', 1555.5, -1955.5, 13.5),
                      **vector('normal', 0, 0, 1)})
        path = ET.SubElement(root.find('.//layout-paths'), 'path', {
            'id': '1', 'name': 'Crew fence', 'area': '0', 'model': '1337', 'curved': no, 'closed': no,
            'live': no, 'follow': no, 'ground': no, 'along-length': no, 'spacing': '4', 'heading': '0',
            'bank': '0'})
        for x in (1590.0, 1610.0):
            ET.SubElement(path, 'knot', {'automatic': yes, **vector('position', x, -1960.0, 13.5),
                                         **vector('in', x, -1960.0, 13.5), **vector('out', x, -1960.0, 13.5)})
        ET.SubElement(path, 'object', {'ref': crate_id})
        document.write(str(lists) + '.xml')
        self.assertTrue(host.command('map_open', str(lists), 1).get('ok'))
        zones = self.save(host, 'plain').count('GangZoneCreate')
        self.assertTrue(host.command('session_begin').get('ok'))
        # From every placed object: their footprints differ between games.
        placed = ','.join(str(item['instance_id']) for item in self.items(host) if item.get('kind') == 'object')
        art = host.command('maptool', 'radar_art', 'ids=' + placed, 'cell=0.5')
        self.assertTrue(art.get('ok') and art['zones'] > 0, art)
        # A textdraw in the TextDraw editor's file.
        self.assertTrue(host.command('textdraw_add', 'text=Crew hello').get('ok'))
        self.assertTrue(host.command('session_commit').get('ok'))
        opened = self.save(host, 'opened')
        self.assertIn('RemoveBuildingForPlayer(playerid, -1, 1500.0', opened)
        self.assertIn('CreateObject(-2000, 1560.0', opened.replace('CreateDynamicObject', 'CreateObject'))
        self.assertGreater(opened.count('GangZoneCreate'), zones)

        # A wrong password is refused.
        self.join(guest, 'Bia', 'mapping crew 7')
        refused = self.wait_state(guest, 'off', 'the refusal')
        self.assertIn('wrong password', refused['status'])

        # Ana is first into the empty session: her map becomes it.
        self.join(host, 'Ana')
        self.wait_state(host, 'joined', 'Ana to seed the session')
        wait_for(lambda: 'the session has' in server_log(), 'the seed to arrive')
        self.assertEqual(self.status(host)['session'], 'Crew server')
        self.assertEqual(self.save(host, 'seeded'), opened)

        # Bia gets the session: the same map, lists included. She joins
        # through a proxy that keeps what goes over the wire: sealed, it shows
        # neither the password nor the map's text.
        wire = Recorder(self.port)
        self.addCleanup(wire.close)
        joined = guest.command('multiplayer_join', '127.0.0.1', wire.port, self.PASSWORD, 'Bia')
        self.assertTrue(joined.get('ok'), joined)
        self.wait_state(guest, 'joined', 'Bia to join')
        wait_for(lambda: self.status(host)['peers'], 'Ana to see Bia')
        joined = self.save(guest, 'joined')
        self.assertIn('RemoveBuildingForPlayer(playerid, -1, 1500.0', joined)
        self.assertEqual(joined, opened)
        # The pin, and the path with the crate: Bia's own document id for it.
        root = ET.parse(str(guest.work / 'joined' / 'map.pwn') + '.xml').getroot()
        pin = root.find('.//alignment/pin')
        self.assertEqual(float(pin.get('position.x')), 1555.5)
        path = root.find('.//layout-paths/path')
        self.assertEqual(path.get('name'), 'Crew fence')
        held = next(o for o in root.iter('object') if o.get('id') == path.find('object').get('ref'))
        self.assertAlmostEqual(float(held.get('position.x')), 1600.0, places=2)
        self.assertAlmostEqual(float(held.get('position.y')), -1890.0, places=2)
        texts = lambda c: [td.get('text') for td in c.command('textdraws').get('textdraws', [])]
        self.assertIn('Crew hello', texts(guest))
        seen = wire.bytes()
        self.assertGreater(len(seen), 2000)	# the whole session went this way
        for secret in (self.PASSWORD, 'Crew hello', 'Welcome', 'crewbox', 'Bia'):
            self.assertNotIn(secret.encode(), seen)
        # A node added with the Nodes tool reaches the other editor's areas.
        if host.command('nodes_status').get('loaded') and guest.command('nodes_status').get('loaded'):
            before = guest.command('nodes_status')['fingerprint']
            added = host.command('nodes_add', 1590.0, -1960.0, 13.5, 1)
            self.assertTrue(added.get('ok'), added)
            ours = host.command('nodes_status')['fingerprint']
            self.assertNotEqual(ours, before)
            wait_for(lambda: guest.command('nodes_status')['fingerprint'] == ours, 'Ana\'s node on Bia')
            self.assertEqual(guest.command('nodes_status')['edited_areas'], host.command('nodes_status')['edited_areas'])
        else:
            print('(no node files in this game: the Nodes part is skipped)')
        # The textdraw file changes live both ways.
        self.assertTrue(guest.command('session_begin').get('ok'))
        self.assertTrue(guest.command('textdraw_add', 'text=From Bia').get('ok'))
        self.assertTrue(guest.command('session_commit').get('ok'))
        wait_for(lambda: 'From Bia' in texts(host), 'Bia\'s textdraw on Ana')

        # Two agents at once: each places an object in its own scratch session.
        self.assertTrue(host.command('session_begin').get('ok'))
        self.assertTrue(guest.command('session_begin').get('ok'))
        self.assertTrue(host.command('place_element', 'object', 1337, 1580.0, -1940.0, 13.5, 0, 0).get('ok'))
        self.assertTrue(guest.command('place_element', 'object', 1337, 1581.0, -1940.0, 13.5, 0, 0).get('ok'))
        wait_for(lambda: self.find(guest, 1580.0, -1940.0) and self.find(host, 1581.0, -1940.0),
                 'both objects on both')

        # Bia's agent rolls back: her object goes everywhere, Ana's stays.
        self.assertTrue(guest.command('session_rollback').get('ok'))
        wait_for(lambda: self.find(host, 1581.0, -1940.0) is None, 'the rollback on Ana')
        self.assertIsNotNone(self.find(guest, 1580.0, -1940.0))
        self.assertIsNotNone(self.find(host, 1580.0, -1940.0))

        # Ana moves the crate, then Bia moves it: Ana's undo skips the move
        # Bia overrode and undoes her step before it, her object.
        self.assertTrue(guest.command('session_begin').get('ok'))
        crate_host = self.find(host, 1600.0, -1890.0)
        self.assertTrue(host.command('transform', crate_host['instance_id'], 1605.0, -1890.0, 14.0, 0, 0).get('ok'))
        crate_guest = wait_for(lambda: self.find(guest, 1605.0, -1890.0), 'Ana\'s move on Bia')
        self.assertTrue(guest.command('transform', crate_guest['instance_id'], 1606.0, -1891.0, 14.0, 0, 0).get('ok'))
        wait_for(lambda: self.find(host, 1606.0, -1891.0), 'Bia\'s move on Ana')
        self.assertTrue(host.command('undo').get('ok'))
        wait_for(lambda: self.find(guest, 1580.0, -1940.0) is None, 'Ana\'s undo on Bia')
        self.assertIsNone(self.find(host, 1580.0, -1940.0))
        self.assertIsNotNone(self.find(host, 1606.0, -1891.0))
        self.assertIsNotNone(self.find(guest, 1606.0, -1891.0))

        self.assertTrue(host.command('session_commit').get('ok'))
        self.assertTrue(guest.command('session_commit').get('ok'))

        # Two agents in Ana's one editor, each with an edit session of its own.
        from haumea_ipc import HaumeaClient, HaumeaError
        one = HaumeaClient(host.socket_path, timeout=300, agent='one')
        two = HaumeaClient(host.socket_path, timeout=300, agent='two')
        self.assertTrue(one.command('session_begin').get('ok'))
        self.assertTrue(two.command('session_begin').get('ok'))
        self.assertTrue(one.command('place_element', 'object', 1337, 1570.0, -1950.0, 13.5, 0, 0).get('ok'))
        self.assertTrue(two.command('place_element', 'object', 1337, 1571.0, -1950.0, 13.5, 0, 0).get('ok'))
        with self.assertRaises(HaumeaError):
            one.command('undo')	# the last step is two's
        self.assertTrue(two.command('session_rollback').get('ok'))
        self.assertIsNone(self.find(host, 1571.0, -1950.0))
        self.assertIsNotNone(self.find(host, 1570.0, -1950.0))
        # Two moves one's object; one rolling back leaves it where two put it.
        self.assertTrue(two.command('session_begin').get('ok'))
        theirs = self.find(host, 1570.0, -1950.0)
        self.assertTrue(two.command('transform', theirs['instance_id'], 1572.0, -1951.0, 13.5, 0, 0).get('ok'))
        self.assertTrue(one.command('session_rollback').get('ok'))
        self.assertIsNotNone(self.find(host, 1572.0, -1951.0))
        self.assertTrue(two.command('session_commit').get('ok'))
        wait_for(lambda: self.find(guest, 1572.0, -1951.0), 'two\'s move on Bia')

        wait_for(lambda: self.same_map('edited'), 'the same map after the edits', timeout=10)
        edited = self.save(host, 'edited')

        # The server dies outright: both are told and keep the map. It starts
        # again from its state file and journal, and Bia gets the session as
        # it was, the last edits included.
        self.stop_server(signal.SIGKILL)
        self.wait_state(host, 'off', 'Ana to be told')
        self.wait_state(guest, 'off', 'Bia to be told')
        self.assertTrue(guest.command('map_new', 1).get('ok'))
        self.start_server()
        wait_for(lambda: 'loaded' in server_log(), 'the session to load')
        # Through Namaka's multiplayer_session, which waits until joined.
        from service import HaumeaService
        namaka = HaumeaService(guest.socket_path, self.tmp / 'assets.sqlite3', discovery_dir=self.tmp / 'discoveries',
                               state_dir=self.tmp / 'state')
        rejoined = namaka.multiplayer('join', address='127.0.0.1', port=self.port, password=self.PASSWORD,
                                      name='Bia', discard_changes=True)
        self.assertEqual(rejoined['state'], 'joined', rejoined)
        self.assertEqual(self.save(guest, 'restored'), edited)

        # Ana rejoins, over IPv6 when this machine has it, and places an
        # object: its id is new to the session.
        ipv6 = socket.has_ipv6
        if ipv6:
            try:
                socket.create_connection(('::1', self.port), timeout=5).close()
            except OSError:
                ipv6 = False
        joined = host.command('multiplayer_join', '::1' if ipv6 else '127.0.0.1', self.port, self.PASSWORD, 'Ana')
        self.assertTrue(joined.get('ok'), joined)
        self.wait_state(host, 'joined', 'Ana to rejoin')
        self.assertIn('Ana joined from %s' % ('::1' if ipv6 else '127.0.0.1'), server_log())
        self.assertTrue(host.command('session_begin').get('ok'))
        self.assertTrue(host.command('place_element', 'object', 1337, 1582.0, -1940.0, 13.5, 0, 0).get('ok'))
        wait_for(lambda: self.find(guest, 1582.0, -1940.0), 'Ana\'s new object on Bia')
        self.assertIsNotNone(self.find(guest, 1606.0, -1891.0))
        self.assertTrue(host.command('session_commit').get('ok'))
        wait_for(lambda: self.same_map('rejoined'), 'the same map after rejoining', timeout=10)


if __name__ == '__main__':
    unittest.main()
