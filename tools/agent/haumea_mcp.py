#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""MCP stdio adapter for Haumea's shared local agent service."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Literal

import functools
import inspect

from mcp.server import MCPServer
from mcp.server.mcpserver.exceptions import ToolError
from mcp.server.mcpserver import Image
from PIL import Image as PILImage
from pydantic import BaseModel

if __package__:
	from .haumea_ipc import HaumeaError, DEFAULT_ENGINE_SOCKET
else:
	from haumea_ipc import HaumeaError, DEFAULT_ENGINE_SOCKET
if __package__:
	from .asset_index import DEFAULT_DATABASE
else:
	from asset_index import DEFAULT_DATABASE
if __package__:
	from .asset_discovery import DEFAULT_SESSION_DIR
else:
	from asset_discovery import DEFAULT_SESSION_DIR
if __package__:
	from .service import HaumeaService, ScenePatchError, DEFAULT_STATE_DIR
else:
	from service import HaumeaService, ScenePatchError, DEFAULT_STATE_DIR


service = HaumeaService(
	Path(os.environ.get("HAUMEA_ENGINE_SOCKET", DEFAULT_ENGINE_SOCKET)),
	Path(os.environ.get("HAUMEA_ASSET_DB", DEFAULT_DATABASE)),
	discovery_dir=Path(os.environ.get("HAUMEA_DISCOVERY_DIR", DEFAULT_SESSION_DIR)),
	state_dir=Path(os.environ.get("HAUMEA_AGENT_STATE_DIR", DEFAULT_STATE_DIR)),
)
mcp = MCPServer(
	"haumea",	# the server id stays "haumea": tools are mcp__haumea__*
	title="Namaka for Haumea Studio",
	description="Namaka: look at and edit the map open in Haumea Studio, and find GTA assets.",
	instructions=(
		"This is Namaka, Haumea Studio's agent companion. "
		"Haumea edits SA-MP/open.mp maps: the document is the user's Pawn map (.pwn), which holds "
		"objects, vehicles, pickups, 3D text labels, actors, player classes, gang zones, map icons, "
		"NPCs, NPC path points, sounds, checkpoints and textdraws, plus removed GTA buildings. "
		"Start with haumea_status: the open map and the camera target, a good place to build. "
		"open_map, new_map, merge_map and save_map work like the File menu. "
		"Place with place_element (one) or place_elements (many), with their settings in properties "
		"in the same call; change them with set_element_properties and move_element; delete_element "
		"removes an element or a GTA building (RemoveBuildingForPlayer). The first edit starts an "
		"edit session: save_map commits it, finish_edit_session(commit=False) undoes all of it, "
		"undo_edit steps back once. Check with map_check (what breaks in game) and capture_view "
		"(a screenshot), then save_map; checkpoints need format='streamer'. "
		"Find models with search_assets. For larger scenes: survey the area, build a palette with "
		"propose_asset_palette and render_palette_board, use stable keys in apply_scene_patch, block "
		"out large forms before details, review the images and fix concrete findings. Preserve "
		"locked objects and the user's selection. Move the user's camera with move_camera only "
		"when they ask to see or go somewhere, or to show a result you just made; otherwise look "
		"with capture_from_pose, which leaves it alone. Whenever you move it, say so and that "
		"camera_back returns."
	),
)


_register_tool = mcp.tool


def _guarded_tool(*args, **kwargs):
	"""Register a tool whose failures reach the client with their message.

	The MCP SDK replaces anything but ToolError with a bare "Error executing
	tool", which hid every editor, validation and connection error."""
	register = _register_tool(*args, **kwargs)

	def decorate(function):
		if inspect.iscoroutinefunction(function):
			@functools.wraps(function)
			async def guarded(*call_args, **call_kwargs):
				try:
					return await function(*call_args, **call_kwargs)
				except ToolError:
					raise
				except Exception as error:
					raise ToolError(str(error) or type(error).__name__) from error
		else:
			@functools.wraps(function)
			def guarded(*call_args, **call_kwargs):
				try:
					return function(*call_args, **call_kwargs)
				except ToolError:
					raise
				except Exception as error:
					raise ToolError(str(error) or type(error).__name__) from error
		return register(guarded)
	return decorate


mcp.tool = _guarded_tool


class SceneOperation(BaseModel):
	"""One declarative edit; fields are validated again by Haumea before use.

	place puts an object by default; set kind (and properties, as for
	place_element) to place any other element. Relative placement, arrays and
	support snapping work for objects only."""
	action: Literal["place", "place_relative", "transform", "transform3d", "delete", "array_line", "array_ring"]
	key: str | None = None
	kind: Literal["object", "vehicle", "pickup", "label", "actor", "class", "gangzone",
	              "mapicon", "npc", "pathpoint", "sound", "checkpoint", "area"] = "object"
	properties: dict | None = None
	model: int | str | None = None
	instance_id: int | None = None
	group: str | None = None
	supported_by: str | None = None
	support_mode: Literal["declare_only", "snap"] = "declare_only"
	clearance: float = 0.0
	relative_to: str | None = None
	parent_anchor: str | None = None
	child_anchor: str | None = None
	relation: Literal["offset", "on_top", "in_front", "behind", "left", "right"] = "offset"
	gap: float = 0.0
	local_x: float = 0.0
	local_y: float = 0.0
	local_z: float = 0.0
	x: float | None = None
	y: float | None = None
	z: float | None = None
	x2: float | None = None
	y2: float | None = None
	z2: float | None = None
	radius: float | None = None
	count: int | None = None
	jitter: float = 0.0
	heading_noise: float = 0.0
	seed: str | int | None = None
	face: Literal["outward", "inward"] = "outward"
	pitch: float | None = None
	roll: float | None = None
	heading: float | None = None
	snap: bool = True


class SemanticZone(BaseModel):
	"""An explicit entrance, route, or keep-clear footprint in world coordinates."""
	name: str
	role: Literal["entrance", "circulation", "keep_clear"] = "keep_clear"
	shape: Literal["circle", "rect"]
	x: float
	y: float
	radius: float | None = None
	width: float | None = None
	depth: float | None = None
	heading: float = 0
	z_min: float | None = None
	z_max: float | None = None
	ignore_keys: list[str] = []
	severity: Literal["warning", "error"] = "error"


@mcp.tool()
def haumea_status() -> dict:
	"""Start here: the open map (path, format, unsaved changes, element count, and
	area_removals: its RemoveBuildingForPlayer model -1 spheres, which remove every world
	building inside them), whether an edit session is open, the map's extent and the live
	camera's target (a good spot to build)."""
	try:
		bounds = service.engine("scene_bounds")
	except HaumeaError:
		bounds = None
	session = service.engine("session_status")
	camera = service.engine("camera_context").get("camera", {})
	capabilities = service.engine("capabilities")
	return {
		"map": session.get("map"),
		"session": {"active": session.get("active"), "name": session.get("session_id") or None},
		"bounds": bounds,
		"camera_target": camera.get("target"),
		"element_kinds": capabilities.get("element_kinds"),
		"build_id": capabilities.get("build_id"),
	}


