# SPDX-License-Identifier: GPL-2.0-or-later
"""Several Namakas on one map, none of them on the user's screen.

Each worker is a hidden Haumea (--background) with a private config folder,
joined to the multiplayer session of the user's editor as a person of its own:
its own camera, edit session and undo. The user sees its edits arrive and its
marker fly around, and their own camera and selection are never touched.

When the user's editor is in no session, it hosts one for this computer only
(loopback, nothing asked of the router).
"""

from __future__ import annotations

import atexit
import os
from pathlib import Path
import re
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

if __package__:
	from .haumea_ipc import HaumeaError
	from .service import HaumeaService
else:
	from haumea_ipc import HaumeaError
	from service import HaumeaService

MAX_WORKERS = 8	# each is a whole editor with the game loaded
START_TIMEOUT = 180.0
JOIN_TIMEOUT = 60.0
# Agents can't do these to the session from a worker: they would take it from everyone.
OWNER_ONLY_TOOLS = {"open_map", "new_map", "merge_map", "multiplayer_session", "test_in_game", "namaka_team",
                    "textdraw_file"}
# With no namaka given, these still act on the user's editor: its file, its view and its session.
EDITOR_TOOLS = OWNER_ONLY_TOOLS | {"haumea_status", "save_map", "move_camera", "camera_back"}
# These don't look at the scene, so they need no worker (and use one only if it's up).
OFFLINE_TOOLS = {"search_assets", "inspect_asset", "render_asset_views", "similar_assets", "start_asset_discovery",
                 "browse_asset_families", "discovery_candidates", "open_asset_family", "mark_discovery_assets",
                 "discover_assets", "asset_passport", "annotate_asset_passport", "search_annotated_assets",
                 "search_creative_catalogue", "propose_asset_palette", "render_palette_board",
                 "inspect_placement_anchors", "list_composition_recipes", "search_textures", "filter_assets",
                 "get_mapping_project", "update_mapping_project", "record_mapping_review", "list_prefabs"}
DEFAULT_WORKER = "Namaka"
USER_EDITOR = "editor"	# namaka="editor": on the user's screen, when they ask to watch
SETTLE_TIMEOUT = 10.0


def _resident_mb(pid: int) -> int | None:
	try:
		for line in Path(f"/proc/{pid}/status").read_text().splitlines():
			if line.startswith("VmRSS:"):
				return int(line.split()[1]) // 1024
	except (OSError, ValueError, IndexError):
		pass
	return None


class Worker:
	def __init__(self, name: str, process: subprocess.Popen, service: HaumeaService, folder: Path, log):
		self.name = name
		self.process = process
		self.service = service
		self.folder = folder
		self.log = log
		service.is_worker = True

	def summary(self) -> dict:
		running = self.process.poll() is None
		out = {"name": self.name, "running": running}
		if running:
			if (memory := _resident_mb(self.process.pid)) is not None:
				out["memory_mb"] = memory
		else:
			out["exit_code"] = self.process.returncode
		return out

	def stop(self) -> None:
		if self.process.poll() is None:
			try:
				self.service.client.command("multiplayer_leave")	# it quits by itself after this
				self.process.wait(timeout=10)
			except (OSError, HaumeaError, subprocess.TimeoutExpired):
				self.process.terminate()
				try:
					self.process.wait(timeout=10)
				except subprocess.TimeoutExpired:
					self.process.kill()
					self.process.wait()
		self.log.close()
		shutil.rmtree(self.folder, ignore_errors=True)


