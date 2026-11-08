# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared local IPC client for Haumea agent tools."""

from __future__ import annotations

import json
import errno
import math
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import uuid


PROTOCOL_VERSION = 1
# The same default as launch-haumea-agent.sh, so the CLI and the MCP server
# find an editor it started without any configuration.
DEFAULT_ENGINE_SOCKET = Path(os.environ.get(
	"HAUMEA_ENGINE_SOCKET", str(Path.home() / ".cache" / "haumea-agent.sock")))
MAX_REQUEST_BYTES = 1_048_576
# sockaddr_un.sun_path, with its terminating NUL.
SUN_PATH_MAX = 104 if sys.platform == "darwin" else 108
MAX_RESPONSE_BYTES = 4 * 1024 * 1024
# Who this process is to the editor: several agents can work in one Haumea,
# each with its own edit session. It goes after an '@' in each request id.
AGENT_ID = "".join(c for c in os.environ.get("HAUMEA_AGENT_ID", "") if c.isalnum() or c in "-_")[:40] \
	or uuid.uuid4().hex[:12]


def haumea_config_dir() -> Path:
	"""Haumea's config folder, found as the editor finds it (workspace.cpp)."""
	custom = os.environ.get("HAUMEA_CONFIG_DIR")
	if custom:
		return Path(custom)
	if os.name == "nt":
		return Path(os.environ.get("APPDATA", str(Path.home()))) / "Haumea"
	if sys.platform == "darwin":
		return Path.home() / "Library" / "Application Support" / "Haumea"
	return Path(os.environ.get("XDG_CONFIG_HOME") or (Path.home() / ".config")) / "haumea"


def namaka_connection() -> tuple[str, str] | None:
	"""The port and token a Haumea listening for Namaka on loopback TCP wrote
	(Windows: <config>/namaka/connection.json), or None. Read on every request:
	the port changes each time Haumea starts."""
	try:
		data = json.loads((haumea_config_dir() / "namaka" / "connection.json").read_text(encoding="utf-8"))
		return str(int(data["port"])), str(data["token"])
	except (OSError, ValueError, KeyError, TypeError):
		return None


class HaumeaError(RuntimeError):
	"""A typed engine error that preserves revision and structured context."""

	def __init__(self, message: str, response: dict | None = None):
		super().__init__(message)
		self.response = response or {}
		self.scene_revision = self.response.get("scene_revision")
		self.camera_revision = self.response.get("camera_revision")


