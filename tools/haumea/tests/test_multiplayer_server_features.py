# SPDX-License-Identifier: GPL-2.0-or-later
"""haumea-server's chat, kept maps and maps loaded for everyone (mprelay.cpp).

Ana and Bia (people) and Robo (a --background editor, an agent's) join a
server started with --maps-dir, --map-out, --autosave and --admin-password:
- chat reaches everyone, with the sender's name, Robo's messages tagged as an
  agent's; a joining editor gets what was said before; so does one after the
  server restarts;
- Ana saving the map leaves it in the maps folder (and a dated copy) and in
  --map-out, and tells the chat; an edit afterwards reaches --map-out by
  itself (autosave);
- loading a map for everyone needs the admin password: a wrong one leaves the
  session's map alone; the right one gives everyone the new map, keeps a copy
  of the session as it was, and the chat;
- the server Ana joined is in her recent servers.
"""
import glob
import os
import signal
import subprocess
import sys
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_multiplayer as base  # noqa: E402

OTHER_MAP = '''#include <a_samp>
#include <streamer>

public OnGameModeInit()
{
	CreateDynamicObject(1337, 1700.000000, -1700.000000, 13.500000, 0.000000, 0.000000, 0.000000);
	return 1;
}
'''


@unittest.skipUnless(base.BINARY and base.SERVER and base.XVFB and sys.platform.startswith('linux'),
                     'needs the editor, haumea-server and xvfb-run on Linux')
