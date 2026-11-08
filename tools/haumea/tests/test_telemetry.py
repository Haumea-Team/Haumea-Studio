"""The daily ping: opt-in, once a day, only {uid, version, channel, os}.

Each case starts the editor on the synthetic game with a short UI script and
its own config folder, and points it with HAUMEA_TELEMETRY_URL at a server on
127.0.0.1 (which also lets this automated run send). A counting server records
every request; one case uses the real receiving server, haumea-telemetry's haumea_telemetry.py,
with a private database, as deployed.
- Never asked, or asked and no: nothing is sent and no ID is made.
- Yes: one POST of exactly the four fields; the ID and the day are kept; a
  second start the same day sends nothing.
- HAUMEA_NO_TELEMETRY: nothing, even with yes.
- A refused ping (HTTP 500) isn't marked sent and isn't retried.
- The start screen's Yes sends at once; leaving the question unanswered
  is no; Preferences > Privacy off deletes the ID.
- The real server takes the ping (204) and stores the install.
"""
from pathlib import Path
import datetime
import glob
import http.server
import json
import os
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
from haumea_test_env import editor_env  # noqa: E402

BINARY = os.environ.get('HAUMEA_BINARY') or next(iter(sorted(
    glob.glob(str(ROOT / 'bin' / '*' / 'Release' / 'haumea')))), None)
XVFB = shutil.which('xvfb-run')
# The receiving server is its own repo (haumea-telemetry): HAUMEA_TELEMETRY_DIR, or beside this checkout.
def _telemetry_dir():
    places = [Path(os.environ['HAUMEA_TELEMETRY_DIR'])] if os.environ.get('HAUMEA_TELEMETRY_DIR') else []
    places += [ROOT.parent / 'haumea-telemetry', ROOT.parent.parent / 'haumea-telemetry']
    return next((p for p in places if (p / 'haumea_telemetry.py').exists()), None)


TELEMETRY_DIR = _telemetry_dir()
SERVER = TELEMETRY_DIR / 'haumea_telemetry.py' if TELEMETRY_DIR else None


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class CountingServer:
    """Answers every POST with status and keeps the bodies."""

    def __init__(self, status=204):
        self.bodies = []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get('Content-Length', 0))
                outer.bodies.append((self.path, self.headers.get('Content-Type'), self.rfile.read(length)))
                self.send_response(status)
                self.end_headers()

            def log_message(self, *args):
                pass

        self.httpd = http.server.HTTPServer(('127.0.0.1', 0), Handler)
        self.url = 'http://127.0.0.1:%d/haumea/telemetry/ping' % self.httpd.server_port
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