class HaumeaClient:
	def __init__(self, engine_socket: Path = DEFAULT_ENGINE_SOCKET, timeout: float = 30.0,
	             agent: str | None = None):
		self.engine_socket = Path(engine_socket)
		self.timeout = timeout
		self.agent = agent or AGENT_ID
		self.tcp_port = os.environ.get("HAUMEA_ENGINE_TCP_PORT")
		self.token = os.environ.get("HAUMEA_ENGINE_TOKEN", "")
		# Nothing set: a Haumea listening for Namaka on its own (connection.json
		# on Windows, the default socket elsewhere).
		self.automatic = (self.tcp_port is None and "HAUMEA_ENGINE_SOCKET" not in os.environ and
			self.engine_socket == DEFAULT_ENGINE_SOCKET)

	def _check_config(self) -> None:
		# Validated per request rather than at construction, so an MCP server
		# with a bad environment still starts and can explain the problem.
		if self.tcp_port is not None:
			if not self.tcp_port.isascii() or not self.tcp_port.isdigit() or not 1 <= int(self.tcp_port) <= 65535:
				raise HaumeaError("HAUMEA_ENGINE_TCP_PORT must be between 1 and 65535")
			if not 32 <= len(self.token.encode("utf-8")) <= 256 or any(c in self.token for c in "\r\n"):
				raise HaumeaError("HAUMEA_ENGINE_TOKEN must contain 32-256 UTF-8 bytes and no newlines")
		elif len(os.fsencode(self.engine_socket)) >= SUN_PATH_MAX:
			runtime = os.environ.get("XDG_RUNTIME_DIR") or (
				f"/run/user/{os.getuid()}" if hasattr(os, "getuid") else tempfile.gettempdir())
			raise HaumeaError(
				f"The agent socket path is {len(os.fsencode(self.engine_socket))} bytes long: {self.engine_socket}. "
				f"Unix socket paths must be shorter than {SUN_PATH_MAX} bytes (the AF_UNIX limit), so "
				f"neither the editor nor this client can use it. Pick a shorter one, such as "
				f"{runtime}/haumea.sock, and give the same path to the editor's --agent-socket "
				"and to HAUMEA_ENGINE_SOCKET here.")

	def _unreachable(self, error: OSError) -> HaumeaError:
		if self.tcp_port is not None:
			return HaumeaError(
				f"Haumea is not reachable on 127.0.0.1:{self.tcp_port} ({error}). Start the "
				"editor with the same HAUMEA_ENGINE_TCP_PORT and HAUMEA_ENGINE_TOKEN as this MCP server.")
		return HaumeaError(
			f"Haumea is not reachable at {self.engine_socket} ({error}). Start the editor with "
			f"--agent-socket {self.engine_socket} (launch-haumea-agent.sh does this).")

	def command(self, command: str, *fields: object) -> dict:
		if self.automatic:
			connection = namaka_connection()
			self.tcp_port, self.token = connection if connection else (None, "")
			if connection is None and os.name == "nt":
				raise HaumeaError(
					"Haumea isn't listening for Namaka. Start Haumea with Namaka installed and "
					"\"Let Namaka connect\" on (Preferences > Namaka).")
		self._check_config()
		request_id = uuid.uuid4().hex + "@" + self.agent
		encoded_fields = [str(field) for field in fields]
		if any("\n" in field or "\r" in field for field in encoded_fields):
			raise ValueError("Haumea IPC fields cannot contain newlines")
		payload = "\n".join([
			f"HAUMEA_IPC/{PROTOCOL_VERSION}", request_id, command, *encoded_fields
		]).encode("utf-8")
		if self.tcp_port is not None:
			payload = b"HAUMEA_AUTH/1 " + self.token.encode("utf-8") + b"\n" + payload
		if len(payload) > MAX_REQUEST_BYTES:
			raise ValueError("Haumea IPC request exceeds the 1 MB engine limit")

		try:
			response = self._stream_command(payload)
		except OSError as error:
			# Running installations may still expose the original datagram bridge.
			# Keep read/write compatibility during deployment, but all new builds use
			# framed streams so bulk mutation receipts cannot disappear after success.
			if self.tcp_port is not None or error.errno not in {
				errno.EPROTOTYPE, errno.EPROTONOSUPPORT, errno.ECONNREFUSED,
				errno.EINVAL, errno.ENOENT,
			}:
				raise self._unreachable(error) from error
			try:
				response = self._datagram_command(payload)
			except OSError as fallback_error:
				raise self._unreachable(error) from fallback_error
		return self._validate_response(response, request_id)

	def _stream_command(self, payload: bytes) -> dict:
		with socket.socket(socket.AF_INET if self.tcp_port is not None else socket.AF_UNIX, socket.SOCK_STREAM) as client:
			client.settimeout(self.timeout)
			client.connect(("127.0.0.1", int(self.tcp_port)) if self.tcp_port is not None else str(self.engine_socket))
			client.sendall(struct.pack("!I", len(payload)) + payload)
			header = self._recv_exact(client, 4)
			response_size = struct.unpack("!I", header)[0]
			if response_size < 1 or response_size > MAX_RESPONSE_BYTES:
				raise HaumeaError("Haumea returned an invalid framed response size")
			return json.loads(self._recv_exact(client, response_size).decode("utf-8"))

	@staticmethod
	def _recv_exact(client: socket.socket, size: int) -> bytes:
		chunks: list[bytes] = []
		remaining = size
		while remaining:
			chunk = client.recv(remaining)
			if not chunk:
				raise HaumeaError("Haumea closed the framed response early")
			chunks.append(chunk)
			remaining -= len(chunk)
		return b"".join(chunks)

	def _datagram_command(self, payload: bytes) -> dict:

		client_path = Path(tempfile.gettempdir()) / f"haumea-client-{os.getpid()}-{uuid.uuid4().hex[:8]}.sock"
		with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as client:
			try:
				client.bind(str(client_path))
				os.chmod(client_path, 0o600)
				client.settimeout(self.timeout)
				client.sendto(payload, str(self.engine_socket))
				response = json.loads(client.recv(1_048_576).decode("utf-8"))
			finally:
				client_path.unlink(missing_ok=True)
		return response

	@staticmethod
	def _validate_response(response: dict, request_id: str) -> dict:
		if response.get("error") == "authentication failed":
			raise HaumeaError("Haumea TCP authentication failed", response)
		if response.get("request_id") != request_id:
			raise HaumeaError("Haumea returned a response for a different request")
		if response.get("protocol_version") != PROTOCOL_VERSION:
			raise HaumeaError("Haumea returned an incompatible protocol version")
		if not response.get("ok"):
			raise HaumeaError(response.get("error", "unknown Haumea error"), response)
		return response

	def capture_current(self, output_path: Path, *, label: str = "current") -> dict:
		"""Capture the live viewport and retain the exact pose used as provenance."""
		output_path = Path(output_path).resolve()
		output_path.parent.mkdir(parents=True, exist_ok=True)
		return self.command("capture", output_path, label)

	def capture_at_pose(self, output_path: Path, *, position: list[float],
	                    target: list[float], up: list[float] | None = None,
	                    fov: float = 58.0, label: str = "custom",
	                    expected_camera_revision: int | None = None) -> dict:
		"""Capture one temporary pose; the engine restores the complete live pose."""
		up = up or [0.0, 0.0, 1.0]
		output_path = Path(output_path).resolve()
		output_path.parent.mkdir(parents=True, exist_ok=True)
		fields: list[object] = [output_path, label, *position, *target, *up, fov]
		if expected_camera_revision is not None:
			fields.append(expected_camera_revision)
		return self.command("capture_pose", *fields)

	def raycast_segment(self, start: list[float], target: list[float], *,
	                    target_tolerance: float = 1.0) -> dict:
		"""Test line of sight without changing the live camera."""
		return self.command("raycast_segment", *start, *target, target_tolerance)["ray"]

	def _score_view_pose(self, position: list[float], target: list[float], span: float,
	                     support_z: float | None = None) -> dict:
		probe_radius = max(1.5, min(span * 0.12, 6.0))
		vertical_radius = max(1.5, min(span * 0.08, 4.0))
		probes = [
			("center", [0.0, 0.0, 0.0]),
			("east", [probe_radius, 0.0, 0.0]),
			("west", [-probe_radius, 0.0, 0.0]),
			("north", [0.0, probe_radius, 0.0]),
			("south", [0.0, -probe_radius, 0.0]),
			("high", [0.0, 0.0, vertical_radius]),
			("north_east", [probe_radius, probe_radius, 0.0]),
			("south_west", [-probe_radius, -probe_radius, 0.0]),
		]
		omitted_probes = []
		# A below-ground endpoint makes the floor look like an occluder for every
		# possible camera. Only sample the lower subject when it stays above support.
		if support_z is None or target[2] - vertical_radius > support_z + 0.25:
			probes.insert(5, ("low", [0.0, 0.0, -vertical_radius]))
		else:
			omitted_probes.append("low_below_support_plane")
		results = []
		for label, offset in probes:
			endpoint = [target[index] + offset[index] for index in range(3)]
			ray = self.raycast_segment(position, endpoint, target_tolerance=1.5)
			results.append({"probe": label, **ray})
		visible = sum(1 for result in results if result["visible"])
		line_of_sight_score = sum(
			1.0 if result["visible"] else float((result.get("hit") or {}).get("fraction", 0.0))
			for result in results) / len(results)
		return {"visible_probes": visible, "probe_count": len(results),
		        "visibility": visible / len(results),
		        "line_of_sight_score": line_of_sight_score, "probes": results,
		        "support_z": support_z, "omitted_probes": omitted_probes}

	def _resolve_visible_pose(self, base: tuple[float, ...], span: float, label: str,
	                         support_z: float | None = None) -> tuple[list[float], dict]:
		"""Choose the least-displaced candidate with the clearest target footprint."""
		target = list(map(float, base[3:]))
		base_position = list(map(float, base[:3]))
		dx, dy = base_position[0] - target[0], base_position[1] - target[1]
		horizontal = max(math.hypot(dx, dy), 1.0)
		azimuth = math.atan2(dy, dx)
		base_height = base_position[2] - target[2]
		# Preserve the named compass intent: occlusion avoidance may orbit at most
		# 30 degrees, then it must solve with height or distance instead.
		variations = [
			(0, 0.0, 1.0), (0, 0.18, 1.0),
			(20, 0.0, 1.0), (-20, 0.0, 1.0),
			(30, 0.18, 1.0), (-30, 0.18, 1.0),
			(0, 0.40, 1.0), (0, 0.12, 0.72),
			(30, 0.22, 0.85), (-30, 0.22, 0.85),
		]
		best_position, best_diagnostic, best_rank, best_variation = base_position, None, None, None
		for candidate_index, (angle_degrees, height_factor, distance_factor) in enumerate(variations):
			angle = azimuth + math.radians(angle_degrees)
			position = [
				target[0] + math.cos(angle) * horizontal * distance_factor,
				target[1] + math.sin(angle) * horizontal * distance_factor,
				target[2] + base_height + span * height_factor,
			]
			diagnostic = self._score_view_pose(position, target, span, support_z=support_z)
			rank = (diagnostic["visible_probes"], diagnostic["line_of_sight_score"],
			        -candidate_index)
			if best_rank is None or rank > best_rank:
				best_position, best_diagnostic, best_rank = position, diagnostic, rank
				best_variation = (angle_degrees, height_factor, distance_factor)
			if diagnostic["visible_probes"] == diagnostic["probe_count"]:
				break
		assert best_diagnostic is not None and best_variation is not None
		best_diagnostic.update({
			"strategy": "multi_candidate_segment_raycast_v1",
			"label": label,
			"base_position": base_position,
			"selected_position": best_position,
			"candidate_count": candidate_index + 1,
			"azimuth_adjustment_degrees": best_variation[0],
			"height_adjustment": span * best_variation[1],
			"distance_factor": best_variation[2],
			"azimuth_limit_degrees": 30,
			"adjusted": any(abs(a - b) > 1.0e-5 for a, b in zip(best_position, base_position)),
		})
		return [*best_position, *target], best_diagnostic

	def capture_views(self, output_dir: Path, fov: float = 58.0, *,
	                  center: list[float] | None = None,
	                  span: float | None = None) -> dict:
		"""Frame a scratch layer or explicit survey zone without parking the live camera."""
		if center is None:
			try:
				bounds = self.command("scene_bounds")["bounds"]
				minimum, maximum, center = bounds["min"], bounds["max"], bounds["center"]
				span = max(maximum[0] - minimum[0], maximum[1] - minimum[1], 30.0)
				center_z, ground_z = center[2], minimum[2]
			except HaumeaError:
				camera = self.command("camera_context")["camera"]
				center = list(map(float, camera["target"]))
				span = max(float(span or 60.0), 10.0)
				center_z = ground_z = center[2]
		else:
			center = list(map(float, center))
			span = max(float(span or 60.0), 10.0)
			center_z = ground_z = center[2]
			maximum = [center[0], center[1], center[2]]
		assert center is not None and span is not None
		views = {
			"aerial": (
				center[0] - span * 0.9, center[1] - span * 0.9, maximum[2] + span * 1.15,
				center[0], center[1], center_z,
			),
			"south": (
				center[0], center[1] - span * 1.35, ground_z + span * 0.42,
				center[0], center[1], center_z,
			),
			"west": (
				center[0] - span * 1.35, center[1], ground_z + span * 0.42,
				center[0], center[1], center_z,
			),
			"player": (
				center[0] - span * 0.45, center[1] - span * 0.7, ground_z + 4.5,
				center[0], center[1], ground_z + 2.2,
			),
		}
		output_dir = Path(output_dir).resolve()
		output_dir.mkdir(parents=True, exist_ok=True)
		items: list[dict] = []
		for name, camera in views.items():
			camera, framing = self._resolve_visible_pose(
				camera, span, name, support_z=float(ground_z))
			path = output_dir / f"{name}.png"
			result = self.capture_at_pose(path, position=list(camera[:3]),
			                              target=list(camera[3:]), fov=fov, label=name)
			result["framing"] = framing
			items.append(result)
		manifest = {"schema": 1, "kind": "haumea_scene_views",
		            "scene_revision": items[-1].get("scene_revision") if items else None,
		            "center": center, "span": span, "views": items}
		manifest_path = output_dir / "capture-manifest.json"
		manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
		manifest["manifest_path"] = str(manifest_path)
		return manifest
