# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the UI scripts in ui/ through the editor's own input driver.

Each script (ui/*.uis) is played with --ui-script in its own Xvfb, against
the real game when HAUMEA_GAME_DIR names one (not a synthetic one), else against the synthetic
game from fixture/make_game.py; scripts with a "# needs: game" line only run
against the real game, and those with "# needs: server" only when PAWN_TOOLCHAIN
has open.mp's server (they get a free port in UI_SERVER_PORT; a server still
running from the test's folder afterwards fails it and is stopped). Every run
has its own config folder. A failing script prints its log.

Scripts run several at once (UI_JOBS, default CPUs/8, at least 1): when the module
is loaded as a whole (run_all.sh, discover, `-m unittest test_ui_scripts`),
every selected script starts on a worker pool and each test then takes its
own result; a single test named on the command line runs by itself. A summary
of the times follows the run.
"""
from pathlib import Path
import glob
import os
import shutil
import random
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, str(Path(__file__).resolve().parent))
from haumea_test_env import EditorTimeout, editor_env, is_real_game, run_editor

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
UI = HERE / 'ui'
BINARY = os.environ.get('HAUMEA_BINARY') or next(iter(sorted(
    glob.glob(str(ROOT / 'bin' / '*' / 'Release' / 'haumea')))), None)
XVFB = shutil.which('Xvfb')


def img_entries(path):
    with open(path, 'rb') as f:
        assert f.read(4) == b'VER2'
        count, = struct.unpack('<I', f.read(4))
        entries = {}
        for _ in range(count):
            offset, size, size2, name = struct.unpack('<IHH24s', f.read(32))
            entries[name.split(b'\0')[0].decode().lower()] = (offset * 2048, (size or size2) * 2048)
        return entries


def real_game():
    """HAUMEA_GAME_DIR when it is the real game: run_all.sh --fixture points it at the synthetic one."""
    game = os.environ.get('HAUMEA_GAME_DIR')
    try:
        return game if game and len(img_entries(Path(game, 'models', 'gta3.img'))) > 1000 else None
    except (OSError, AssertionError, struct.error):
        return None


REAL_GAME = real_game()


def server_package():
    """The Pawn toolchain's open.mp server, for scripts with "# needs: server"."""
    toolchain = os.environ.get('PAWN_TOOLCHAIN')
    package = Path(toolchain, 'omp', 'Server') if toolchain else None
    return package if package and (package / 'omp-server').is_file() else None


_ports_lock = threading.Lock()
_ports_given = set()


def free_port():
    """A free TCP port, never the same one twice in this run (scripts run at once)."""
    with _ports_lock:
        while True:
            with socket.socket() as s:
                s.bind(('127.0.0.1', 0))
                port = s.getsockname()[1]
            if port not in _ports_given:
                _ports_given.add(port)
                return port


def servers_in(folder):
    """PIDs of omp-servers running from inside folder (a test's own; never anyone else's)."""
    pids = []
    for proc in Path('/proc').glob('[0-9]*'):
        try:
            if proc.joinpath('comm').read_text().strip() == 'omp-server' and \
               Path(os.readlink(proc / 'cwd')).is_relative_to(folder):
                pids.append(int(proc.name))
        except OSError:
            pass
    return pids


def start_xvfb():
    """A private Xvfb and its DISPLAY. Not xvfb-run -a: its -displayfd takes no
    lock file, so two runs starting at once (parallel sessions) can pick the
    same display; the loser's Xvfb exits, its editor runs on the other test's
    server, and xvfb-run then fails a passing script. Here a random display is
    asked for by number, which X locks, and used only once its lock holds this
    Xvfb's PID and its socket is up."""
    for attempt in range(20):
        number = random.randint(400, 9999)
        lock, sock = Path(f'/tmp/.X{number}-lock'), Path(f'/tmp/.X11-unix/X{number}')
        if lock.exists() or sock.exists():
            continue
        proc = subprocess.Popen([XVFB, f':{number}', '-screen', '0', '1600x1000x24', '-nolisten', 'tcp'],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if proc.poll() is not None:
                break
            try:
                if int(lock.read_text().strip()) == proc.pid and sock.exists():
                    return proc, f':{number}'
            except (OSError, ValueError):
                pass
            time.sleep(0.1)
        proc.kill()
        proc.wait()
    raise RuntimeError('could not start Xvfb')


def needs(script):
    for line in script.read_text().splitlines():
        if line.startswith('# needs:'):
            return line.split(':', 1)[1].split()
    return []


_shared_lock = threading.Lock()
_shared = {}


def shared():
    """The run's temp folder and game folder (the synthetic one made once)."""
    with _shared_lock:
        if not _shared:
            tmp = tempfile.mkdtemp(prefix='haumea-ui-')
            game = REAL_GAME
            if not game:
                game = os.path.join(tmp, 'game')
                subprocess.run([sys.executable, str(HERE / 'fixture' / 'make_game.py'), game],
                               check=True, capture_output=True)
            _shared.update(tmp=tmp, game=game)
        return _shared['tmp'], _shared['game']


def run_script(name):
    """Play ui/<name> in its own Xvfb, config and workspace: {'skip': why}, or the
    editor's result, the test servers it left running (stopped) and its time.
    Safe to run several at once."""
    script = UI / name
    script = UI / name
    if 'game' in needs(script) and not REAL_GAME:
        return {'skip': 'needs the real game (HAUMEA_GAME_DIR)'}
    if 'server' in needs(script) and not server_package():
        return {'skip': 'needs the Pawn toolchain\'s open.mp server (PAWN_TOOLCHAIN)'}
    tmp, game = shared()
    started = time.monotonic()
    work = tempfile.mkdtemp(dir=tmp)
    env = editor_env(work, virtual_display=True)	# start_xvfb below
    # Scripts open the panels they use. Pin that starting state explicitly
    # instead of depending on the editor's defaults or one-time migrations.
    config = Path(env['XDG_CONFIG_HOME'], 'haumea')
    config.mkdir(parents=True, exist_ok=True)
    (config / 'savesettings.txt').write_text(
        'settings_version 8\nshow_browser_window 0\nshow_instance_window 0\n')
    # A copy, so scripts can save over the maps they open.
    shutil.copytree(UI / 'fixtures', Path(work, 'fixtures'))
    env['UI_FIXTURES'] = str(Path(work, 'fixtures'))
    server = Path(work, 'server')
    (server / 'gamemodes').mkdir(parents=True)
    (server / 'models').mkdir()
    env['SERVER'] = str(server)
    env['UI_SERVER_PORT'] = str(free_port())	# Test in game, beside other sessions' servers
    args = [BINARY, '--ui-script', str(script)]
    if name == 'first_run.uis':
        env.pop('HAUMEA_GAME_DIR', None)	# no game folder known
    else:
        args[1:1] = ['--game', game, '--workspace', str(server)]
    if name == 'terminal.uis':
        # A shell that starts plainly in an empty HOME (zsh opens its new-user wizard there).
        env['SHELL'] = shutil.which('bash') or '/bin/sh'
    if name in ('startscreen_launch.uis', 'rc_smoke.uis'):
        # Scripts skip it otherwise. It only shows the screen; settings still go to
        # editor_env's private config, like every script's.
        env['HAUMEA_START_SCREEN'] = '1'
    if name == 'namaka.uis':
        from test_namaka_install import make_bundle, python314
        bundle = make_bundle(Path(work, 'namaka'))
        if bundle is None:
            return {'skip': 'needs NAMAKA_WHEELHOUSE and CPython 3.14'}
        env['HAUMEA_NAMAKA_BUNDLE'] = str(bundle)
        env['HAUMEA_NAMAKA_PYTHON'] = python314()
    if name == 'custom_model.uis':
        entries = img_entries(Path(game, 'models', 'gta3.img'))
        with open(Path(game, 'models', 'gta3.img'), 'rb') as f:
            for source, target in (('barrel4.dff', 'ui_barrel.dff'), ('dynbarrels.txd', 'ui_barrel.txd')):
                offset, size = entries[source]
                f.seek(offset)
                Path(work, target).write_bytes(f.read(size))
        env['DFF'] = str(Path(work, 'ui_barrel.dff'))
        env['TXD'] = str(Path(work, 'ui_barrel.txd'))
    xvfb, env['DISPLAY'] = start_xvfb()
    env.pop('XAUTHORITY', None)
    try:
        # Scripts play many frames: longer than a load, still a hard limit.
        limit = 600 if '--game' in args and is_real_game(game) else 300
        result = run_editor(args, cwd=work, env=env, timeout=limit)
    except EditorTimeout as error:
        result = subprocess.CompletedProcess(args, -9, str(error), '')
    finally:
        xvfb.terminate()	# so it removes its lock and socket
        try:
            xvfb.wait(10)
        except subprocess.TimeoutExpired:
            xvfb.kill()
            xvfb.wait()
    left = servers_in(Path(work).resolve())
    for pid in left:
        os.kill(pid, signal.SIGKILL)
    return {'result': result, 'left': left, 'seconds': time.monotonic() - started}


_futures = {}	# script name -> the pool's run of it (ParallelUiSuite)
_times = {}


def ui_jobs():
    try:
        return max(1, int(os.environ['UI_JOBS']))
    except (KeyError, ValueError):
        return max(1, (os.cpu_count() or 8) // 8)


@unittest.skipUnless(BINARY and XVFB, 'needs the editor and Xvfb')
class UiScriptTests(unittest.TestCase):
    @classmethod
    def tearDownClass(cls):
        with _shared_lock:
            if _shared:
                shutil.rmtree(_shared['tmp'], ignore_errors=True)
                _shared.clear()

    def play(self, name):
        future = _futures.pop(name, None)
        outcome = future.result() if future else run_script(name)
        if 'skip' in outcome:
            self.skipTest(outcome['skip'])
        _times[name] = outcome['seconds']
        result, left = outcome['result'], outcome['left']
        log = '\n'.join(line for line in result.stdout.splitlines() if line.startswith('[ui-script]'))
        self.assertEqual(left, [], 'a test server was still running after the script')
        self.assertEqual(result.returncode, 0, log or result.stdout[-3000:] + result.stderr[-2000:])
        if sys.platform.startswith('linux') and 'GLFW platform:' in result.stdout:
            # The editor must have been in its Xvfb, not on the desktop's Wayland.
            self.assertIn('GLFW platform: X11', result.stdout)


# Scripts that take longest start first, so they don't finish last alone.
SLOW_FIRST = ['namaka.uis', 'workspace.uis', 'test_in_game.uis', 'ctrl_keys.uis', 'el_sound.uis']
_pool = None


def start_pool(suite):
    """Start every UI script in suite on a pool now: when the module is loaded
    with others (run_all.sh's discover), they run while the other modules'
    tests do."""
    global _pool
    def flatten(s):
        for test in s:
            if isinstance(test, unittest.TestSuite):
                yield from flatten(test)
            else:
                yield test
    names = [t._testMethodName[len('test_'):] + '.uis' for t in flatten(suite) if isinstance(t, UiScriptTests)]
    names.sort(key=lambda n: SLOW_FIRST.index(n) if n in SLOW_FIRST else len(SLOW_FIRST))
    jobs = ui_jobs()
    if _pool is None and BINARY and XVFB and jobs > 1 and len(names) > 1:
        _pool = ThreadPoolExecutor(jobs)
        for name in names:
            _futures[name] = _pool.submit(run_script, name)


class ParallelUiSuite(unittest.TestSuite):
    """Takes the pool's results; prints a summary of the times at the end."""

    def run(self, result, debug=False):
        global _pool
        started = time.monotonic()
        try:
            return super().run(result, debug)
        finally:
            jobs = ui_jobs() if _pool else 1
            if _pool:
                _pool.shutdown(wait=True, cancel_futures=True)
                _pool = None
            if _times:
                wall = time.monotonic() - started
                slow = sorted(_times.items(), key=lambda kv: -kv[1])[:3]
                print(f'\nUI scripts: {len(_times)} ran with {jobs} at once ({sum(_times.values()):.0f} s of scripts, '
                      f'{wall:.0f} s after the other tests); slowest: ' +
                      ', '.join(f'{n} {t:.0f} s' for n, t in slow), file=sys.stderr)
                _times.clear()


def load_tests(loader, tests, pattern):
    suite = ParallelUiSuite(tests)
    start_pool(suite)
    return suite


def add_script_test(path):
    def test(self):
        self.play(path.name)
    setattr(UiScriptTests, 'test_' + path.stem, test)


# UI_ONLY='a.uis b.uis' plays only those (run_all.sh --quick); unset or ALL, every one.
_only = os.environ.get('UI_ONLY', 'ALL').split()
for path in sorted(UI.glob('*.uis')):
    if 'ALL' in _only or path.name in _only:
        add_script_test(path)


if __name__ == '__main__':
    unittest.main()
