# SPDX-License-Identifier: GPL-2.0-or-later
"""Several Namakas at once, hidden, on the user's map (tools/agent/namaka_team.py).

The tools are called as the MCP server has them: an editor stands for the
user's, three background workers join its session, and they build in parallel
threads with namaka='<name>'. All their work must reach the user's editor while
its camera and selection stay as they were; markers must follow smoothly; one
rolling back must leave the others' work; workers go away on stop, and a worker
whose session ended goes away by itself.

Needs the mcp package (Namaka's venv) and runs in an Xvfb, as run_all.sh does.
"""
import concurrent.futures
import importlib
import inspect
import os
from pathlib import Path
import sys
import time
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / 'agent'))
import test_multiplayer as base  # noqa: E402

try:
    importlib.import_module('mcp')
    HAVE_MCP = True
except ImportError:
    HAVE_MCP = False


@unittest.skipUnless(base.BINARY and base.XVFB and HAVE_MCP and os.environ.get('HAUMEA_TEST_XVFB') == '1',
                     'needs the editor, the mcp package and to run in an Xvfb')
class NamakaTeamTests(base.EditorsMixin, unittest.TestCase):
    NAMES = ['Ana', 'Bia', 'Cid']

    @classmethod
    def setUpClass(cls):
        cls.startEditors()
        cls.user = cls.editor('user')
        os.environ['HAUMEA_ENGINE_SOCKET'] = str(cls.user.socket_path)
        os.environ['HAUMEA_AGENT_STATE_DIR'] = str(cls.tmp / 'state')
        cls.tools = importlib.import_module('haumea_mcp')

    @classmethod
    def tearDownClass(cls):
        cls.tools.team.stop_all()
        super().tearDownClass()

    def camera(self):
        return self.user.command('camera_context')['camera']

    def test_team(self):
        tools, user = self.tools, self.user
        tools.team.stop_all()
        user.command('multiplayer_leave')
        # Every tool takes namaka.
        signature = inspect.signature(tools.place_element)
        self.assertIn('namaka', signature.parameters)
        schema = next(t for t in tools.mcp._tool_manager.list_tools() if t.name == 'place_element').parameters
        self.assertIn('namaka', schema['properties'])

        source = self.tmp / 'every_element.pwn'
        source.write_text(base.FIXTURE.read_text())
        self.assertTrue(user.command('map_open', str(source), 1).get('ok'))
        before = self.camera()
        self.assertTrue(user.command('select', self.find(user, 1600.0, -1900.0)['instance_id']).get('ok'))
        selected = user.command('selection')['items']
        self.assertEqual(len(selected), 1)

        started = tools.namaka_team('spawn', self.NAMES)
        self.assertEqual({n['name'] for n in started['namakas']}, set(self.NAMES))
        self.assertTrue(all(n['running'] for n in started['namakas']))
        # This computer only: nothing listens beyond loopback, nothing asked of a router.
        self.assertEqual(started['session']['upnp']['state'], 'off')
        self.assertEqual(started['session']['ipv6'], '')
        with self.assertRaises(ValueError):
            tools.team.spawn(['Ana'])

        # Each is a person in the session, and starts with the session's map.
        base.wait_for(lambda: {p['name'] for p in self.status(user)['peers']} >= set(self.NAMES), 'the three peers')
        for name in self.NAMES:
            self.assertEqual(tools.haumea_status(namaka=name).get('map', {}).get('format'),
                             tools.haumea_status().get('map', {}).get('format'))

        # They build at once, each in its own lane.
        def build(lane):
            name = self.NAMES[lane]
            placed = []
            for i in range(10):
                placed.append(tools.place_element('object', 1500.0 + lane * 10, -1950.0 + i * 3, 13.5, model=1337,
                                                  snap=False, namaka=name))
            return placed
        with concurrent.futures.ThreadPoolExecutor(3) as pool:
            placed = list(pool.map(build, range(3)))
        self.assertEqual(sum(len(p) for p in placed), 30)
        def count(lane):
            return sum(1 for item in self.items(user) if abs(item['position'][0] - (1500.0 + lane * 10)) < 0.01 and
                       -1950.5 < item['position'][1] < -1920.0)
        base.wait_for(lambda: [count(i) for i in range(3)] == [10, 10, 10], 'all the work on the user\'s editor')

        # None of it touched the user's camera or selection.
        self.assertEqual(self.camera(), before)
        self.assertEqual(user.command('selection')['items'], selected)

        # Its marker went to its work.
        ana = lambda: next(p for p in self.status(user)['peers'] if p['name'] == 'Ana')
        base.wait_for(lambda: abs(ana()['camera'][0] - (1500.0 - 8)) < 1 and abs(ana()['shown_camera'][0] - ana()['camera'][0]) < 0.5,
                      'Ana\'s marker at her work')
        # A worker sees: its screenshot isn't empty, from its hidden editor.
        time.sleep(3)	# resting: it draws nothing, and wakes for this
        shot = self.tmp / 'ana.png'
        tools.capture_view(str(shot), position=[1480.0, -1960.0, 40.0], target=[1510.0, -1935.0, 13.5], namaka='Ana')
        self.assertGreater(len(set(shot.read_bytes()[64:4000])), 32, 'an empty screenshot')
        # And her marker goes to where she looked from.
        base.wait_for(lambda: abs(ana()['shown_camera'][0] - 1480.0) < 0.5, 'Ana\'s marker where she looked from')

        # A worker at rest still receives the session: what the user places reaches it.
        time.sleep(3)
        tools.place_element('object', 1590.0, -1960.0, 13.5, model=1337, snap=False, namaka='editor')
        base.wait_for(lambda: any(abs(i['position'][0] - 1590.0) < 0.01 for i in
                                  tools.team.service_for('Ana').client.command('list')['instances']), 'the user\'s edit on a resting worker')

        # A long jump, by the camera of her own hidden editor, glides: never there in one look.
        tools.team.service_for('Ana').client.command('camera', 2500.0, -1700.0, 60.0, 2500.0, -1690.0, 50.0, 70.0)
        seen = []
        end = time.time() + 6
        while time.time() < end:
            peer = ana()
            seen.append(peer['shown_camera'][0])
            if abs(peer['shown_camera'][0] - 2500.0) < 0.5:
                break
        self.assertLess(abs(seen[-1] - 2500.0), 0.5, seen[-5:])
        between = [x for x in seen if 1600.0 < x < 2400.0]
        self.assertTrue(between, 'the marker jumped: %r' % seen[:20])
        self.assertEqual(seen, sorted(seen), 'the marker went back and forth')

        # A selection shows others what she works on, and leaves the user's alone.
        ana_id = next(i for i in tools.team.service_for('Ana').client.command('list')['instances']
                      if abs(i['position'][0] - 1500.0) < 0.01)['instance_id']
        self.assertTrue(user.command('select', self.find(user, 1600.0, -1900.0)['instance_id']).get('ok'))
        selected = user.command('selection')['items']
        self.assertEqual(len(selected), 1)
        tools.select_elements([ana_id], namaka='Ana')
        base.wait_for(lambda: ana()['selection'], 'her selection on the user\'s editor')
        self.assertEqual(user.command('selection')['items'], selected)

        # Talking to a Namaka in the chat: she waits for a mention, the user writes, she answers.
        import threading
        heard = {}
        waiter = threading.Thread(target=lambda: heard.update(tools.session_chat('wait', timeout_seconds=30, namaka='Ana')))
        waiter.start()
        time.sleep(1)
        self.assertTrue(user.command('multiplayer_chat', 'Bia, ignore this').get('ok'))
        self.assertTrue(user.command('multiplayer_chat', '@Ana please tidy the crates').get('ok'))
        waiter.join(35)
        self.assertEqual([m['text'] for m in heard.get('messages', [])], ['@Ana please tidy the crates'])
        tools.session_chat('send', text='On it: tidying the crates', namaka='Ana')
        base.wait_for(lambda: any(m['text'] == 'On it: tidying the crates' and m['agent'] and m['name'] == 'Ana'
                                  for m in user.command('multiplayer_chat_read', 0)['messages']), 'her answer')
        ana_peer = next(p for p in self.status(user)['peers'] if p['name'] == 'Ana')
        self.assertTrue(ana_peer['agent'])
        self.assertTrue(ana_peer['owner'], 'no "via" on a hosted session')

        # One rolling back takes back only hers.
        tools.finish_edit_session(commit=False, namaka='Bia')
        base.wait_for(lambda: [count(i) for i in range(3)] == [10, 0, 10], 'Bia\'s rollback only')

        # Saving from a Namaka keeps its edits but writes no file; the user's save has them.
        kept = tools.save_map(namaka='Cid')
        self.assertFalse(kept['saved'])
        with self.assertRaises(Exception):
            tools.open_map(str(source), namaka='Ana')	# would take the session from everyone
        with self.assertRaises(Exception):
            tools.place_element('object', 0.0, 0.0, 0.0, model=1337, namaka='Nobody')

        # Stopping one leaves the rest; its edits stay.
        processes = {w.name: w.process for w in tools.team.workers.values()}
        tools.namaka_team('stop', ['Cid'])
        self.assertIsNotNone(processes['Cid'].poll())
        base.wait_for(lambda: 'Cid' not in {p['name'] for p in self.status(user)['peers']}, 'Cid to leave')
        self.assertEqual(count(2), 10)

        # The session ends: the rest go by themselves, and the user keeps the map.
        user.command('multiplayer_leave')
        base.wait_for(lambda: all(p.poll() is not None for p in processes.values()), 'the workers to quit', timeout=45)
        self.assertEqual(count(0), 10)
        self.assertEqual(tools.namaka_team('status')['namakas'][0]['running'], False)

    def test_hidden_by_default(self):
        tools, user = self.tools, self.user
        tools.team.stop_all()
        if self.status(user)['state'] != 'off':
            user.command('multiplayer_leave')
        source = self.tmp / 'default.pwn'
        source.write_text(base.FIXTURE.read_text())
        self.assertTrue(user.command('map_open', str(source), 1).get('ok'))
        self.assertTrue(user.command('select', self.find(user, 1600.0, -1900.0)['instance_id']).get('ok'))
        before = self.camera()

        # Searching starts nothing; looking at the scene does.
        tools.haumea_status()
        tools.search_assets('crate')
        self.assertEqual(tools.team.workers, {})
        self.assertEqual(self.status(user)['state'], 'off')
        tools.place_element('object', 1500.0, -1950.0, 13.5, model=1337, snap=False)
        self.assertEqual(list(tools.team.workers), ['Namaka'])
        status = self.status(user)
        self.assertEqual((status['state'], status['upnp']['state'], status['ipv6']), ('hosting', 'off', ''))
        self.assertEqual(status['peers'][0]['name'], 'Namaka')
        base.wait_for(lambda: self.find(user, 1500.0, -1950.0), 'the hidden edit on the user\'s editor')
        self.assertEqual(self.camera(), before)
        self.assertTrue(tools.haumea_status()['session']['active'])
        # The user's map stays the map in the status, their selection in the worker's ids.
        self.assertEqual(tools.haumea_status()['map']['path'], str(source))
        chosen = tools.selected_mapping_objects()['items']
        self.assertEqual([i['position'][:2] for i in chosen], [[1600.0, -1900.0]])
        worker = tools.team.default_running()
        self.assertIn(chosen[0]['instance_id'], [i['instance_id'] for i in worker.client.command('list')['instances']])

        # Moving the user's view only happens when asked, and takes the worker's id.
        placed = next(i for i in worker.client.command('list')['instances'] if abs(i['position'][0] - 1500.0) < 0.01)
        moved = tools.move_camera(look_at=placed['instance_id'], distance=20)
        self.assertLess(abs(moved['camera']['target'][0] - 1500.0), 1.0, moved['camera'])
        tools.camera_back()
        base.wait_for(lambda: self.camera()['position'] == before['position'], 'the camera back')

        # Saving right after an edit: the file has it, and the session is committed.
        tools.place_element('object', 1505.0, -1950.0, 13.5, model=1337, snap=False)
        saved = tools.save_map()
        self.assertNotIn('warning', saved)
        text = source.read_text()
        self.assertEqual(text.count('1337'), 2, text[:500])
        self.assertFalse(tools.team.default_running().client.command('session_status').get('active'))

        # namaka='editor' works where the user watches.
        tools.place_element('object', 1510.0, -1950.0, 13.5, model=1337, snap=False, namaka='editor')
        self.assertTrue(user.command('session_status').get('active'))
        user.command('session_commit')

        # A saved prefab lands with the user's, not in the worker's private folder.
        ids = [i['instance_id'] for i in tools.team.default_running().client.command('list')['instances']
               if abs(i['position'][0] - 1500.0) < 0.01]
        tools.save_prefab('from_hidden', ids)
        self.assertIn('from_hidden', str(tools.list_prefabs()))
        self.assertIn('from_hidden', str(tools.owner_service.list_prefabs()))
        tools.namaka_team('stop')

    def test_on_a_server_with_accounts(self):
        """The user signed in to a server with accounts: Namakas join with a token from the
        user's account (no password reaches the agent), tagged as theirs, in the user's room.
        A session saved by a one-session server before comes back as a room."""
        import shutil, signal, subprocess
        tools, user = self.tools, self.user
        tools.team.stop_all()
        user.command('multiplayer_leave')
        # A one-session server's session: made, saved, stopped.
        old_state = self.tmp / 'old.hmps'
        port = base.free_port()
        log = open(self.tmp / 'old.txt', 'w')
        old = subprocess.Popen([base.SERVER, '--port', str(port), '--password', 'old-pass-1', '--state', str(old_state)],
                               stdout=log, stderr=subprocess.STDOUT)
        base.wait_for(lambda: 'listening' in (self.tmp / 'old.txt').read_text(), 'the old server')
        source = self.tmp / 'old.pwn'
        source.write_text(base.FIXTURE.read_text())
        self.assertTrue(user.command('map_open', str(source), 1).get('ok'))
        self.assertTrue(user.command('multiplayer_join', '127.0.0.1', port, 'old-pass-1', 'Ana').get('ok'))
        base.wait_for(lambda: self.status(user)['state'] == 'joined', 'the old session')
        user.command('multiplayer_leave')
        old.send_signal(signal.SIGTERM)
        old.wait(timeout=20)
        # The new server: the old session is room main.
        state = self.tmp / 'state'
        (state / 'rooms').mkdir(parents=True)
        shutil.move(str(old_state), str(state / 'rooms' / 'main.hmps'))
        code = subprocess.run([base.SERVER, '--state-dir', str(state), 'invite', '--role', 'owner'], capture_output=True,
                              text=True, check=True).stdout.split(': ', 1)[1].split()[0]
        port = base.free_port()
        log = open(self.tmp / 'new.txt', 'w')
        server = subprocess.Popen([base.SERVER, '--state-dir', str(state), '--port', str(port)], stdout=log,
                                  stderr=subprocess.STDOUT)
        try:
            base.wait_for(lambda: 'listening' in (self.tmp / 'new.txt').read_text(), 'the new server')
            self.assertIn('1 room(s)', (self.tmp / 'new.txt').read_text())
            joined = user.command('multiplayer_join', '127.0.0.1', port, code, 'x', 'invite', '', 'main', '', 'Ana',
                                  'ana-pass-123')
            self.assertTrue(joined.get('ok'), joined)
            base.wait_for(lambda: self.status(user)['state'] == 'joined', 'room main')
            self.assertIsNotNone(self.find(user, 1600.0, -1900.0), 'the old session is not in room main')
            # A Namaka: by token, hers, in her room.
            started = tools.namaka_team('spawn', ['Ana2'])
            self.assertTrue(started['namakas'][0]['running'])
            peer = base.wait_for(lambda: next((p for p in self.status(user)['peers'] if p['name'] == 'Ana2'), None), 'Ana2')
            self.assertEqual((peer['agent'], peer['owner'], peer['role']), (True, 'Ana', 'builder'))
            tools.place_element('object', 1700.0, -1700.0, 13.5, model=1337, snap=False, namaka='Ana2')
            base.wait_for(lambda: self.find(user, 1700.0, -1700.0), 'its edit')
            self.assertNotIn('ana-pass-123', (self.tmp / 'state').joinpath('log.txt').read_text())
        finally:
            tools.team.stop_all()
            user.command('multiplayer_leave')
            server.send_signal(signal.SIGTERM)
            server.wait(timeout=20)


if __name__ == '__main__':
    unittest.main()