@mcp.tool()
def search_assets(query: str, category: str | None = None,
	              max_width: float | None = None, max_depth: float | None = None,
	              has_collision: bool | None = None, defined_only: bool = True,
	              limit: int = 12, detail: Literal["compact", "full"] = "compact") -> dict:
	"""Find object models by name words (e.g. "petrol", "bench", "fence"), optionally limited in size.

	Compact results: id (the model for place_element), name, txd, size [w, d, h]
	in metres and draw_distance; detail="full" adds the whole catalogue record.
	Try a few short words: names are the game's, like "petrolpump" or "nt_bench".
	max_width, max_depth and has_collision need sizes and collision in the asset
	index, which it doesn't have yet: until then they are ignored and the result
	has a "note" saying so."""
	note = service.geometry_note(max_width=max_width, max_depth=max_depth, has_collision=has_collision)
	results = service.search_assets(query, category=category, max_width=max_width,
	                                max_depth=max_depth, has_collision=has_collision,
	                                defined_only=defined_only, limit=limit)
	if detail == "full":
		return {"results": results, **({"note": note} if note else {})}
	compact = []
	for asset in results:
		bounds = (asset.get("runtime") or {}).get("collision_bounds")
		size = [round(bounds[1][i] - bounds[0][i], 2) for i in range(3)] if bounds else None
		compact.append({key: value for key, value in {
			"id": asset.get("id"), "name": asset.get("name"), "txd": asset.get("txd"),
			"size": size, "draw_distance": asset.get("draw_distance")}.items() if value is not None})
	return {"results": compact, "count": len(compact), **({"note": note} if note else {})}


@mcp.tool()
def inspect_asset(asset_id: int, ensure_renderable: bool = False) -> dict | None:
	"""Return catalogue metadata plus authoritative runtime availability and origin bounds."""
	return service.inspect_asset(asset_id, ensure_renderable=ensure_renderable)


@mcp.tool(structured_output=False)
def render_asset_views(asset_id: int, output_path: str, size: int = 512) -> list:
	"""Render two local canonical asset views with identity-axis provenance."""
	result = service.render_asset_views(asset_id, Path(output_path), size=size)
	return [result, Image(path=result["path"])]


@mcp.tool()
def similar_assets(asset_id: int, limit: int = 20) -> list[dict]:
	"""Find assets with matching GTA style, texture dictionary, and scale."""
	return service.assets.similar(asset_id, limit)




@mcp.tool()
def start_asset_discovery(brief: str, roles: list[str] | None = None,
	                      styles: list[str] | None = None, avoid: list[str] | None = None,
	                      context_asset_ids: list[int] | None = None,
	                      max_width: float | None = None, max_depth: float | None = None,
	                      has_collision: bool | None = None, pool_limit: int = 2500) -> dict:
	"""Start a high-recall catalogue session and report auditable coverage.

	After observing the scene, put scene context in ``brief`` and explicitly list
	the prop roles wanted. Use ``avoid`` for visually inappropriate classes. The session
	considers every eligible non-LOD asset, retains diverse family representatives,
	and tracks what Codex has and has not visually reviewed.
	"""
	constraints = {key: value for key, value in {
		"max_width": max_width, "max_depth": max_depth, "has_collision": has_collision,
	}.items() if value is not None}
	structured_brief = {"scene": brief, "roles": roles or [], "styles": styles or [],
	                    "avoid": avoid or []}
	return service.discovery.start(structured_brief, context_asset_ids=context_asset_ids,
	                               constraints=constraints, pool_limit=pool_limit)




@mcp.tool()
def browse_asset_families(session_id: str, offset: int = 0, limit: int = 100) -> dict:
	"""List ranked visual/functional families without collapsing their members."""
	return service.discovery.families(session_id, offset=offset, limit=limit)


@mcp.tool()
def discovery_candidates(session_id: str, offset: int = 0, limit: int = 100) -> list[dict]:
	"""Return fused candidates with scores and independent retrieval-channel evidence."""
	return service.discovery.results(session_id, offset=offset, limit=limit)["assets"]


@mcp.tool()
def open_asset_family(session_id: str, family: str, offset: int = 0, limit: int = 64) -> list[dict]:
	"""Open one family, mark its page reviewed, and return full asset passports."""
	return service.discovery.open_family(session_id, family, offset=offset, limit=limit)["assets"]






@mcp.tool()
def mark_discovery_assets(session_id: str, asset_ids: list[int],
	                      decision: Literal["shortlist", "rejected"], reason: str = "") -> dict:
	"""Persist Codex's visual selection or rejection rationale in the coverage ledger."""
	return service.discovery.mark(session_id, asset_ids, decision=decision, reason=reason)


@mcp.tool()
def discover_assets(brief: str, thoroughness: Literal["quick", "broad", "exhaustive"] = "quick",
	                roles: list[str] | None = None, styles: list[str] | None = None,
	                avoid: list[str] | None = None, limit: int = 40,
	                detail: Literal["compact", "full"] = "compact") -> dict:
	"""Discover diverse concepts; compact results are token-safe, full keeps complete passports."""
	return service.discover_assets(brief, thoroughness=thoroughness, roles=roles,
	                               styles=styles, avoid=avoid, limit=limit, detail=detail)


@mcp.tool()
def begin_edit_session(name: str = "agent-proposal") -> dict:
	"""Begin a named edit session on the open map (optional: the first edit starts one).

	finish_edit_session(commit=False) undoes everything since; save_map commits it."""
	return service.engine("session_begin", [name])


@mcp.tool()
def camera_context() -> dict:
	"""Read the exact live camera pose, basis, FOV, aspect ratio, and viewport."""
	return service.engine("camera_context")


@mcp.tool()
def move_camera(look_at: int | str | list[float] | None = None,
	            position: list[float] | None = None, target: list[float] | None = None,
	            distance: float | None = None, pitch: float = 30.0) -> dict:
	"""Fly the user's live view (a 0.3 s move) when they ask to see or go somewhere, or to
	show them a result you just made; to only look, use capture_from_pose instead.

	Either position and target ([x, y, z] each), or look_at: an instance id, "selection",
	a scene group, a project zone's name, an element's name (its name or npc_name
	property) or [x, y, z]. look_at keeps the user's heading, looks pitch degrees down
	and backs off distance metres (default: enough to fit it). Returns the new camera
	and camera_revision. Refused with "busy" while the user drags the gizmo, places or
	looks around: wait and try again. Always tell the user you moved the view and that
	camera_back returns."""
	return service.move_camera(position=position, target=target, look_at=look_at,
	                           distance=distance, pitch=pitch)


@mcp.tool()
def camera_back() -> dict:
	"""Fly the live view back to where it was before the last move_camera (up to 10 moves)."""
	return service.camera_back()


@mcp.tool()
def screen_to_world(pixel_x: float, pixel_y: float,
	                viewport_width: int, viewport_height: int) -> dict:
	"""Resolve an image pixel to visible geometry or the terrain beneath that ray."""
	return service.engine("screen_to_world", [pixel_x, pixel_y, viewport_width, viewport_height])


@mcp.tool()
def raycast_segment(start_x: float, start_y: float, start_z: float,
	                target_x: float, target_y: float, target_z: float,
	                target_tolerance: float = 1.0) -> dict:
	"""Test whether world geometry occludes a segment without moving the live camera."""
	return service.client.raycast_segment(
		[start_x, start_y, start_z], [target_x, target_y, target_z],
		target_tolerance=target_tolerance)


