"""Crash and error reports: written at the crash, sent only as the user says.

Each case runs the editor on the synthetic game with its own config folder
and a short UI script. A crash is made with --crash-test (only with
HAUMEA_CRASH_TEST=1); the next start turns it into reports/pending/<id>.json,
which must pass the receiving server's own checks (haumea-telemetry's haumea_telemetry.py
validate_report and signature_of) and leave out the user's folders and map
names. Sending goes to HAUMEA_REPORTS_URL: a counting server, and once the real
receiving server with a private database.
"""
from pathlib import Path
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
# The receiving server is its own repo (haumea-telemetry): HAUMEA_TELEMETRY_DIR, or beside this checkout.
def _telemetry_dir():
    places = [Path(os.environ['HAUMEA_TELEMETRY_DIR'])] if os.environ.get('HAUMEA_TELEMETRY_DIR') else []
    places += [ROOT.parent / 'haumea-telemetry', ROOT.parent.parent / 'haumea-telemetry']
    return next((p for p in places if (p / 'haumea_telemetry.py').exists()), None)


TELEMETRY_DIR = _telemetry_dir()
from haumea_test_env import editor_env  # noqa: E402
if TELEMETRY_DIR:
    sys.path.insert(0, str(TELEMETRY_DIR))
    import haumea_telemetry  # noqa: E402
else:
    haumea_telemetry = None

BINARY = os.environ.get('HAUMEA_BINARY') or next(iter(sorted(
    glob.glob(str(ROOT / 'bin' / '*' / 'Release' / 'haumea')))), None)
XVFB = shutil.which('xvfb-run')


class CountingServer:
    def __init__(self, status=204):
        self.bodies = []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get('Content-Length', 0))
                outer.bodies.append(json.loads(self.rfile.read(length)))
                self.send_response(status)
                self.end_headers()

            def log_message(self, *args):
                pass

        self.httpd = http.server.HTTPServer(('127.0.0.1', 0), Handler)
        self.url = 'http://127.0.0.1:%d/haumea/reports' % self.httpd.server_port
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


@unittest.skipUnless(BINARY and XVFB and haumea_telemetry and sys.platform.startswith('linux'),
                     'needs the editor, xvfb-run on Linux and haumea-telemetry (its own repo) beside this checkout')
class CrashReportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix='hcr-'))
        cls.game = cls.tmp / 'game'
        subprocess.run([sys.executable, str(HERE / 'fixture' / 'make_game.py'), str(cls.game)],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.work = Path(tempfile.mkdtemp(dir=self.tmp))
        self.config = self.work / 'config'
        self.config.mkdir()
        self.reports = self.config / 'reports'

    def run_editor(self, *args, script='wait 30\n', url=None, crash_test=False, check=True):
        (self.work / 'run.uis').write_text(script)
        env = editor_env(str(self.work), virtual_display=True)
        env['HAUMEA_TEST_XVFB'] = '1'
        env['HAUMEA_CONFIG_DIR'] = str(self.config)
        env.pop('HAUMEA_NO_REPORTS', None)
        env.pop('HAUMEA_REPORTS_URL', None)
        if url:
            env['HAUMEA_REPORTS_URL'] = url
        if crash_test:
            env['HAUMEA_CRASH_TEST'] = '1'
        result = subprocess.run([XVFB, '-a', '-s', '-screen 0 1600x1000x24', BINARY, '--game', str(self.game),
                                 '--ui-script', str(self.work / 'run.uis'), *args],
                                cwd=self.work, env=env, capture_output=True, text=True, timeout=240)
        if check:
            self.assertEqual(result.returncode, 0, result.stdout[-3000:])
            self.assertNotIn('FAIL', result.stdout, result.stdout[-3000:])
        return result

    def pending(self):
        return sorted((self.reports / 'pending').glob('*.json')) if (self.reports / 'pending').is_dir() else []

    def crash(self, how='--crash-test'):
        result = self.run_editor(how, crash_test=True, check=False)
        self.assertNotEqual(result.returncode, 0, 'it did not crash:\n' + result.stdout[-2000:])
        raws = list(self.reports.glob('running-*.raw'))
        self.assertEqual(len(raws), 1, result.stdout[-2000:])
        return result

    def check_report(self, path, kind):
        text = path.read_text()
        self.assertLess(len(text), 256 * 1024)
        data = json.loads(text)
        r = haumea_telemetry.validate_report(data)	# raises on anything the server refuses
        self.assertEqual(haumea_telemetry.signature_of(r), r['signature'])
        self.assertEqual(r['kind'], kind)
        self.assertEqual(r['os'], 'linux')
        self.assertTrue(any(f['module'] == 'haumea' for f in r['frames']), r['frames'])
        if kind == 'crash':
            # The stack starts where it crashed: in haumea, or for abort() in the
            # C library's raise and abort, just above it.
            first = next(i for i, f in enumerate(r['frames']) if f['module'] == 'haumea')
            self.assertLessEqual(first, 3, r['frames'])
        self.assertNotIn('uid', data)	# the daily ping is off
        # Nothing that names the user or their folders.
        for secret in (str(self.game), str(self.work), str(self.config), os.environ.get('HOME', '/nonexistent')):
            self.assertNotIn(secret, text)
        return data

    def test_a_crash_becomes_a_report_at_the_next_start(self):
        self.crash()
        self.run_editor()
        self.assertEqual(list(self.reports.glob('running-*.raw')), [], 'the crash file is used up')
        reports = self.pending()
        self.assertEqual(len(reports), 1)
        data = self.check_report(reports[0], 'crash')
        self.assertEqual(data['crash']['signal'], 'SIGSEGV')
        self.assertEqual(data['frames'][0]['module'], 'haumea', 'the faulting frame comes first')
        self.assertTrue(any('crashing on purpose' in line and '<GTA>' in line for line in data['log']), data['log'][-5:])
        self.assertTrue(data['gl_renderer'], 'the renderer is named')
        self.assertTrue(data.get('build_id'), 'the build id is there')

    def test_abort_is_reported(self):
        self.crash('--crash-test=abort')
        self.run_editor()
        data = self.check_report(self.pending()[0], 'crash')
        self.assertEqual(data['crash']['signal'], 'SIGABRT')

    def test_crash_test_needs_the_variable(self):
        self.run_editor('--crash-test')	# runs to the end
        self.assertEqual(self.pending(), [])

    def test_a_clean_exit_leaves_nothing(self):
        self.run_editor()
        self.assertEqual(list(self.reports.glob('running-*.raw')), [])
        self.assertEqual(self.pending(), [])

    def test_errors_are_counted_in_one_report(self):
        self.run_editor('--crash-test=error', crash_test=True)
        reports = self.pending()
        self.assertEqual(len(reports), 1)
        data = self.check_report(reports[0], 'error')
        self.assertEqual(data['error']['count'], 2)
        self.assertIn('<GTA>', data['error']['message'])

    def test_never_deletes_and_sends_nothing(self):
        server = CountingServer()
        self.addCleanup(server.close)
        self.crash()
        self.reports.joinpath('settings.txt').write_text('send never\n')
        self.run_editor(url=server.url)
        self.assertEqual(self.pending(), [])
        self.assertEqual(server.bodies, [])

    def test_always_sends_and_deletes(self):
        server = CountingServer()
        self.addCleanup(server.close)
        self.crash()
        self.reports.joinpath('settings.txt').write_text('send always\n')
        self.run_editor(url=server.url, script='wait 60\n')
        self.assertEqual(len(server.bodies), 1)
        haumea_telemetry.validate_report(server.bodies[0])
        self.assertEqual(self.pending(), [])

    def test_ask_banner_send(self):
        server = CountingServer()
        self.addCleanup(server.close)
        self.crash()
        self.run_editor(url=server.url, script=("wait 10\n"
                                                "assert window \"Send a report?\"\n"
                                                "click_item '*' 'Send'\n"
                                                "wait 60\n"))
        self.assertEqual(len(server.bodies), 1)
        self.assertEqual(self.pending(), [])

    def test_ask_banner_dont_send(self):
        server = CountingServer()
        self.addCleanup(server.close)
        self.crash()
        self.run_editor(url=server.url, script=("wait 10\n"
                                                "click_item '*' 'Don\\'t send'\n"
                                                "wait 10\n"
                                                "assert no_window \"Send a report?\"\n"))
        self.assertEqual(server.bodies, [])
        self.assertEqual(self.pending(), [])

    def test_no_banner_or_sending_in_a_test_run(self):
        # Without a test server, an automated run neither asks nor sends.
        self.crash()
        self.reports.joinpath('settings.txt').write_text('send always\n')
        self.run_editor(script='wait 10\nassert no_window "Send a report?"\n')
        self.assertEqual(len(self.pending()), 1)

    def test_refused_report_is_kept(self):
        server = CountingServer(status=500)
        self.addCleanup(server.close)
        self.crash()
        self.reports.joinpath('settings.txt').write_text('send always\n')
        self.run_editor(url=server.url, script='wait 60\n')
        self.assertEqual(len(server.bodies), 1)
        self.assertEqual(len(self.pending()), 1)

    def test_old_reports_go_after_30_days(self):
        self.crash()
        self.run_editor()
        report = self.pending()[0]
        old = time.time() - 31 * 24 * 3600
        os.utime(report, (old, old))
        self.run_editor()
        self.assertEqual(self.pending(), [])

    def test_install_id_only_with_the_ping_on(self):
        server = CountingServer()
        self.addCleanup(server.close)
        self.crash()
        self.reports.joinpath('settings.txt').write_text('send always\n')
        (self.config / 'telemetry.txt').write_text('consent yes\nuid ' + 'ab' * 16 + '\n')
        self.run_editor(url=server.url, script='wait 60\n')
        self.assertEqual(server.bodies[0].get('uid'), 'ab' * 16)

    def test_real_server_takes_the_report(self):
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            port = s.getsockname()[1]
        db = self.work / 'server.db'
        env = dict(os.environ, HAUMEA_LISTEN='127.0.0.1:%d' % port, HAUMEA_DB=str(db), HAUMEA_FETCH_SYMBOLS='0',
                   HAUMEA_TRUST_PROXY='0', HAUMEA_SYMBOLS_DIR=str(self.work / 'symbols'))
        server = subprocess.Popen([sys.executable, str(TELEMETRY_DIR / 'haumea_telemetry.py'),
                                   '--config', os.devnull], env=env, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        self.addCleanup(server.wait)
        self.addCleanup(server.kill)
        end = time.time() + 30
        while time.time() < end:
            try:
                socket.create_connection(('127.0.0.1', port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.2)
        self.crash()
        self.reports.joinpath('settings.txt').write_text('send always\n')
        result = self.run_editor(url='http://127.0.0.1:%d/haumea/reports' % port, script='wait 60\n')
        self.assertIn('sent 1 report', result.stdout)
        connection = sqlite3.connect(str(db))
        rows = connection.execute('SELECT kind, os FROM reports').fetchall()
        connection.close()
        self.assertEqual(rows, [('crash', 'linux')])


if __name__ == '__main__':
    unittest.main()
