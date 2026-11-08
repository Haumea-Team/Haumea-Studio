# SPDX-License-Identifier: GPL-2.0-or-later
"""haumea-server with accounts, invites, roles, rooms and its identity (mprelay.cpp, mpaccounts.cpp).

The server's owner makes the first invite from the command line; Ana makes her
account with it and starts room "main". She invites Bia (a builder) and Cid (a
viewer) from inside Haumea. Then:
- Cid can look and chat, but an edit of his snaps back and nobody else gets it;
- a wrong password and a name nobody has are refused the same way;
- Bia, signed in without a room, gets the lobby's list; as a builder she can't make
  a room, as the admin Ana makes her (at once, while she's in) she can, and lock it;
- Ana removes Cid; an agent with Ana's token comes in tagged as hers, at most a
  builder, and not at all once she bars agents;
- everyone's ping is measured; the log has who came, the chat and every admin act;
- the server keeps it all over a restart, and an editor refuses a server whose
  identity changed since it first joined.
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

ADMIN_INVITE, ADMIN_SET_ROLE, ADMIN_KICK, ADMIN_LOCK, ADMIN_DELETE_ROOM, ADMIN_USERS, ADMIN_AGENTS = range(1, 8)


@unittest.skipUnless(base.BINARY and base.SERVER and base.XVFB and sys.platform.startswith('linux'),
                     'needs the editor, haumea-server and xvfb-run on Linux')
class AccountTests(base.EditorsMixin, unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.startEditors()
        cls.port = base.free_port()
        cls.state = cls.tmp / 'state'
        cls.server = None
        cls.start_server()
        cls.ana = cls.editor('ana')
        cls.bia = cls.editor('bia')
        cls.cid = cls.editor('cid')
        cls.robo = cls.editor('robo', extra_args=('--background',))

    @classmethod
    def cli(cls, *args):
        return subprocess.run([base.SERVER, '--state-dir', str(cls.state), *args], capture_output=True, text=True,
                              check=True).stdout

    @classmethod
    def start_server(cls):
        cls.log = open(cls.tmp / 'server.txt', 'a')
        cls.server = subprocess.Popen([base.SERVER, '--state-dir', str(cls.state), '--port', str(cls.port),
                                       '--name', 'Crew'], stdout=cls.log, stderr=subprocess.STDOUT)
        base.wait_for(lambda: (cls.tmp / 'server.txt').read_text().count('listening on port') >
                      getattr(cls, 'starts', 0), 'the server')
        cls.starts = getattr(cls, 'starts', 0) + 1

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

    def users(self):
        return dict(line.split() for line in self.cli('users').splitlines() if line.strip())

    def join(self, client, *fields, state='joined'):
        result = client.command('multiplayer_join', '127.0.0.1', self.port, *fields)
        self.assertTrue(result.get('ok'), result)
        return base.wait_for(lambda: (lambda s: s if s['state'] in (state, 'off') else None)(self.status(client)),
                             'the join')

    def admin(self, client, op, *args):
        seq = self.status(client)['admin_reply']['seq']
        self.assertTrue(client.command('multiplayer_admin', op, *args).get('ok'))
        return base.wait_for(lambda: (lambda r: r if r['seq'] > seq else None)(self.status(client)['admin_reply']),
                             'the admin reply')

    def test_accounts(self):
        ana, bia, cid, robo = self.ana, self.bia, self.cid, self.robo
        owner_code = self.cli('invite', '--role', 'owner').split(': ', 1)[1].split()[0]
        fingerprint = self.cli('fingerprint').strip()
        source = self.tmp / 'ana.pwn'
        source.write_text(base.FIXTURE.read_text())
        self.assertTrue(ana.command('map_open', str(source), 1).get('ok'))

        # Ana's account, from the owner's invite, starts room main with her map.
        s = self.join(ana, owner_code, 'ignored', 'invite', '', 'main', '', 'Ana', 'ana-pass-123')
        self.assertEqual(s['state'], 'joined', (s['status'], owner_code, (self.tmp / 'server.txt').read_text()[-800:]))
        self.assertEqual((s['state'], s['role'], s['room'], s['fingerprint']), ('joined', 'owner', 'main', fingerprint))
        self.assertEqual(self.users().get('Ana'), 'owner')
        # The invite is used up.
        s = self.join(bia, owner_code, 'x', 'invite', '', 'main', '', 'Mallory', 'mallory-pass')
        self.assertEqual(s['state'], 'off')
        self.assertIn('wrong password', s['status'])

        # Invites from inside Haumea: a builder and a viewer.
        builder = self.admin(ana, ADMIN_INVITE, 'builder', '1', '7')
        viewer = self.admin(ana, ADMIN_INVITE, 'viewer', '1', '7')
        self.assertTrue(builder['ok'] and viewer['ok'], (builder, viewer))
        s = self.join(bia, builder['data'], 'x', 'invite', '', 'main', '', 'Bia', 'bia-pass-123')
        self.assertEqual((s['state'], s['role']), ('joined', 'builder'))
        s = self.join(cid, viewer['data'], 'x', 'invite', '', 'main', '', 'Cid', 'cid-pass-123')
        self.assertEqual((s['state'], s['role']), ('joined', 'viewer'))
        self.assertIsNotNone(self.find(cid, 1600.0, -1900.0))
        base.wait_for(lambda: {p['name']: p['role'] for p in self.status(ana)['peers']} == {'Bia': 'builder', 'Cid': 'viewer'},
                      'the roles on Ana')

        # Cid (a viewer) moves the pickup: it snaps back, and nobody else gets it.
        self.assertTrue(cid.command('session_begin').get('ok'))
        pickup = self.find(cid, 1600.0, -1900.0)
        self.assertTrue(cid.command('transform', pickup['instance_id'], 1650.0, -1950.0, 13.5, 0, 0).get('ok'))
        base.wait_for(lambda: self.find(cid, 1600.0, -1900.0) and not self.find(cid, 1650.0, -1950.0), 'the snap back')
        self.assertIsNone(self.find(ana, 1650.0, -1950.0))
        self.assertIn('viewer', self.status(cid)['status'])
        cid.command('session_rollback')
        # He can chat.
        self.assertTrue(cid.command('multiplayer_chat', 'looks good').get('ok'))
        base.wait_for(lambda: any(m['text'] == 'looks good' for m in ana.command('multiplayer_chat_read', 0)['messages']),
                      'the viewer\'s chat')
        # Bia (a builder) edits for real.
        self.assertTrue(bia.command('session_begin').get('ok'))
        crate = self.find(bia, 1600.0, -1890.0)
        self.assertTrue(bia.command('transform', crate['instance_id'], 1611.0, -1890.0, 14.0, 0, 0).get('ok'))
        base.wait_for(lambda: self.find(ana, 1611.0, -1890.0), 'the builder\'s edit')
        bia.command('session_commit')

        # Wrong password, and a name nobody has: the same answer.
        bia.command('multiplayer_leave')
        wrong = self.join(bia, 'not-it-at-all', 'x', 'account', 'Bia', 'main')
        nobody = self.join(bia, 'whatever-123', 'x', 'account', 'Nobody', 'main')
        self.assertEqual((wrong['state'], nobody['state']), ('off', 'off'))
        self.assertIn('wrong password', wrong['status'])
        self.assertEqual(wrong['status'], nobody['status'])

        # Bia without a room: the lobby. A builder can't make one.
        s = self.join(bia, 'bia-pass-123', 'x', 'account', 'Bia', '', state='lobby')
        self.assertEqual(s['state'], 'lobby')
        self.assertEqual([(r['name'], r['people'], r['role']) for r in s['lobby']], [('main', 2, 'builder')])
        self.assertTrue(bia.command('multiplayer_enter', 'side').get('ok'))
        s = base.wait_for(lambda: (lambda s: s if s['state'] == 'lobby' and 'admin can make it' in s['status'] else None)(self.status(bia)),
                          'the refusal to make a room')
        # Ana makes her an admin while she's in: it takes at once.
        self.assertTrue(self.admin(ana, ADMIN_SET_ROLE, 'Bia', 'admin', '')['ok'])
        self.assertEqual(self.users().get('Bia'), 'admin')
        side = self.tmp / 'side.pwn'
        side.write_text(base.FIXTURE.read_text())
        self.assertTrue(bia.command('multiplayer_enter', 'side').get('ok'))
        s = base.wait_for(lambda: (lambda s: s if s['state'] == 'joined' else None)(self.status(bia)), 'room side')
        self.assertEqual((s['room'], s['role']), ('side', 'admin'))
        self.assertTrue(self.admin(bia, ADMIN_LOCK, 'secret-room')['ok'])

        # Cid, out of main, can't get into the locked room without its password.
        cid.command('multiplayer_leave')
        s = self.join(cid, 'cid-pass-123', 'x', 'account', 'Cid', 'side', 'wrong')
        self.assertEqual(s['state'], 'off')
        self.assertIn('locked', s['status'])
        s = self.join(cid, 'cid-pass-123', 'x', 'account', 'Cid', 'side', 'secret-room')
        self.assertEqual((s['state'], s['room'], s['role']), ('joined', 'side', 'viewer'))

        # Bia removes Cid from her room.
        cid_id = next(p['id'] for p in self.status(bia)['peers'] if p['name'] == 'Cid')
        self.assertTrue(self.admin(bia, ADMIN_KICK, str(cid_id))['ok'])
        s = base.wait_for(lambda: (lambda s: s if s['state'] == 'off' else None)(self.status(cid)), 'the removal')
        self.assertIn('removed by Bia', s['status'])

        # An agent with Ana's token: hers, tagged, at most a builder.
        token = base.wait_for(lambda: ana.command('multiplayer_agent_token').get('token'), 'the agent token')
        s = self.join(robo, token, 'Robo', 'agent', '', 'main')
        self.assertEqual((s['state'], s['role']), ('joined', 'builder'))
        robot = base.wait_for(lambda: next((p for p in self.status(ana)['peers'] if p['name'] == 'Robo'), None), 'Robo')
        self.assertEqual((robot['agent'], robot['owner'], robot['role']), (True, 'Ana', 'builder'))
        # Barred: an agent can't come in (Cid's editor tries, as one: Robo goes when it leaves a session).
        self.assertTrue(self.admin(ana, ADMIN_AGENTS, 'none')['ok'])
        s = self.join(cid, token, 'Robo2', 'agent', '', 'main')
        self.assertEqual(s['state'], 'off')
        self.assertIn('agents', s['status'])

        # Bia moves between rooms without signing in again: side to main, the lobby, a new empty room.
        self.assertTrue(bia.command('multiplayer_enter', 'main').get('ok'))
        s = base.wait_for(lambda: (lambda s: s if s['state'] == 'joined' and s['room'] == 'main' else None)(self.status(bia)),
                          'Bia in main')
        self.assertIsNotNone(self.find(bia, 1611.0, -1890.0), 'main\'s map')
        self.assertTrue(bia.command('multiplayer_enter', '').get('ok'))
        s = base.wait_for(lambda: (lambda s: s if s['state'] == 'lobby' else None)(self.status(bia)), 'the lobby')
        self.assertIn('main', [r['name'] for r in s['lobby']])
        self.assertTrue(bia.command('multiplayer_enter', 'scratch', '', 'empty').get('ok'))
        base.wait_for(lambda: (lambda s: s if s['state'] == 'joined' and s['room'] == 'scratch' else None)(self.status(bia)),
                      'room scratch')
        self.assertIsNone(self.find(bia, 1611.0, -1890.0), 'a new empty room has a map in it')
        self.assertTrue(bia.command('session_begin').get('ok'))
        self.assertTrue(bia.command('place_element', 'object', 1337, 1750.0, -1750.0, 13.5, 0, 0).get('ok'))
        self.assertTrue(bia.command('session_commit').get('ok'))
        base.wait_for(lambda: self.find(bia, 1750.0, -1750.0), 'the object in scratch')
        # A new empty map for everyone in the room.
        self.assertTrue(bia.command('multiplayer_load_for_everyone', '').get('ok'))
        base.wait_for(lambda: self.status(bia)['state'] == 'joined' and self.find(bia, 1750.0, -1750.0) is None,
                      'the empty map')
        self.assertEqual(self.status(bia)['room'], 'scratch')
        bia.command('multiplayer_enter', 'side')

        # Ping, measured both ways.
        base.wait_for(lambda: self.status(ana)['ping'] >= 0 and all(p['ping'] >= 0 for p in self.status(ana)['peers']),
                      'the pings')
        self.assertGreaterEqual(self.status(bia)['ping'], 0)

        # The log: who came, the chat, what admins did.
        log = (self.state / 'log.txt').read_text()
        for line in ('[Info] Ana joined room main', '[chat] [main] Cid: looks good', 'Ana (owner) made Bia a admin',
                     'Bia (admin) locked room side', 'Bia (admin) removed Cid from room side', 'refused 127.0.0.1',
                     'Bia (peer', 'left room side', 'Bia made room scratch'):
            self.assertIn(line.replace('a admin', 'a admin'), log)

        # A restart keeps the accounts, rooms and roles.
        for client in (ana, bia):
            client.command('multiplayer_leave')
        self.stop_server()
        self.start_server()
        s = self.join(bia, 'bia-pass-123', 'x', 'account', 'Bia', 'main')
        self.assertEqual((s['state'], s['role']), ('joined', 'admin'))
        self.assertIsNotNone(self.find(bia, 1611.0, -1890.0))
        bia.command('multiplayer_leave')

        # Another server answering at the same address: refused, as SSH would.
        self.stop_server()
        (self.state / 'identity.key').unlink()
        self.start_server()
        self.assertNotEqual(self.cli('fingerprint').strip(), fingerprint)
        s = self.join(bia, 'bia-pass-123', 'x', 'account', 'Bia', 'main')
        self.assertEqual(s['state'], 'off')
        self.assertIn("isn't the one you joined before", s['status'])


if __name__ == '__main__':
    unittest.main()