@mcp.tool()
def apply_scene_patch(operations: list[SceneOperation], patch_id: str | None = None,
	                  expected_revision: int | None = None) -> dict:
	"""Apply a replay-safe patch using stable caller-owned keys.

	Set ``expected_revision`` after inspection to reject stale plans. Reusing the
	same ``patch_id`` returns the original receipt without duplicating mutations.
	"""
	try:
		return service.apply_scene_patch(
			[operation.model_dump(exclude_none=True) for operation in operations],
			patch_id=patch_id, expected_revision=expected_revision)
	except ScenePatchError as error:
		return error.payload


@mcp.tool()
def resolve_scene_patch(operations: list[SceneOperation],
	                    expected_revision: int | None = None) -> dict:
	"""Resolve and fully prevalidate a revision-bound plan without mutating the scene."""
	try:
		return service.resolve_scene_patch(
			[operation.model_dump(exclude_none=True) for operation in operations],
			expected_revision=expected_revision)
	except ScenePatchError as error:
		return error.payload


@mcp.tool()
def apply_resolved_plan(plan_id: str, patch_id: str | None = None) -> dict:
	"""Apply a prior resolved plan only if its scratch session and revision still match."""
	try:
		return service.apply_resolved_plan(plan_id, patch_id=patch_id)
	except ScenePatchError as error:
		return error.payload


@mcp.tool()
def create_scene_group(group: str, members: list[str | int]) -> dict:
	"""Create or replace a semantic group from stable keys or runtime IDs."""
	return service.create_group(group, members)


@mcp.tool()
def list_scene_groups() -> dict:
	"""List semantic groups in the active scratch composition."""
	return service.list_groups()


@mcp.tool()
def inspect_scene_group(group: str, offset: int = 0, limit: int = 50) -> dict:
	"""Inspect one revision-friendly page of a semantic group."""
	return service.inspect_group(group, offset=offset, limit=limit)


@mcp.tool()
def transform_scene_group(group: str, dx: float = 0.0, dy: float = 0.0,
	                      dz: float = 0.0, dheading: float = 0.0,
	                      patch_id: str | None = None,
	                      expected_revision: int | None = None) -> dict:
	"""Move and rotate a complete composition around its centroid."""
	return service.transform_group(group, dx=dx, dy=dy, dz=dz, dheading=dheading,
	                               patch_id=patch_id, expected_revision=expected_revision)


@mcp.tool()
def clone_scene_group(group: str, new_group: str, key_prefix: str,
	                  dx: float = 0.0, dy: float = 0.0, dz: float = 0.0,
	                  dheading: float = 0.0, patch_id: str | None = None) -> dict:
	"""Clone a composition with new stable keys and an optional rigid transform."""
	return service.clone_group(group, new_group, key_prefix, dx=dx, dy=dy, dz=dz,
	                           dheading=dheading, patch_id=patch_id)


@mcp.tool()
def delete_scene_group(group: str, patch_id: str | None = None) -> dict:
	"""Delete every live object in a semantic group."""
	return service.delete_group(group, patch_id=patch_id)


@mcp.tool()
def set_object_support(child: str | int, parent: str | int) -> dict:
	"""Declare that a child is intentionally supported by another scene object."""
	return service.set_support(child, parent)


@mcp.tool()
def snap_object_to_support(child: str | int, parent: str | int,
	                       clearance: float = 0.0) -> dict:
	"""Place a child on the collision top of a parent and record the support relation."""
	return service.snap_to_support(child, parent, clearance)






@mcp.tool()
def list_scene_objects(offset: int = 0, limit: int = 64,
	                   x: float | None = None, y: float | None = None,
	                   radius: float | None = None, include_world: bool = False) -> dict:
	"""List the map's elements (one page, optionally within a circle of radius around x, y):
	instance_id, model, position, rotation, kind, properties and materials.

	include_world=True (needs x, y, radius) also lists the GTA buildings there
	(agent_scene=false): delete_element on one exports RemoveBuildingForPlayer."""
	if include_world:
		if x is None or y is None or radius is None:
			raise ValueError("include_world needs x, y and radius")
		return service.engine("inspect_zone_page", [x, y, radius, offset, limit])
	fields: list[object] = [offset, limit]
	if x is not None and y is not None and radius is not None:
		fields.extend([x, y, radius])
	return service.engine("list_page", fields)


@mcp.tool()
def analyze_placement(instance_id: int, grid_size: int = 3) -> dict:
	"""Measure terrain support, signed clearance, relief, normals, and slope below an object."""
	return service.engine("analyze_placement", [instance_id, grid_size])


@mcp.tool()
def fit_object_to_terrain(instance_id: int, profile: Literal["building", "prop"],
	                      search_radius: float = 30.0, search_step: float = 3.0,
	                      max_support_relief: float | None = None) -> dict:
	"""Relocate an upright building to flat support or ground-align a small prop."""
	fields: list[object] = [instance_id, profile, search_radius, search_step]
	if max_support_relief is not None:
		fields.append(max_support_relief)
	return service.engine("fit_terrain", fields)


@mcp.tool()
def validate_scene(x: float | None = None, y: float | None = None, radius: float | None = None,
	               offset: int = 0, limit: int = 64) -> dict:
	"""Check placement: objects floating, buried, on steep slopes, duplicated or overlapping.

	With x, y and radius, one page of that area only. For what breaks in game
	(limits, missing models, names, NPC paths) use map_check."""
	if x is not None and y is not None and radius is not None:
		return service.engine("validate_zone", [x, y, radius, offset, limit])
	return service.validate_composition()


@mcp.tool()
def map_check() -> dict:
	"""List what will break or look wrong in game: limits, models, positions, materials, NPCs,
	checkpoints, server version. Each item has severity, category, text and the elements
	concerned (instance_id, kind, name, position, rotation), ready for move_element,
	set_element_properties or delete_element. Run it again after fixing."""
	return service.map_check()




@mcp.tool()
def validate_semantic_zones(zones: list[SemanticZone]) -> dict:
	"""Detect props blocking explicitly declared entrances, routes, or clear areas."""
	return service.validate_semantic_zones([
		zone.model_dump(exclude_none=True) for zone in zones
	])


@mcp.tool()
def list_checkpoints() -> dict:
	"""List persistent scratch manifests available across Haumea restarts."""
	return service.checkpoint_list()


@mcp.tool()
def save_checkpoint(name: str) -> dict:
	"""Atomically persist the current scratch layer without saving the IPL."""
	return service.checkpoint_save(name)


@mcp.tool()
def restore_checkpoint(name: str) -> dict:
	"""Replace the active scratch proposal from a persistent checkpoint manifest."""
	return service.checkpoint_restore(name)