@unittest.skipUnless(BINARY and XVFB and sys.platform.startswith('linux'), 'needs the editor and xvfb-run on Linux')
class TelemetryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='htl-'))
        cls.game = cls.tmp / 'game'
        subprocess.run([sys.executable, str(HERE / 'fixture' / 'make_game.py'), str(cls.game)],
                       check=True, capture_output=True)
        shutil.copy(HERE / 'ui' / 'fixtures' / 'ui_map.pwn', cls.tmp / 'map.pwn')

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.work = Path(tempfile.mkdtemp(dir=self.tmp))
        self.config = self.work / 'config'
        self.config.mkdir()

    def state(self, text=None):
        """telemetry.txt: writes it, or reads it as a dict ({} when there is none)."""
        path = self.config / 'telemetry.txt'
        if text is not None:
            path.write_text(text)
            return None
        if not path.exists():
            return {}
        return dict(line.split(' ', 1) for line in path.read_text().splitlines() if ' ' in line)

    def run_editor(self, url, script='wait 60\n', start_screen=False, no_telemetry=False):
        (self.work / 'run.uis').write_text(script)
        env = editor_env(str(self.work), virtual_display=True)
        env['HAUMEA_TEST_XVFB'] = '1'
        env['HAUMEA_CONFIG_DIR'] = str(self.config)
        env['HAUMEA_TELEMETRY_URL'] = url
        env['CAMERA_MAP'] = str(self.tmp / 'map.pwn')
        env.pop('HAUMEA_NO_TELEMETRY', None)
        if no_telemetry:
            env['HAUMEA_NO_TELEMETRY'] = '1'
        if start_screen:
            env['HAUMEA_START_SCREEN'] = '1'
        result = subprocess.run([XVFB, '-a', '-s', '-screen 0 1600x1000x24', BINARY, '--game', str(self.game),
                                 '--ui-script', str(self.work / 'run.uis')],
                                cwd=self.work, env=env, capture_output=True, text=True, timeout=240)
        self.assertNotIn('FAIL', result.stdout, result.stdout[-3000:])
        return result.stdout

    def test_not_asked_sends_nothing(self):
        server = CountingServer()
        self.addCleanup(server.close)
        self.run_editor(server.url)
        self.assertEqual(server.bodies, [])
        self.assertEqual(self.state(), {}, 'nothing is written before an answer')

    def test_no_sends_nothing(self):
        server = CountingServer()
        self.addCleanup(server.close)
        self.state('consent no\n')
        self.run_editor(server.url)
        self.assertEqual(server.bodies, [])
        self.assertNotIn('uid', self.state())

    def test_yes_sends_once_a_day(self):
        server = CountingServer()
        self.addCleanup(server.close)
        self.state('consent yes\n')
        out = self.run_editor(server.url)
        self.assertEqual(len(server.bodies), 1, out[-2000:])
        path, content_type, body = server.bodies[0]
        self.assertEqual(path, '/haumea/telemetry/ping')
        self.assertEqual(content_type, 'application/json')
        ping = json.loads(body)
        self.assertEqual(set(ping), {'uid', 'version', 'channel', 'os'})
        self.assertRegex(ping['uid'], r'^[0-9a-f]{32}$')
        self.assertRegex(ping['version'], r'^\d+\.\d+\.\d+\.\d+$')
        self.assertIn(ping['channel'], ('production', 'beta', 'dev', 'pr'))
        self.assertEqual(ping['os'], 'linux')
        state = self.state()
        self.assertEqual(state.get('uid'), ping['uid'])
        self.assertEqual(state.get('last_sent'), datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%d'))
        self.assertIn('Telemetry: sent today', out)
        # The same day again: nothing.
        self.run_editor(server.url)
        self.assertEqual(len(server.bodies), 1)

    def test_no_telemetry_env_wins(self):
        server = CountingServer()
        self.addCleanup(server.close)
        self.state('consent yes\n')
        self.run_editor(server.url, no_telemetry=True)
        self.assertEqual(server.bodies, [])

    def test_refused_ping_is_not_marked_or_retried(self):
        server = CountingServer(status=500)
        self.addCleanup(server.close)
        self.state('consent yes\n')
        out = self.run_editor(server.url, script='wait 120\n')
        self.assertEqual(len(server.bodies), 1)
        self.assertNotIn('last_sent', self.state())
        self.assertIn("wasn't taken (HTTP 500)", out)

    def test_https_only(self):
        # A non-loopback http address is refused, test URL or not.
        self.state('consent yes\n')
        out = self.run_editor('http://example.invalid/haumea/telemetry/ping')
        self.assertIn('refusing', out)
        self.assertNotIn('last_sent', self.state())

    def test_start_screen_yes_sends(self):
        server = CountingServer()
        self.addCleanup(server.close)
        script = ('wait 10\n'
                  "click_item '*' 'Yes, send it'\n"
                  'wait 60\n')
        self.run_editor(server.url, script, start_screen=True)
        self.assertEqual(len(server.bodies), 1)
        self.assertEqual(self.state().get('consent'), 'yes')

    def test_start_screen_unanswered_is_no(self):
        server = CountingServer()
        self.addCleanup(server.close)
        script = ('wait 10\n'
                  "assert item '*' 'Yes, send it'\n"
                  "click_item '*' 'Continue without workspace'\n"
                  'wait 20\n')
        self.run_editor(server.url, script, start_screen=True)
        self.assertEqual(server.bodies, [])
        self.assertEqual(self.state(), {'consent': 'no'})

    def test_preferences_off_deletes_the_id(self):
        server = CountingServer()
        self.addCleanup(server.close)
        self.state('consent yes\n')
        script = ('wait 30\n'
                  'shortcut window_preferences\n'
                  'wait 5\n'
                  "click_item 'Preferences' 'Privacy'\n"
                  'wait 5\n'
                  "click_item 'Preferences' 'Send a daily usage ping'\n"
                  'wait 10\n')
        self.run_editor(server.url, script)
        self.assertEqual(len(server.bodies), 1)	# sent at start, while it was on
        self.assertEqual(self.state(), {'consent': 'no'})

    @unittest.skipUnless(SERVER, 'needs haumea-telemetry (its own repo) beside this checkout')
    def test_real_server_takes_the_ping(self):
        port = free_port()
        db = self.work / 'server.db'
        env = dict(os.environ, HAUMEA_LISTEN='127.0.0.1:%d' % port, HAUMEA_DB=str(db),
                   HAUMEA_FETCH_SYMBOLS='0', HAUMEA_TRUST_PROXY='0', HAUMEA_SYMBOLS_DIR=str(self.work / 'symbols'))
        server = subprocess.Popen([sys.executable, str(SERVER), '--config', os.devnull], env=env,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.addCleanup(server.wait)
        self.addCleanup(server.kill)
        end = time.time() + 30
        while time.time() < end:
            try:
                socket.create_connection(('127.0.0.1', port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.2)
        self.state('consent yes\n')
        out = self.run_editor('http://127.0.0.1:%d/haumea/telemetry/ping' % port)
        self.assertIn('Telemetry: sent today', out)
        uid = self.state()['uid']
        connection = sqlite3.connect(str(db))
        rows = connection.execute('SELECT uid, os FROM installs').fetchall()
        connection.close()
        self.assertEqual(rows, [(uid, 'linux')])


if __name__ == '__main__':
    unittest.main()
