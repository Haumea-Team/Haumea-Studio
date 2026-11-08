# SPDX-License-Identifier: GPL-2.0-or-later
"""The workspace "new" dots stay dismissed across restarts."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from haumea_test_env import editor_env, run_editor
from test_ui_scripts import BINARY, XVFB, start_xvfb

HERE = Path(__file__).resolve().parent


@unittest.skipUnless(BINARY and XVFB, 'needs the editor and Xvfb')
class WorkspaceHintTests(unittest.TestCase):
    def test_dismissed_hint_does_not_return_after_restart(self):
        with tempfile.TemporaryDirectory(prefix='haumea-workspace-hint-') as tmp:
            work = Path(tmp)
            game = work / 'game'
            subprocess.run([sys.executable, str(HERE / 'fixture/make_game.py'), str(game)],
                           check=True, capture_output=True)
            env = editor_env(work, virtual_display=True)
            xvfb, env['DISPLAY'] = start_xvfb()
            env.pop('XAUTHORITY', None)
            script = work / 'hint.uis'
            fixture = HERE / 'ui/fixtures/ui_map.pwn'

            def play(source):
                script.write_text(f'open "{fixture}"\nwait 10\n' + source)
                result = run_editor([BINARY, '--game', game, '--workspace', work,
                                     '--ui-script', script], cwd=work, env=env)
                self.assertEqual(result.returncode, 0, result.stdout)

            try:
                # Right-click hides a dot without opening its editor.
                play('''assert new_dot textdraws == 1
assert new_dot code == 1
assert work_area_top < 90
click_item '*' 'TextDraws' right
wait 3
assert new_dot textdraws == 0
assert new_dot code == 1
''')
                # Opening an editor hides its dot too.
                play('''assert new_dot textdraws == 0
assert new_dot code == 1
assert item_count 'TextDraws' == 1
click_item '*' 'Code'
wait 3
click_item '*' 'Map'
wait 3
assert new_dot textdraws == 0
assert new_dot code == 0
''')
                play("assert new_dot textdraws == 0\nassert new_dot code == 0\n")
            finally:
                xvfb.terminate()
                xvfb.wait(10)


if __name__ == '__main__':
    unittest.main()