@mcp.tool(structured_output=False)
def capture_view(output_path: str, label: str = "view",
                 position: list[float] | None = None, target: list[float] | None = None,
                 fov: float = 58.0, up: list[float] | None = None,
                 expected_camera_revision: int | None = None) -> list:
	"""Screenshot the 3D view as a PNG and return it, to check your work.

	Without position/target: the user's live view as it is. With only target
	([x, y, z]; use a position from an element's reply, so it isn't inside a
	building): Haumea picks a camera about
	25 m away that has a clear view of it. With position and target: exactly
	that view. The user's camera is put back afterwards."""
	if position is None and target is None:
		result = service.client.capture_current(Path(output_path), label=label)
	else:
		if target is None or len(target) != 3 or (position is not None and len(position) != 3):
			raise ValueError("target must be [x, y, z]; position too when given")
		if position is None:
			# Try the four diagonals and keep the clearest line of sight.
			best = None
			candidates = [(dx, dy, dz) for dz in (14, 30) for dx, dy in
			              ((18, -18), (-18, -18), (18, 18), (-18, 18))] + [(4, -6, 45)]
			for dx, dy, dz in candidates:
				base = (target[0] + dx, target[1] + dy, target[2] + dz, *target)
				pose, diagnostic = service.client._resolve_visible_pose(base, 20.0, label)
				rank = (diagnostic["visible_probes"], diagnostic["line_of_sight_score"])
				if best is None or rank > best[0]:
					best = (rank, pose)
				if diagnostic["visible_probes"] == diagnostic["probe_count"]:
					break
			position = best[1][:3]
		result = service.client.capture_at_pose(
			Path(output_path), position=list(position), target=list(target),
			up=list(up or [0.0, 0.0, 1.0]), fov=fov, label=label,
			expected_camera_revision=expected_camera_revision)
	for noise in ("protocol_version", "request_id"):
		result.pop(noise, None)
	return [result, Image(path=result["path"])]


@mcp.tool(structured_output=False)
def render_scene_views(output_directory: str, fov: float = 58.0,
	                   center_x: float | None = None, center_y: float | None = None,
	                   center_z: float | None = None, span: float | None = None) -> list:
	"""Render four provenance-bound views; an explicit center enables empty-scene survey."""
	center = None
	if center_x is not None and center_y is not None:
		center = [center_x, center_y, center_z if center_z is not None else 0.0]
	manifest = service.capture_views(Path(output_directory), fov, center=center, span=span)
	views = []
	for item in manifest["views"]:
		name, path = item["label"], item["path"]
		view = PILImage.open(path).convert("RGB")
		# Full-resolution captures stay on disk; the MCP review sheet is bounded so
		# one visual feedback pass does not consume tens of megabytes of context.
		view.thumbnail((960, 540), PILImage.Resampling.LANCZOS)
		views.append((name, view))
	width = max(view.width for _, view in views)
	height = max(view.height for _, view in views)
	sheet = PILImage.new("RGB", (width * 2, (height + 28) * 2), "#17191d")
	from PIL import ImageDraw
	draw = ImageDraw.Draw(sheet)
	for index, (name, view) in enumerate(views):
		x = index % 2 * width
		y = index // 2 * (height + 28)
		sheet.paste(view, (x, y))
		draw.rectangle((x, y + height, x + width, y + height + 28), fill="#111215")
		framing = manifest["views"][index].get("framing", {})
		angle = float(framing.get("azimuth_adjustment_degrees", 0.0))
		caption = name if abs(angle) < 0.5 else f"{name} · auto-orbit {angle:+.0f}°"
		draw.text((x + 8, y + height + 7), caption, fill="white")
	output = Path(output_directory).resolve() / "review-sheet.png"
	sheet.save(output)
	manifest["review_sheet"] = {"path": str(output), "tiles": [
		{"label": item["label"], "path": item["path"],
		 "actual_pose": item.get("actual_pose"),
		 "capture_camera_revision": item.get("capture_camera_revision"),
		 "framing": item.get("framing")}
		for item in manifest["views"]
	]}
	Path(manifest["manifest_path"]).write_text(
		__import__("json").dumps(manifest, indent=2), encoding="utf-8")
	return [manifest, Image(path=output)]


@mcp.tool()
def finish_edit_session(commit: bool) -> dict:
	"""commit=True keeps the session's edits (save_map writes them); commit=False restores the map as it was at begin_edit_session."""
	return service.engine("session_commit" if commit else "session_rollback")




@mcp.tool()
def get_mapping_project() -> dict:
    """Read the persistent brief, palette, zones, relations, locks and creative phase."""
    return service.vibe.project()


@mcp.tool()
def update_mapping_project(changes: dict, expected_revision: int | None = None) -> dict:
    """Merge project fields. List fields replace their previous values. Preserve user choices and original brief."""
    return service.vibe.update_project(changes, expected_revision)


@mcp.tool(structured_output=False)
def survey_mapping_area(output_directory: str, radius: float = 30, grid: int = 7,
                        pixel_x: float | None = None, pixel_y: float | None = None) -> list:
    """Observe the camera target or selected pixel: annotated RGB, sampled depth/IDs, terrain and native objects."""
    result = service.vibe.observe(output_directory, radius, grid, pixel_x, pixel_y)
    # Full spatial payload stays available on disk; avoid duplicating hundreds of rays in the model context.
    return [{"center": result["center"], "scene_revision": result["scene_revision"],
             "objects": result["objects"][:64], "total_objects": result["total_objects"],
             "surface": result["surface"], "survey_path": str(Path(output_directory).resolve() / "survey.json"),
             "limitations": result["limitations"]}, Image(path=result["images"]["annotated.png"])]


@mcp.tool()
def list_composition_recipes() -> dict:
    """List supported parametric compositions and their required palette roles."""
    return service.vibe.dispatch("recipe.list", {})


@mcp.tool()
def build_composition_recipe(name: Literal["terrace", "market", "checkpoint", "alley", "fence", "camp", "logistics", "outpost", "garden"],
                             palette: dict[str, int], key: str, x: float, y: float,
                             heading: float = 0, count: int = 3, aisle: float = 3,
                             gap: float = 0.25, seed: str = "0",
                             points: list[list[float]] | None = None, gate_width: float = 0) -> dict:
    """Generate a dimension-aware patch without applying it. Merge its circulation constraints into project zones."""
    return service.vibe.recipe(name, palette, key=key, x=x, y=y, heading=heading,
                               count=count, aisle=aisle, gap=gap, seed=seed, points=points, gate_width=gate_width)


@mcp.tool()
def asset_passport(asset_id: int) -> dict:
    """Read measured catalogue metadata, sourced visual annotations, functional anchors and explicit unknowns."""
    return service.profiles.get(asset_id)


@mcp.tool()
def annotate_asset_passport(asset_id: int, features: dict, source: str, confidence: float = 0.7) -> dict:
    """Store visually observed features. Anchors are local xyz; front_heading is degrees from model +Y. Include image provenance."""
    return service.profiles.annotate(asset_id, features, source, confidence)


@mcp.tool()
def search_annotated_assets(query: str, limit: int = 30) -> list[dict]:
    """Search learned visual/functional annotations, including the user's own vocabulary."""
    return service.profiles.search(query, limit)










@mcp.tool(structured_output=False)
def review_mapping(output_directory: str, center: list[float] | None = None, span: float | None = None) -> list:
    """Capture geometry/circulation checks and four views for an explicit visual review."""
    report = service.vibe.review(output_directory, center, span)
    return [report, *[Image(path=v["path"]) for v in report["captures"]["views"]]]