class ServerFeatureTests(base.EditorsMixin, unittest.TestCase):
    PASSWORD = 'crew pass 9'
    ADMIN = 'boss only'

    @classmethod
    def setUpClass(cls):
        cls.startEditors()
        cls.port = base.free_port()
        cls.maps = cls.tmp / 'maps'
        cls.out = cls.tmp / 'gamemodes' / 'map.pwn'
        cls.out.parent.mkdir()
        cls.state = cls.tmp / 'crew.hmps'
        cls.server = None
        cls.start_server()
        cls.ana = cls.editor('ana')
        cls.bia = cls.editor('bia')
        cls.robo = cls.editor('robo', extra_args=('--background',))

    @classmethod
    def start_server(cls):
        cls.log = open(cls.tmp / 'server.txt', 'a')
        cls.server = subprocess.Popen([base.SERVER, '--port', str(cls.port), '--password', cls.PASSWORD,
                                       '--state', str(cls.state), '--maps-dir', str(cls.maps),
                                       '--map-out', str(cls.out), '--autosave', '0.05',
                                       '--admin-password', cls.ADMIN, '--name', 'Crew'],
                                      stdout=cls.log, stderr=subprocess.STDOUT)
        base.wait_for(lambda: (cls.tmp / 'server.txt').read_text().count('listening') >= 1 and
                      cls.server.poll() is None, 'the server')
        time.sleep(0.3)

    @classmethod
    def stop_server(cls):
        if cls.server and cls.server.poll() is None:
            cls.server.send_signal(signal.SIGTERM)
            cls.server.wait(timeout=20)
        cls.log.close()

    @classmethod
    def tearDownClass(cls):
        cls.stop_server()
        super().tearDownClass()

    def join(self, client, name):
        self.assertTrue(client.command('multiplayer_join', '127.0.0.1', self.port, self.PASSWORD, name).get('ok'))
        base.wait_for(lambda: self.status(client)['state'] == 'joined', name + ' to join')

    def chat(self, client, since=0):
        return client.command('multiplayer_chat_read', since)['messages']

    def said(self, client, text):
        return [m for m in self.chat(client) if m['text'] == text]

    def test_server_features(self):
        ana, bia, robo = self.ana, self.bia, self.robo
        source = self.tmp / 'ana.pwn'
        source.write_text(base.FIXTURE.read_text())
        self.assertTrue(ana.command('map_open', str(source), 1).get('ok'))
        self.join(ana, 'Ana')
        self.join(bia, 'Bia')
        self.join(robo, 'Robo')

        # Robo is an agent's editor: everyone sees it so.
        base.wait_for(lambda: {p['name']: p['agent'] for p in self.status(ana)['peers']} == {'Bia': False, 'Robo': True},
                      'the agent tag')

        # Chat, with who said it.
        self.assertTrue(ana.command('multiplayer_chat', 'hello @Bia').get('ok'))
        self.assertTrue(robo.command('multiplayer_chat', 'parking lot done').get('ok'))
        base.wait_for(lambda: self.said(bia, 'hello @Bia') and self.said(bia, 'parking lot done'), 'the chat on Bia')
        hello = self.said(bia, 'hello @Bia')[0]
        robot = self.said(bia, 'parking lot done')[0]
        self.assertEqual((hello['name'], hello['agent'], hello['mine']), ('Ana', False, False))
        self.assertEqual((robot['name'], robot['agent']), ('Robo', True))
        self.assertTrue(self.said(ana, 'hello @Bia')[0]['mine'])
        # Reading on from the last one returns only what's new.
        last = bia.command('multiplayer_chat_read', 0)['last']
        self.assertEqual(self.chat(bia, last), [])

        # Ana saves: the server keeps it, a dated copy, the game server's file, and says so.
        saved = self.tmp / 'saved' / 'crewmap.pwn'
        saved.parent.mkdir()
        self.assertTrue(ana.command('save', str(saved)).get('ok'))
        base.wait_for(lambda: (self.maps / 'crewmap.pwn').exists() and self.out.exists(), 'the kept map')
        self.assertEqual((self.maps / 'crewmap.pwn').read_text(), saved.read_text())
        self.assertEqual(self.out.read_text(), saved.read_text())
        self.assertEqual(len(glob.glob(str(self.maps / 'history' / 'crewmap-*.pwn'))), 1)
        base.wait_for(lambda: any('Ana saved the map' in m['text'] for m in self.chat(bia)), 'the save in the chat')

        # An edit reaches the game server's file by itself.
        self.assertTrue(bia.command('session_begin').get('ok'))
        pickup = self.find(bia, 1600.0, -1900.0)
        self.assertTrue(bia.command('transform', pickup['instance_id'], 1633.25, -1901.5, 13.5, 0, 0).get('ok'))
        base.wait_for(lambda: '1633.25' in self.out.read_text(), 'the autosave in --map-out', timeout=40)
        self.assertTrue(bia.command('session_commit').get('ok'))

        # Loading another map for everyone: a wrong admin password changes nothing.
        other = self.tmp / 'other.pwn'
        other.write_text(OTHER_MAP)
        refused = bia.command('multiplayer_load_for_everyone', str(other), 'not it')
        self.assertTrue(refused.get('ok'), refused)
        base.wait_for(lambda: self.status(bia)['state'] == 'off', 'the refusal')
        self.assertIn('wrong admin password', self.status(bia)['status'])
        self.assertIsNone(self.find(ana, 1700.0, -1700.0))
        self.assertIsNotNone(self.find(ana, 1633.25, -1901.5))

        # The right one: everyone gets the new map, the old session is kept, the chat goes on.
        self.join(bia, 'Bia')
        self.assertTrue(bia.command('multiplayer_load_for_everyone', str(other), self.ADMIN).get('ok'))
        for client, who in ((ana, 'Ana'), (robo, 'Robo'), (bia, 'Bia')):
            base.wait_for(lambda: self.status(client)['state'] == 'joined' and self.find(client, 1700.0, -1700.0),
                          'the new map on ' + who)
            self.assertIsNone(self.find(client, 1633.25, -1901.5), who + ' still has the old map')
        self.assertEqual(len(glob.glob(str(self.state) + '.*.bak')), 1)
        base.wait_for(lambda: any('loaded another map for everyone' in m['text'] for m in self.chat(ana)),
                      'the replacement in the chat')
        self.assertTrue(self.said(ana, 'hello @Bia'), 'the chat went with the old map')
        edited = self.find(ana, 1700.0, -1700.0)
        self.assertTrue(ana.command('session_begin').get('ok'))
        self.assertTrue(ana.command('transform', edited['instance_id'], 1701.5, -1700.0, 13.5, 0, 0).get('ok'))
        base.wait_for(lambda: self.find(robo, 1701.5, -1700.0), 'an edit to the new map')
        self.assertTrue(ana.command('session_commit').get('ok'))

        # The chat outlives a restart of the server.
        for client in (ana, bia, robo):
            client.command('multiplayer_leave')
        self.stop_server()
        self.start_server()
        self.join(bia, 'Bia')
        self.assertTrue(self.said(bia, 'parking lot done'))
        self.assertIsNotNone(self.find(bia, 1701.5, -1700.0))

        # Ana's recent servers.
        servers = glob.glob(str(self.tmp / 'ana' / '**' / 'multiplayer_servers.txt'), recursive=True)
        self.assertTrue(servers, 'no server list')
        self.assertIn('127.0.0.1\t%d\tCrew' % self.port, Path(servers[0]).read_text())


if __name__ == '__main__':
    unittest.main()
