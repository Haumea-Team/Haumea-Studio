# SPDX-License-Identifier: GPL-2.0-or-later
"""Animated actors changed by someone else (multiplayer.cpp, anim.cpp).

One editor has an actor playing an animation; the other changes its skin over
and over, as people do while trying skins. Each change rebuilds the actor's
model in the first editor, and a new model often lands at the address of the
one just freed: the animation must notice the change anyway, not go on posing
the freed skeleton (SIGSEGV in rw::Frame::updateObjects, 1.0.0.342).
"""
import sys
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_multiplayer as base  # noqa: E402


@unittest.skipUnless(base.BINARY and base.XVFB and sys.platform.startswith('linux'), 'needs the editor and xvfb-run on Linux')
class AnimatedActorTests(base.EditorsMixin, unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.startEditors()
        cls.host = cls.editor('host')
        cls.guest = cls.editor('guest')

    def alive(self, client):
        return client.command('ping').get('ok')

    def test_skin_changes_while_animating(self):
        host, guest = self.host, self.guest
        source = self.tmp / 'every_element.pwn'
        source.write_text(base.FIXTURE.read_text())
        self.assertTrue(host.command('map_open', str(source), 1).get('ok'))
        started = host.command('multiplayer_host', 0, 'Ana', 0, 'local')
        self.assertTrue(started.get('ok'), started)
        self.assertTrue(guest.command('multiplayer_join', '127.0.0.1', started['port'], started['code'], 'Bia').get('ok'))
        base.wait_for(lambda: guest.command('multiplayer_status')['state'] == 'joined', 'the guest to join')
        # The host looks at the dancing actor, so it's drawn and posed every frame.
        host.command('camera', 1600.0, -1910.0, 16.0, 1605.0, -1900.0, 14.0, 70.0)
        actor = self.find(guest, 1605.0, -1900.0)
        self.assertEqual(actor['properties'].get('anim_name'), 'DEALER_IDLE')
        self.assertTrue(guest.command('session_begin').get('ok'))
        skins = [179, 180, 0, 7, 179, 23, 180, 46]
        anims = [('DEALER', 'DEALER_IDLE'), ('PED', 'IDLE_CHAT'), ('DANCING', 'DNCE_M_A'), ('PED', 'WALK_PLAYER')]
        for round_ in range(6):
            for i, skin in enumerate(skins):
                guest.command('properties', actor['instance_id'], 'skin=%d' % skin)
                lib, name = anims[i % len(anims)]
                guest.command('properties', actor['instance_id'], 'anim_lib=' + lib, 'anim_name=' + name)
                time.sleep(0.03)
            # Gone and back, as an undo brings it.
            guest.command('delete', actor['instance_id'])
            time.sleep(0.1)
            guest.command('undo')
            time.sleep(0.1)
            actor = self.find(guest, 1605.0, -1900.0) or actor
            self.assertTrue(self.alive(host), 'the host crashed in round %d' % round_)
        last = skins[-1]
        base.wait_for(lambda: (self.find(host, 1605.0, -1900.0) or {}).get('properties', {}).get('skin') == last,
                      'the last skin on the host')
        time.sleep(1)	# frames posing the last one
        self.assertTrue(self.alive(host))


if __name__ == '__main__':
    unittest.main()