@mcp.tool()
def record_mapping_review(review_path: str, scores: dict[str, float], findings: list[dict], accepted: bool = False) -> dict:
    """Record image-grounded criterion scores and specific fixes. Refuses stale scene/project reviews."""
    return service.vibe.record_review(review_path, scores, findings, accepted)


@mcp.tool()
def mapping_environment(hour: int | None = None, minute: int | None = None,
                        weather_a: int | None = None, weather_b: int | None = None,
                        blend: float | None = None) -> dict:
    """Read available weather names or change only specified atmosphere fields; returns the previous state."""
    return service.environment(hour=hour, minute=minute, weather_a=weather_a, weather_b=weather_b, blend=blend)










@mcp.tool()
def check_mapping_route(points: list[list[float]], width: float = 1.2, height: float = 1.8) -> dict:
    """Probe a ground-level xyz route against native and scratch collision, at foot/head height and lateral offsets."""
    return service.vibe.check_route(points, width, height)

@mcp.tool()
def selected_mapping_objects() -> dict:
    """Read the user's editor selection to ground instructions such as 'keep this' or 'move these'."""
    return service.engine("selection")

@mcp.tool()
def search_creative_catalogue(query: str, role: str | None = None, styles: list[str] | None = None,
                              avoid: list[str] | None = None, limit: int = 24,
                              max_dimensions: list[float] | None = None) -> dict:
    """Search French/English concepts. Hard role/size/exclusion filters, sourced observations ranked first."""
    return service.dispatch("catalogue.search", locals())


@mcp.tool()
def propose_asset_palette(brief: str, roles: list[str], styles: list[str] | None = None,
                          avoid: list[str] | None = None, per_role: int = 4,
                          max_dimensions: list[float] | None = None) -> dict:
    """Return coherent candidates grouped by role. Missing roles stay explicit; inspect a visual board before choosing."""
    return service.dispatch("catalogue.palette", locals())


@mcp.tool(structured_output=False)
def render_palette_board(asset_ids: list[int], output_directory: str, size: int = 192) -> list:
    """Render a labelled comparison board with two native views per asset and dimensions."""
    report = service.dispatch("catalogue.board", locals())
    return [report, Image(path=report["path"])]


@mcp.tool()
def propose_composition_variants(name: str, palette: dict[str, int], options: dict, variants: int = 3) -> dict:
    """Generate compact/balanced/open alternatives without changing the scene. Options include key,x,y,count."""
    return service.vibe.recipe_variants(name, palette, options, variants)








@mcp.tool()
def inspect_placement_anchors(asset_id: int) -> dict:
    """Get measured bounds anchors and separately sourced functional anchors. Use names with place_relative."""
    return service.profiles.anchors(asset_id)


@mcp.tool()
def set_object_material(instance_id: int, slot: int, model_id: int,
                        txd: str = "none", texture: str = "none",
                        color: str = "0x00000000") -> dict:
	"""Retexture one material slot (0-15) like SetObjectMaterial.

	model_id: -1 keeps the texture (colour only), 0 is SA-MP's white blanktex,
	otherwise a model whose TXD holds `texture` (e.g. 19297 "matcolours" "white").
	color is ARGB ("0xFFRRGGBB"); 0 keeps the model colour, any other value
	also removes the object's vertex lighting in game. Replaces slot text."""
	return service.set_object_material(instance_id, slot, model_id, txd, texture, color)


@mcp.tool()
def set_object_material_text(instance_id: int, slot: int, text: str,
                             font: str = "Arial", font_size: int = 24, bold: bool = True,
                             text_color: str = "0xFFFFFFFF",
                             background_color: str = "0x00000000",
                             alignment: Literal["left", "center", "right"] = "left",
                             material_size: str = "256x128") -> dict:
	"""Draw text on one material slot (0-15) like SetObjectMaterialText.

	Colours are ARGB. {RRGGBB} in text switches colour; newlines start lines.
	Center alignment also centres vertically. material_size is WxH from 32x32
	to 512x512 (OBJECT_MATERIAL_SIZE_*). Replaces a texture on that slot."""
	return service.set_object_material_text(
		instance_id, slot, text, font=font, font_size=font_size, bold=bold,
		text_color=text_color, background_color=background_color,
		alignment=alignment, material_size=material_size)


@mcp.tool()
def clear_object_material(instance_id: int, slot: int = -1) -> dict:
	"""Restore one material slot, or every slot with slot=-1."""
	return service.clear_object_material(instance_id, slot)


ELEMENT_PROPERTIES_DOC = """Properties by kind (all optional; defaults in brackets):
	every kind: name (Pawn variable), group, comment, world (-1 every world), interior,
	  interior_zero_only [false; streamer, interior 0: writes interiorid 0, only interior 0,
	  instead of -1, every interior], draw_distance, stream_distance (streamer)
	object: move_offset [x,y,z] (MoveObject: metres from where it stands; "none" stops it
	  moving), move_speed (m/s), move_rotation [rx,ry,rz] (the turn on the way; "none");
	object: attach_to (instance id, -1 detaches), attach_sync_rotation
	vehicle: color1, color2 [-1 random], respawn_delay [100 seconds; -1 never], paintjob [-1 none],
	  plate [none], mods [none; component ids, a wrong one lists those that fit]
	pickup: model [1239], pickup_type [1]
	label: text ["3D text label"; {RRGGBB} colour codes, newlines], color [0xFFFFFFFF],
	  draw_distance [20], test_los [false]
	actor: skin [0], health [100], invulnerable [true], plus the animation keys below
	class: skin [0], team [-1 none], weapons [w1,w2,w3; none], ammo [a1,a2,a3]
	gangzone: size_x, size_y [50 each; extent around the position], color [0xFF000088],
	  zone_mode [global; player, streamed: per-player zones, open.mp only]
	mapicon: icon [56, the red dot; 0-63], color [0xFFFFFFFF], style [0 MAPICON_LOCAL]
	npc: npc_name [npc, npc_2...; unique, 3-24 chars], skin [0], health [100], armour [0],
	  invulnerable [false], weapon [0 fists], ammo, world [0],
	  path (an NPC path to walk), move_type [2 jog; NPC_MOVE_TYPE 0-5], move_speed [-1 auto],
	  path_reversed [false], path_repeat [once; loop, back_and_forth: the map gets an
	  OnNPCFinishMovePath helper the gamemode calls], recording (npcmodes/recordings name),
	  recording_at_npc, vehicle (a vehicle's instance id, -1 on foot; NPC_PutInVehicle),
	  seat [0 driver; 1-3 passengers; a driver recording needs seat 0],
	  plays_node, node (0-63), node_point, node_radius, node_set_angle, plus the animation keys
	actor and npc animation (ApplyActorAnimation / NPC_ApplyAnimation): anim_lib, anim_name
	  (e.g. PED / IDLE_CHAT; empty name: none), anim_delta (4.1), anim_loop, anim_lock_x,
	  anim_lock_y, anim_freeze, anim_time (ms, 0: until stopped)
	pathpoint: path (required), order [after the path's last point], stop_range [0.2]
	sound: sound_kind [game; stream: PlayAudioStreamForPlayer, started in _ShowForPlayer, loops
	  as the stream does; game: PlayerPlaySound when a player walks within radius], radius [30 m],
	  url and use_position [true] (stream), sound_id [1097] and stop_on_leave [true] (game; 0 stops it)
	checkpoint (streamer only; native saves leave them out): checkpoint_kind (normal:
	  CreateDynamicCP; race: CreateDynamicRaceCP pointing at the next one of its race),
	  size [5], race_type [0 normal; 1 finish, 3-8 air; 9 automatic: normal, finish on
	  the last; 10 the same in the air], race (name; sets kind race),
	  order [after the race's last checkpoint], race_mode [point_to_point; circuit: the last
	  leads back to the first], race_laps [1; circuits], race_playable [false; true saves the
	  race for haumea_races.inc: StartMapRace(playerid, <map prefix>_race_<name>), works
	  without the streamer]; the race settings apply to all of the race's checkpoints
	area (streamer only; a dynamic area, CreateDynamicCircle..Polygon; the position is its
	  centre, or the bottom centre for a cuboid, cube or cylinder): area_shape [cuboid; circle,
	  cylinder, sphere, rectangle, cuboid, cube, polygon], radius [10] (circle, cylinder,
	  sphere), size_x, size_y [10 each] (rectangle, cuboid, cube), size_z [5] (cuboid, cube),
	  height [5] (cylinder; polygon: 0 means no height limit), points (polygon: [[x, y], ...]
	  world coordinates, 3-32 corners); name is the area's Pawn variable [area, area_2...]
	  and what items refer to; renaming it keeps its items. Read back: items (how many use it)
	object, pickup, label, actor, mapicon, checkpoint: area (a dynamic area's name: the item
	  only streams for players inside it; "" for everywhere)"""