class NamakaTeam:
	def __init__(self, owner: HaumeaService):
		self.owner = owner
		self.workers: dict[str, Worker] = {}
		self._lock = threading.Lock()
		self._default_lock = threading.Lock()
		self._default_started = False
		self._can_hide = os.environ.get("HAUMEA_NAMAKA_BACKGROUND", "1") != "0"
		atexit.register(self.stop_all)

	def service_for(self, name: str) -> HaumeaService:
		worker = self.workers.get(name)
		if worker is None:
			raise ValueError(f"no Namaka called {name!r}: namaka_team(action='spawn') starts them; "
			                 f"running: {sorted(self.workers) or 'none'}")
		if worker.process.poll() is not None:
			raise ValueError(f"Namaka {name!r} has stopped (exit code {worker.process.returncode}); "
			                 "its finished work is still in the session")
		return worker.service

	def default_running(self) -> HaumeaService | None:
		worker = self.workers.get(DEFAULT_WORKER)
		return worker.service if worker and worker.process.poll() is None else None

	def default_service(self) -> HaumeaService:
		"""The hidden Namaka tools work in unless told otherwise, started on first need.

		Where one can't run, the user's editor, as before workers existed."""
		if not self._can_hide:
			return self.owner
		with self._default_lock:
			running = self.default_running()
			if running:
				return running
			if self._default_started:
				# It closed. Its instance ids meant nothing to the next one, so the caller must look again.
				self._default_started = False
				raise HaumeaError("the background Namaka closed after sitting idle, so instance ids from before no "
				                  "longer apply: list the scene again and retry. What it built is still in the map.")
			try:
				info = self.owner.client.command("editor_info")
				if not info.get("can_hide_window") or info.get("background"):
					self._can_hide = False
					return self.owner
				self.spawn([DEFAULT_WORKER])
			except (OSError, ValueError, HaumeaError) as error:
				print(f"Namaka: working in the user's editor: {error}", file=sys.stderr, flush=True)
				if "unknown command" in str(error):
					self._can_hide = False
				return self.owner
			self._default_started = True
			return self.workers[DEFAULT_WORKER].service

	def _scene_fingerprint(self, service: HaumeaService) -> list:
		return sorted((str(item.get("kind")), item.get("model_id") or 0, *[round(v, 2) for v in item["position"]])
		              for item in service.enumerate_scene())

	def settle(self, commit: bool = True) -> str | None:
		"""Before the user's editor writes or runs the map: what the workers did has reached it."""
		workers = [w for w in self.workers.values() if w.process.poll() is None]
		for worker in workers:
			if commit and worker.name == DEFAULT_WORKER and worker.service.client.command("session_status").get("active"):
				worker.service.client.command("session_commit")
		deadline = time.time() + SETTLE_TIMEOUT
		for worker in workers:
			while self._scene_fingerprint(worker.service) != self._scene_fingerprint(self.owner):
				if time.time() > deadline:
					return (f"{worker.name}'s latest edits hadn't reached the editor after {SETTLE_TIMEOUT:.0f} s "
					        "(is someone still editing?): this may not have them")
				time.sleep(0.1)
		return None

	def end_default_session(self, discard_changes: bool) -> None:
		"""Opening or starting a map ends the hidden Namaka's edit session, as it did the editor's."""
		service = self.default_running()
		if service and service.client.command("session_status").get("active"):
			if not discard_changes:
				raise ValueError("an edit session has unsaved edits: save_map to keep them, or pass "
				                 "discard_changes=True to drop them")
			service.client.command("session_rollback")
			self.settle(commit=False)

	def _session(self, address: str | None, password: str | None) -> tuple:
		"""Where workers join: the session the user's editor is in (it says how), or a new one."""
		status = self.owner.client.command("multiplayer_status")
		if status["state"] == "joined":
			info = self.owner.client.command("multiplayer_join_info")
			if info.get("accounts"):
				# A token from the user's account: their password never comes here.
				deadline = time.time() + 20
				while True:
					token = self.owner.client.command("multiplayer_agent_token").get("token")
					if token:
						return info["address"], int(info["port"]), token, "agent", info.get("room", ""), ""
					if time.time() > deadline:
						raise HaumeaError("the server didn't give the agents a token")
					time.sleep(0.2)
			if info.get("code"):
				return info["address"], int(info["port"]), info["code"], "code", "", info.get("name", "")
		if status["state"] == "lobby":
			raise ValueError("the editor is in a server's lobby: pick a room first")
		if status["state"] == "off":
			self.owner._close_session_for_document(False)
			# The user's name, as the Multiplayer panel suggests it.
			who = "".join(c for c in (os.environ.get("USER") or os.environ.get("USERNAME") or "Mapper")
			              if c.isprintable() and c not in "\t\n")[:31] or "Mapper"
			status = self.owner.client.command("multiplayer_host", 0, who, 0, "local")
		if status["state"] == "connecting" and status.get("code"):
			deadline = time.time() + JOIN_TIMEOUT
			while status["state"] != "joined" and time.time() < deadline:
				time.sleep(0.1)
				status = self.owner.client.command("multiplayer_status")
		if status.get("code"):	# hosting here
			info = self.owner.client.command("multiplayer_join_info")
			return "127.0.0.1", int(status["port"]), status["code"], "code", "", info.get("name", "")
		if status["state"] == "joined" and address and password:
			return address, int(status["port"]), password
		raise ValueError("the editor's session didn't say how to join it: pass the address and password")

	def _launch(self, name: str, info: dict) -> Worker:
		folder = Path(tempfile.mkdtemp(prefix="haumea-namaka-"))
		env = dict(os.environ, HAUMEA_CONFIG_DIR=str(folder / "config"))
		(folder / "config").mkdir()
		# The user's prefabs are there for it, and what it saves goes to them.
		prefabs = Path(info.get("data_dir") or "") / "prefabs"
		if info.get("data_dir"):
			prefabs.mkdir(parents=True, exist_ok=True)	# a first prefab has to land there too
			try:
				(folder / "config" / "prefabs").symlink_to(prefabs, target_is_directory=True)
			except OSError:
				shutil.copytree(prefabs, folder / "config" / "prefabs")
		# The default one keeps the project's notes where the agent always had them.
		state = self.owner.state_dir if name == DEFAULT_WORKER else self.owner.state_dir / "team" / name
		service = HaumeaService(folder / "s", self.owner.database, self.owner.client.timeout,
		                        self.owner.discovery_dir, state)
		client = service.client
		client.agent = "namaka-" + name
		client.automatic = False
		arguments = [info["executable"], "--background", "--game", info["game"]]
		if os.name == "nt":
			with socket.socket() as reserve:	# Windows: loopback TCP with a token of its own
				reserve.bind(("127.0.0.1", 0))
				port = reserve.getsockname()[1]
			client.tcp_port, client.token = str(port), secrets.token_hex(32)
			env.update(HAUMEA_ENGINE_TCP_PORT=client.tcp_port, HAUMEA_ENGINE_TOKEN=client.token)
		else:
			client.tcp_port, client.token = None, ""
			env.pop("HAUMEA_ENGINE_TCP_PORT", None)
			env.pop("HAUMEA_ENGINE_TOKEN", None)
			arguments += ["--agent-socket", str(client.engine_socket)]
		log = open(folder / "out.txt", "w")
		process = subprocess.Popen(arguments, cwd=folder, env=env, stdin=subprocess.DEVNULL, stdout=log,
		                           stderr=subprocess.STDOUT)
		return Worker(name, process, service, folder, log)

	def _wait_joined(self, worker: Worker, address: str, port: int, password: str, kind: str = "code",
	                 room: str = "", owner: str = "") -> None:
		client = worker.service.client
		deadline = time.time() + START_TIMEOUT
		while True:
			if worker.process.poll() is not None:
				tail = (worker.folder / "out.txt").read_text(errors="replace")[-600:]
				raise HaumeaError(f"Namaka {worker.name} didn't start (exit code {worker.process.returncode}): {tail}")
			try:
				if client.command("ping").get("ok"):
					break
			except (OSError, HaumeaError):
				pass
			if time.time() > deadline:
				raise HaumeaError(f"Namaka {worker.name} didn't start in {START_TIMEOUT:.0f} s")
			time.sleep(0.25)
		# Whose agent it is goes along: the others see "via <name>".
		client.command("multiplayer_join", address, port, password, worker.name, kind, "", room, "", "", "", owner)
		deadline = time.time() + JOIN_TIMEOUT
		while True:
			status = client.command("multiplayer_status")
			if status["state"] == "joined":
				# It starts looking where the user is looking.
				camera = self.owner.client.command("camera_context")["camera"]
				client.command("camera", *camera["position"], *camera["target"], camera["fov"])
				return
			if status["state"] == "off" or time.time() > deadline:
				raise HaumeaError(f"Namaka {worker.name} couldn't join: {status.get('status') or status['state']}")
			time.sleep(0.1)

	def spawn(self, names: list[str], address: str | None = None, password: str | None = None) -> dict:
		for name in names:
			if not re.fullmatch(r"[A-Za-z0-9_-]{1,24}", name):
				raise ValueError(f"a Namaka's name is 1-24 letters, digits, - or _: {name!r}")
		if len(set(names)) != len(names):
			raise ValueError("the names must differ")
		with self._lock:
			self._forget_stopped()
			taken = [name for name in names if name in self.workers]
			if taken:
				raise ValueError(f"already running: {taken}")
			if len(self.workers) + len(names) > MAX_WORKERS:
				raise ValueError(f"at most {MAX_WORKERS} Namakas run at once, and each loads the whole game")
			info = self.owner.client.command("editor_info")
			if info.get("background"):
				raise ValueError("a worker can't start workers")
			if not info.get("can_hide_window"):
				raise ValueError("background Namakas need the OpenGL build of Haumea: this Direct3D 9 one can't hide "
				                 "their windows yet (Preferences > Renderer)")
			if not info.get("executable") or not info.get("game"):
				raise ValueError("the editor has no game folder yet")
			target = self._session(address, password)
			started: list[Worker] = []
			errors: list[str] = []
			threads = []
			try:
				for name in names:
					started.append(self._launch(name, info))
				def join(worker):
					try:
						self._wait_joined(worker, *target)
					except (OSError, HaumeaError) as error:
						errors.append(str(error))
				threads = [threading.Thread(target=join, args=(worker,)) for worker in started]
				for thread in threads:
					thread.start()
				for thread in threads:
					thread.join()
				if errors:
					raise HaumeaError("; ".join(errors))
			except BaseException:
				for worker in started:
					worker.stop()
				raise
			for worker in started:
				self.workers[worker.name] = worker
			return self.status()

	def _forget_stopped(self) -> None:
		for name in [n for n, w in self.workers.items() if w.process.poll() is not None]:
			self.workers.pop(name).stop()

	def status(self) -> dict:
		return {"namakas": [worker.summary() for worker in self.workers.values()],
		        "session": self.owner.client.command("multiplayer_status")}

	def stop(self, names: list[str] | None = None) -> dict:
		with self._lock:
			for name in (names if names is not None else list(self.workers)):
				worker = self.workers.pop(name, None)
				if worker is None:
					raise ValueError(f"no Namaka called {name!r}")
				worker.stop()
				if name == DEFAULT_WORKER:
					self._default_started = False	# stopped on purpose: the next one starts quietly
			return self.status()

	def stop_all(self) -> None:
		for worker in list(self.workers.values()):
			worker.stop()
		self.workers.clear()
		self._default_started = False