@mcp.tool()
def multiplayer_session(action: Literal["join", "host", "leave", "status"], address: str | None = None,
                        port: int = 7790, password: str | None = None, name: str = "Namaka",
                        discard_changes: bool = False) -> dict:
	"""Edit one map with other people and agents, like File > Multiplayer.

	join: the session at address:port (a haumea-server, or an editor hosting) with password
	(the host's six-letter code or the server's password). The session's map replaces the open
	one, or the open map seeds an empty server. Waits until joined; state "off" with status
	says why not. host: shares the open map on port; others join with this computer's address
	and the returned code. leave: keeps the map as it is. status: state, peers and their
	selections. Several agents can work in one editor too, each with its own edit session:
	finish_edit_session(commit=False) and undo only take back this agent's own edits, and what
	someone else changed last stays. join and host end the edit
	session: unsaved edits refuse them unless discard_changes drops the edits."""
	return service.multiplayer(action, address=address, port=port, password=password, name=name,
	                           discard_changes=discard_changes)


@mcp.tool()
def open_map(path: str, discard_changes: bool = False) -> dict:
	"""Open a map as the document, like File > Open: Pawn (.pwn/.inc), Texture Studio (.db),
	MTA:SA (.map) or IPL (.ipl, converted to Pawn calls).

	Refuses while the open map or the edit session has unsaved changes unless
	discard_changes is true, which drops them. Returns the map status and an
	import report (placed, removed buildings, unreadable calls, unknown models,
	and left_out: what another tool's map had that SA-MP can't show)."""
	return service.open_map(path, discard_changes=discard_changes)


@mcp.tool()
def merge_map(path: str) -> dict:
	"""Add another map's contents to the open map, like File > Merge, in the edit session.

	Takes the same formats as open_map. finish_edit_session(commit=False) takes
	the merged elements out again. Returns the map status, how many elements
	came in and the import report with left_out."""
	return service.merge_map(path)


@mcp.tool()
def new_map(discard_changes: bool = False) -> dict:
	"""Start an empty untitled map, like File > New. Same unsaved-changes rule as open_map."""
	return service.new_map(discard_changes=discard_changes)


@mcp.tool()
def save_map(path: str | None = None, format: Literal["native", "streamer"] | None = None,
             text_encoding: Literal["windows-1252", "utf-8"] | None = None) -> dict:
	"""Save the map as Pawn like Ctrl+S; a path works like Save As and becomes the map's path.

	format: native (CreateObject...) or streamer (CreateDynamicObject...);
	omitted keeps the map's format. text_encoding: windows-1252 (what SA-MP and
	open.mp clients draw; the default) or utf-8; omitted keeps the map's. Text
	you pass to the tools is always UTF-8. An untitled map needs a path. Commit
	or roll back the edit session first."""
	return service.save_map(path, format, text_encoding)


PLACE_DOC = inspect.cleandoc("""
	model: an object or vehicle model id/name (required for those kinds); the
	skin for actor, class and npc (default 0, CJ); the pickup model (default
	1239); omitted for label, gangzone, mapicon, pathpoint, sound, checkpoint and area.
	Labels, gang zones, map icons, path points, sounds and areas have no model: the
	editor and its captures show them as small coloured icons.
	snap (default) puts it on the highest surface below (x, y) (an icon kind at
	that point, peds 1 m above it), so a roof, awning or lamp post counts too:
	check with capture_view.
	snap=False uses z. heading: degrees, counter-clockwise from north.
	Settings go in properties in the same call, e.g. a tuned car:
	  kind="vehicle", model=560, x=..., y=..., properties={"color1": 1, "mods": [1010, 1080]}
	an actor with an animation:
	  kind="actor", model=50, properties={"anim_lib": "PED", "anim_name": "IDLE_CHAT", "anim_loop": true}
	Path points join the path named in properties.path in placement order; an
	NPC with properties.path walks it. The first edit starts an edit session.
	""")


@mcp.tool(description=inspect.cleandoc("""Place one map element and return it with its instance_id.
	""") + "\n\n" + PLACE_DOC + "\n\n" + ELEMENT_PROPERTIES_DOC)
def place_element(kind: Literal["object", "vehicle", "pickup", "label", "actor", "class",
                                "gangzone", "mapicon", "npc", "pathpoint", "sound",
                                "checkpoint", "area"],
                  x: float, y: float, z: float = 0.0, heading: float = 0.0,
                  model: int | str | None = None, snap: bool = True,
                  properties: dict | None = None) -> dict:
	return service.place_element(kind, x, y, z, heading=heading, model=model, snap=snap,
	                             properties=properties)


@mcp.tool(description=inspect.cleandoc("""Place several elements in one call: a list of objects with the fields of place_element
	(kind, x, y and optionally z, heading, model, snap, properties). All or nothing:
	if one fails, none stay and the error says which. Returns their instance_ids, e.g.
	  [{"kind": "pathpoint", "x": 1, "y": 2, "properties": {"path": "patrol"}}, ...,
	   {"kind": "npc", "model": 280, "x": 1, "y": 2, "properties": {"npc_name": "Guard", "path": "patrol"}}]
	""") + "\n\n" + PLACE_DOC + "\n\n" + ELEMENT_PROPERTIES_DOC)
def place_elements(elements: list[dict]) -> dict:
	return service.place_elements(elements)


@mcp.tool()
def list_prefabs() -> dict:
	"""List the prefab library: Mine (Haumea's config folder), Workspace (the server's prefabs/),
	Starter (shipped with the editor) and their subfolders, with instance, element and vehicle
	counts and tags."""
	return service.list_prefabs()


@mcp.tool()
def place_prefab(prefab: str, x: float, y: float, z: float = 0.0, heading: float = 0.0,
                 snap: bool = True, group: str | None = None) -> dict:
	"""Place a prefab in the edit session by name, folder/name or path; returns the new instance ids.

	heading turns it in degrees about Z. snap puts it on the ground below (x, y). group, when set,
	puts everything placed in that group."""
	return service.place_prefab(prefab, x, y, z, heading=heading, snap=snap, group=group)


@mcp.tool()
def save_prefab(name: str, instance_ids: list[int], folder: str = "", tags: str = "",
                origin: str = "lowest", where: str = "mine") -> dict:
	"""Save instances as a prefab file (.haumea) in Mine or the workspace's prefabs/, optionally in a
	subfolder, with comma-separated tags. origin is what is held when it is placed: lowest (the middle,
	at the bottom), centre or first (the first id). TextDraws have their own file, not prefabs."""
	return service.save_prefab(name, instance_ids, folder=folder, tags=tags, origin=origin, where=where)


@mcp.tool(description=inspect.cleandoc("""Change settings of one element in the edit session; all or nothing, with undo.

	Names are checked (Pawn identifiers, unique NPC names) and keys must fit the
	element's kind. Returns the element as listed.

	""") + "\n\n" + ELEMENT_PROPERTIES_DOC)
def set_element_properties(instance_id: int, properties: dict) -> dict:
	return service.set_element_properties(instance_id, properties)


@mcp.tool()
def map_tool(op: Literal["array", "mirror", "align", "distribute", "snap_rotations",
                         "drop", "relative", "lay_along", "radar_art", "duplicate"],
             ids: list[int] | None = None, count: int | None = None, radial: bool | None = None,
             offset: list[float] | None = None, rotate: list[float] | None = None,
             centre: list[float] | None = None, angle: float | None = None,
             rows: int | None = None, row_offset: list[float] | None = None,
             axis: Literal["x", "y", "z"] | None = None,
             around: str | None = None,
             mode: Literal["min", "centre", "max"] | None = None, step: float | None = None,
             attach: bool | None = None, copy: bool | None = None,
             points: list[list[float]] | None = None,
             model: int | None = None, spacing: float | None = None,
             heading: float | None = None, along_length: bool | None = None,
             follow_slope: bool | None = None, on_ground: bool | None = None,
             radar_art: dict | None = None) -> dict:
	"""Run one of the Tools window's transform tools on ids (the first is the reference); one undo step.

	array: count copies; linear (offset [x,y,z] m and rotate [x,y,z] degrees per copy,
	  turning around the group centre) or radial=true (around centre [x,y,z], spread over
	  angle degrees; 360 is a full circle including the originals). Linear with rows > 1
	  repeats the whole line rows times, row_offset [x,y,z] m apart, for a grid.
	duplicate: one copy of ids moved by offset [x,y,z] m and turned by rotate [x,y,z]
	  degrees around the group centre; local=true uses the first object's own axes.
	mirror: axis x or y, around selection (its centre), origin or a coordinate on that
	  axis (e.g. "1479.6"); rotations are fixed so
	  ramps and stairs face the mirrored way. copy=true mirrors a copy (for symmetric
	  builds) and returns the originals plus the copies.
	align: axis x/y/z, mode min/centre/max of the bounding boxes, to the first id.
	distribute: even spacing along axis between the two outermost objects.
	snap_rotations: round every angle to step degrees.
	drop: each onto the ground below it.
	relative: Pawn for ids relative to the first (attach=true: AttachObjectToObject;
	  false: CreateObject at x/y/z plus offsets); changes nothing.
	lay_along (no ids): copies of model every spacing metres (default: the model's
	  length) along points [[x,y,z], ...], facing along the line (+heading degrees);
	  along_length (default true) turns models longer in X, like fences, to run along
	  it; follow_slope tilts them, on_ground (default true) drops them.
	radar_art (ids optional): gang zones drawn from the objects' top-down
	  footprints, grouped "radar art", one undo step. The objects are ids, else
	  radar_art.group, else every scripted object. radar_art keys: cell [4 m] or fit
	  (the smallest cell needing at most that many zones), shape [model; collision],
	  above_water [true: leave out what is below the water's surface],
	  colours [radar: the game's map colours by surface; texture, one, group],
	  colour [0x8C8C8C00; one colour, or the fallback], num_colours [6], alpha [200],
	  outline [false], outline_colour [0x20202000], height_shading [true; not with
	  radar colours],
	  height_levels [3], zone_mode [global (1024 on the server); player or streamed:
	  per-player zones, open.mp only], out_group ["radar art"], replace [true: delete
	  that group's zones first; locked ones stop it], apply [true; false only counts].
	  Unknown keys and values that don't fit are errors. Returns zones, created,
	  cell, objects and cells.
	model is a Pawn model id (custom models are negative). Returns the objects afterwards
	(with copies or new objects) as ids, or the Pawn; the selection is left empty."""
	return service.map_tool(op, ids, count=count, radial=radial, offset=offset, rotate=rotate,
	                        centre=centre, angle=angle, rows=rows, row_offset=row_offset, axis=axis, around=around, mode=mode,
	                        step=step, attach=attach, copy=copy, points=points, model=model,
	                        spacing=spacing, heading=heading, along_length=along_length,
	                        follow_slope=follow_slope,
	                        on_ground=on_ground, radar_art=radar_art)


@mcp.tool()
def test_in_game(action: Literal["run", "status", "stop", "restart"] = "status",
                 log_lines: int = 40) -> dict:
	"""File > Test in game: run the map as it is now on a private open.mp test server.

	run exports the map (unsaved edits included; the map file is not written),
	compiles it with the server's pawncc and starts or reloads the server; it
	takes a few seconds, so poll status until state is running (or idle with
	compiler errors). Returns state, status, compiler messages (file:line: text)
	and the server log's last log_lines lines. The server package is set in
	Preferences > Test server or comes from the workspace server."""
	return service.test_in_game(action, log_lines)


@mcp.tool()
def move_element(instance_id: int, x: float | None = None, y: float | None = None,
                 z: float | None = None, heading: float | None = None,
                 pitch: float | None = None, roll: float | None = None, snap: bool = False) -> dict:
	"""Move and/or rotate one element; anything left out keeps its current value.

	Angles in degrees (heading counter-clockwise from north; pitch/roll only
	matter for objects and vehicles). snap=True drops it onto the ground below
	(x, y). E.g. move_element(id, heading=90) turns it in place. Returns the
	element's new position."""
	return service.move_element(instance_id, x=x, y=y, z=z, heading=heading,
	                            pitch=pitch, roll=roll, snap=snap)


@mcp.tool()
def delete_element(instance_id: int) -> dict:
	"""Delete one element of the map, or remove a GTA world building (exported as RemoveBuildingForPlayer)."""
	return service.delete_element(instance_id)


TEXTDRAW_FIELDS_DOC = inspect.cleandoc("""
	TextDraw fields (SA-MP's 640x448 screen canvas; colours are 0xRRGGBBAA):
	- name: the Pawn variable (empty makes one up); per_player: CreatePlayerTextDraw instead of TextDrawCreate
	- x, y: position; text: the text (~n~ new line, ~r~ ~g~ ~b~ ~y~ ~p~ ~w~ ~l~ colours, ~h~ lighter, _ blank),
	  or "TXD:texture" for a sprite (font 4), e.g. "LD_BEAT:chit"
	- font: 0 Beckett, 1 Aharoni, 2 Bank Gothic, 3 Pricedown, 4 sprite, 5 model preview
	- letter_x, letter_y: letter size (default 0.48, 1.12); size_x, size_y: TextDrawTextSize
	  (left alignment: box right edge and wrap; centre: size_y is the width; sprites/models: width, height; a negative one mirrors a sprite)
	- alignment: 1 left, 2 centre, 3 right; colour, use_box, box_colour, background_colour (shadow/outline,
	  or a model preview's background); shadow, outline: sizes; proportional, selectable: true/false
	- locked: true/false, keeps it from being dragged on the user's canvas; hidden: true/false, not drawn
	  in the editor (still in the file); group: a name shared by textdraws edited together
	- preview_model, preview_rot ([x, y, z] degrees), preview_zoom, preview_colours ([colour1, colour2])
	""")


def _textdraw_lines(properties: dict | None) -> list[str]:
	lines = []
	for key, value in (properties or {}).items():
		key = str(key).strip().lower()
		if not key or "=" in key:
			raise ValueError(f"invalid textdraw field {key!r}")
		if isinstance(value, bool):
			text = "1" if value else "0"
		elif isinstance(value, (list, tuple)):
			text = ",".join(str(v) for v in value)
		elif key in ("text", "name"):
			text = service._material_field(str(value), key)
		else:
			text = str(value)
		lines.append(f"{key}={text}")
	return lines


@mcp.tool()
def search_textures(query: str, limit: int = 50) -> dict:
	"""Find textures by name or TXD across the game's, SA-MP's and the workspace's custom model TXDs.

	Each result has model, txd and texture: the model is one whose own TXD it
	is, so SetObjectMaterial(obj, slot, model, txd, texture, ...) resolves in
	game (pass them to set_object_material). samp marks SA-MP's textures,
	custom the workspace's custom models, and tstudio_index is the texture's
	number in Texture Studio's list (0 when it has none). tiles (across, down or both)
	marks textures that repeat without a seam, the ones for floors, walls and long surfaces."""
	return service.engine("texture_search", [query, int(limit)])


@mcp.tool()
def filter_assets(filters: str = "", name: str = "", limit: int = 200,
                  models: list[int] | None = None) -> dict:
	"""Find models by what they are, with the Object Browser's filters, or get those facts for models.

	filters is key=value pairs joined by ";", e.g. "size=2-10;collision=with;city=san_fierro;sort=polygons".
	Ranges are min-max (either side may be empty): size (longest side, m), width (X), depth (Y),
	height (Z), volume (m3), footprint (m2), draw_distance (m), polygons, materials.
	Lists (any of): source=gta,samp,custom; type=object,building,dynamic,breakable,timed,animated,
	upgrade,interior,exterior,alpha,explosive; city=los_santos,san_fierro,las_venturas,countryside,interiors,
	not_placed; zone=<names from the game, e.g. Ganton,Doherty>; mine=favourites,recent,open_map,workspace.
	Others: ide=<text in the IDE path>, txd=<text>, texture=<text in a texture name>,
	collision=with|without, two_sided=1, lod=hd|lod, has_lod=1|0, hour=0-23 (timed models shown then),
	sort=id|name|size|polygons|draw_distance|recent, descending=1. name narrows to model names containing it.

	Each model has model (the Pawn id), name, category, source, ide, txd, size [x, y, z] of its
	collision, collision, draw_distance, flags, hours (timed), placements (world instances), cities,
	zones, polygons and materials (-1 until read; filters on them read every model first, which takes a
	few seconds once). total is how many passed; at most limit come back. models=[ids] instead returns
	those models' facts."""
	if models:
		return service.engine("model_meta", [",".join(str(int(m)) for m in models)])
	return service.engine("model_filter", [filters, int(limit), name])


@mcp.tool()
def list_textdraws() -> dict:
	"""List the textdraw file's textdraws with their index and every field (read-only).

	Textdraws live in their own .pwn/.inc file, apart from the map; file gives
	its path, unsaved_changes, its function prefix (prefix_CreateTextDraws /
	prefix_ShowTextDraws) and read_only_in_map: textdraws an old map still holds."""
	return service.engine("textdraws", [])


@mcp.tool()
def textdraw_file(action: Literal["new", "open", "save", "move_from_map"], path: str | None = None,
                  discard_changes: bool = False) -> dict:
	"""The textdraw file, like the TextDraws window's file buttons.

	new starts an untitled file; open reads a .pwn/.inc (any Pawn file's textdraws);
	both refuse unsaved textdraw changes unless discard_changes, and like move_from_map
	need no edit session open (save_map commits one). save writes the
	file, to path when given (needed while untitled). move_from_map moves an old
	map's read-only textdraws into the file and saves it: into the open file when it
	has one (another path is refused), else to path or <map>_textdraws.pwn. Save the
	map afterwards to take them out of it."""
	command = {"new": "textdraw_new", "open": "textdraw_open", "save": "textdraw_save",
	           "move_from_map": "textdraw_move_from_map"}[action]
	if action == "open" and not path:
		raise ValueError("open needs a path")
	if action in ("new", "open"):
		lines = ([path] if action == "open" else []) + (["true"] if discard_changes else [])
	else:
		lines = [path or ""]
	return service.engine(command, lines)


@mcp.tool(description=inspect.cleandoc("""Add a textdraw to the textdraw file (needs an edit session); returns it with its index.

	Without fields it is a "Text" at the screen's centre with SA-MP's defaults.
	Session rollback restores the textdraws as they were when the session began.
	""") + "\n\n" + TEXTDRAW_FIELDS_DOC)
def add_textdraw(properties: dict | None = None) -> dict:
	return service.engine("textdraw_add", _textdraw_lines(properties))


@mcp.tool(description=inspect.cleandoc("""Change fields of one textdraw by index (needs an edit session).
	""") + "\n\n" + TEXTDRAW_FIELDS_DOC)
def set_textdraw(index: int, properties: dict) -> dict:
	if not properties:
		raise ValueError("properties must name at least one field")
	return service.engine("textdraw_set", [int(index), *_textdraw_lines(properties)])


@mcp.tool()
def delete_textdraw(index: int) -> dict:
	"""Delete one textdraw by index (needs an edit session); later indexes move down by one."""
	return service.engine("textdraw_delete", [int(index)])


@mcp.tool()
def undo_edit(steps: int = 1, redo: bool = False) -> dict:
	"""Step back (or forward with redo=True) through the editor's undo history, the same as Ctrl+Z/Ctrl+Y."""
	return service.engine("redo" if redo else "undo", [steps])


def main():
	mcp.run(transport="stdio")


if __name__ == "__main__":
	main()
