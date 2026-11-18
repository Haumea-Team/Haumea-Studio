// SPDX-License-Identifier: GPL-2.0-or-later
#include "layoutpaths.h"
#include "haumea.h"
#include "i18n.h"
#include "imgui/imgui_internal.h"
#include "layoutmath.h"
#include "maptools.h"
#include "shortcuts.h"
#include "xmlfields.h"
#include <cmath>
#include <map>
#include <set>

namespace
{
using Knot = LayoutMath::Knot;
using Point = LayoutMath::Point;
using EditorXML::Node;
Point point(rw::V3d p)
{
	return {p.x, p.y, p.z};
}
rw::V3d vector(Point p)
{
	return {p[0], p[1], p[2]};
}
struct LayoutGuide {
	int id = 0, area = 0, model = 1228;
	std::string name;
	std::vector<Knot> knots;
	bool curved = true, closed = false, live = false, follow = true, ground = false, alongLength = true;
	float spacing = 3, heading = 0, bank = 0;
	int count = 0;	// objects spread evenly over the range; 0 places them every spacing metres
	float from = 0, to = 1;	// the stretch of the path that gets objects, as fractions of its length
	float sideOffset = 0, heightOffset = 0;	// metres across and above the curve, banking with it
	std::vector<int> objects;
};
struct Pose {
	rw::V3d p;
	rw::Quat q;
	bool deleted;
};
struct State {
	std::vector<LayoutGuide> paths;
	std::map<int, Pose> objects;
	int active = -1;
};
std::vector<LayoutGuide> paths;
std::set<int> owned;
int active = -1, selected = -1, handle = 0, hoverPoint = -1, hoverHandle = 0, nextId = 1;
bool show = true, editing = false, picking = false, visible = false, changed = false;
int direct = -1, panelFrame = -100;
ImVec2 mouseStart;
rw::V3d dragStart, planeNormal;
std::shared_ptr<State> before;
LayoutGuide *current()
{
	return active >= 0 && active < (int)paths.size() ? &paths[active] : nullptr;
}
bool project(rw::V3d p, ImVec2 *out)
{
	rw::V3d s;
	float w, h;
	if (!Sprite::CalcScreenCoors(p, &s, &w, &h, false))
		return false;
	*out = EditorUiPoint(s.x, s.y);
	return true;
}
std::shared_ptr<State> snapshot()
{
	auto state = std::make_shared<State>();
	state->paths = paths;
	state->active = active;
	for (int id : owned)
		if (ObjectInst *o = GetInstanceByID(id))
			if (!o->m_isDeleted)
				state->objects[id] = {o->m_translation, o->m_rotation, false};
	return state;
}
void beginEdit()
{
	if (!before)
		before = snapshot();
}
void restore(const State &state)
{
	for (int id : owned)
		if (ObjectInst *o = GetInstanceByID(id)) {
			auto it = state.objects.find(id);
			if (it == state.objects.end()) {
				if (!o->m_isDeleted)
					SetLayoutObjectState(o, o->m_translation, o->m_rotation, true);
			} else
				SetLayoutObjectState(o, it->second.p, it->second.q, it->second.deleted);
		}
	paths = state.paths;
	active = state.active;
	selected = std::min(selected, current() ? (int)current()->knots.size() - 1 : -1);
	BumpChangeSeq();
}
float spacingFor(const LayoutGuide &p)
{
	if (p.count < 2)
		return p.spacing;
	float length = LayoutMath::curveLength(p.knots, p.curved, p.closed) * (p.to - p.from);
	bool wrap = p.closed && p.from <= 0 && p.to >= 1;
	return std::max(.01f, length / (p.count - (wrap ? 0 : 1)));
}
std::vector<LayoutMath::Sample> samples(const LayoutGuide &p)
{
	return LayoutMath::sample(p.knots, p.curved, p.closed, spacingFor(p), p.count >= 2 ? (size_t)p.count : 2000,
	                          p.from, p.to);
}
// The curve's frame at a sample, with the bank and the points' tilt.
void frame(const LayoutGuide &p, const LayoutMath::Sample &s, Point &forward, Point &right, Point &normal)
{
	using namespace LayoutMath;
	forward = p.follow ? s.tangent : unit({s.tangent[0], s.tangent[1], 0});
	normal = p.follow ? s.normal : Point{0, 0, 1};
	float bank = (p.bank + s.tilt) * 3.14159265f / 180;
	normal = add(mul(normal, std::cos(bank)), mul(cross(forward, normal), std::sin(bank)));
	right = unit(cross(forward, normal));
	normal = unit(cross(right, forward));
}
rw::V3d placement(const LayoutGuide &p, const LayoutMath::Sample &s)
{
	using namespace LayoutMath;
	Point forward, right, normal;
	frame(p, s, forward, right, normal);
	rw::V3d pos = vector(add(s.p, add(mul(right, p.sideOffset), mul(normal, p.heightOffset))));
	if (p.ground) {
		rw::V3d hit;
		if (GetGroundPlacementSurface(pos, &hit, nullptr, true))
			pos.z = hit.z + GetPlacementBaseOffset(p.model);
	}
	return pos;
}
rw::Quat rotation(const LayoutGuide &p, const LayoutMath::Sample &s)
{
	using namespace LayoutMath;
	Point forward, right, normal;
	frame(p, s, forward, right, normal);
	bool x = false;
	MapToolsModelLength(p.model, &x);
	if (p.alongLength && x) {
		right = forward;
		forward = unit(cross(normal, right));
	}
	float h = p.heading * 3.14159265f / 180;
	Point r = add(mul(right, std::cos(h)), mul(forward, std::sin(h)));
	Point f = add(mul(forward, std::cos(h)), mul(right, -std::sin(h)));
	rw::Matrix m;
	m.setIdentity();
	m.right = vector(r);
	m.up = vector(f);
	m.at = vector(normal);
	return QuatFromMatrix(m);
}
void setPose(ObjectInst *o, rw::V3d pos, rw::Quat q, bool deleted)
{
	rw::V3d old = o->m_translation;
	rw::Quat oldq = o->m_rotation;
	SetLayoutObjectState(o, pos, q, deleted);
	if (o->m_lod && owned.count(o->m_lod->m_id)) {
		rw::Quat delta = rw::mult(rw::conj(oldq), q);
		rw::V3d offset = rw::rotate(sub(o->m_lod->m_translation, old), rw::conj(delta));
		SetLayoutObjectState(o->m_lod, add(pos, offset), rw::mult(o->m_lod->m_rotation, delta), deleted);
	}
}
void sync(LayoutGuide &p, bool reconcile)
{
	if (!p.live)
		return;
	auto result = samples(p);
	if (result.empty() || !GetObjectDef(p.model))
		return;
	// Deleted instances remain deleted while previewing. Rebuild replaces gaps on commit.
	if (reconcile)
		p.objects.erase(std::remove_if(p.objects.begin(), p.objects.end(),
		                               [](int id) {
			                               auto *o = GetInstanceByID(id);
			                               return !o || o->m_isDeleted;
		                               }),
		                p.objects.end());
	int oldModel = GetSpawnObjectId(), oldKind = GetSpawnElementKind();
	if (reconcile)
		SetSpawnObjectId(p.model);
	for (size_t i = 0; i < result.size(); i++) {
		rw::V3d pos = placement(p, result[i]);
		rw::Quat q = rotation(p, result[i]);
		if (i < p.objects.size()) {
			if (ObjectInst *o = GetInstanceByID(p.objects[i]))
				if (!o->m_isDeleted)
					setPose(o, pos, q, false);
		} else if (reconcile) {
			ObjectInst *made[2];
			int n = SpawnPlaceObjectNoUndo(pos, &q, made, 2);
			if (n <= 0)
				break;
			// Placement returns the linked LOD first, then the actual requested model.
			p.objects.push_back(made[n - 1]->m_id);
			for (int j = 0; j < n; j++) {
				owned.insert(made[j]->m_id);
				made[j]->m_area = p.area;
				auto props = *GetObjectPawnProperties(made[j]);
				snprintf(props.group, sizeof(props.group), "%s", p.name.c_str());
				SetObjectPawnProperties(made[j], props, false);
			}
		}
	}
	if (reconcile) {
		for (size_t i = result.size(); i < p.objects.size(); i++)
			if (ObjectInst *o = GetInstanceByID(p.objects[i]))
				setPose(o, o->m_translation, o->m_rotation, true);
		if (p.objects.size() > result.size())
			p.objects.resize(result.size());
		SetSpawnObjectId(oldModel);
		SetSpawnElementKind(oldKind);
	}
}
void finishEdit()
{
	if (!before)
		return;
	if (changed) {
		if (LayoutGuide *p = current())
			sync(*p, true);
		UndoRecordLayoutPaths(before, snapshot());
		BumpChangeSeq();
	}
	before.reset();
	changed = false;
}
void cancelEdit()
{
	if (before)
		restore(*before);
	before.reset();
	changed = false;
	direct = -1;
	picking = false;
}
void touched(LayoutGuide &p)
{
	LayoutMath::automaticHandles(p.knots, p.closed);
	changed = true;
	sync(p, false);
}
rw::V3d handlePosition(const LayoutGuide &p, int index, int which)
{
	const Knot &k = p.knots[index];
	if (which == 2)
		return vector(p.curved ? LayoutMath::bezier(k, p.knots[(index + 1) % p.knots.size()], .5f)
		                       : LayoutMath::mix(k.p, p.knots[(index + 1) % p.knots.size()].p, .5f));
	return vector(which == 0 ? k.p : LayoutMath::add(k.p, which == 1 ? k.out : k.in));
}
void moveHandle(LayoutGuide &p, rw::V3d pos)
{
	using namespace LayoutMath;
	Knot &k = p.knots[selected];
	if (handle == 0)
		k.p = point(pos);
	else {
		// Dragging a computed handle takes it over, keeping the other in line.
		if (k.handles == HANDLES_AUTO || k.handles == HANDLES_VECTOR)
			k.handles = HANDLES_ALIGNED;
		Point d = sub(point(pos), k.p);
		Point &moved = handle == 1 ? k.out : k.in, &other = handle == 1 ? k.in : k.out;
		moved = d;
		if (k.handles == HANDLES_ALIGNED) {
			float keep = length(other);
			other = mul(unit(d, {1, 0, 0}), -(keep > .001f ? keep : length(d)));
		}
	}
	touched(p);
}
void insertMiddle(LayoutGuide &p, int segment)
{
	size_t next = (segment + 1) % p.knots.size();
	Knot k;
	if (p.curved)
		k = LayoutMath::split(p.knots[segment], p.knots[next]);
	else
		k.p = LayoutMath::mix(p.knots[segment].p, p.knots[next].p, .5f);
	p.knots.insert(p.knots.begin() + segment + 1, k);
	selected = segment + 1;
	handle = 0;
	changed = true;
}
void createShape(int shape, int axis, float radius, float turns, float rise, const rw::V3d *at = nullptr)
{
	SpawnExitPlaceMode();
	ExitBrushMode();
	ExitPrefabPlaceMode();
	gRectSelectActive = false;
	beginEdit();
	LayoutGuide p;
	p.id = nextId++;
	p.name = std::string(_("Path")) + " " + std::to_string(p.id);
	p.area = currentArea;
	p.curved = shape != 0;
	p.closed = shape == 2;
	Point origin = point(at ? *at : TheCamera.m_target);
	if (shape < 2) {
		for (int i = 0; i < 3; i++) {
			Knot k;
			k.p = LayoutMath::add(origin, {(i - 1) * 20.f, 0, (shape == 1 && i == 1) ? 8.f : 0.f});
			p.knots.push_back(k);
		}
		LayoutMath::automaticHandles(p.knots, false);
	} else {
		int segments = shape == 2 ? 4 : std::max(4, std::min(64, (int)std::ceil(turns * 8)));
		float sweep = (shape == 2 ? 1 : turns) * 6.283185307f;
		Point a = axis == 0 ? Point{0, 1, 0} : Point{1, 0, 0},
		      b = axis == 2 ? Point{0, 1, 0} : Point{0, 0, 1};
		if (axis != 2)
			origin[2] += radius;
		float step = sweep / segments, kappa = 4.f / 3 * std::tan(step / 4);
		for (int i = 0; i < (p.closed ? segments : segments + 1); i++) {
			float angle = i * step - (axis == 2 ? 0 : 1.570796327f);
			Knot k;
			k.handles = LayoutMath::HANDLES_ALIGNED;
			k.p = LayoutMath::add(origin, LayoutMath::add(LayoutMath::mul(a, radius * std::cos(angle)),
			                                              LayoutMath::mul(b, radius * std::sin(angle))));
			k.p[axis] += shape == 3 ? rise * i / segments : 0;
			k.out = LayoutMath::add(LayoutMath::mul(a, -radius * std::sin(angle) * kappa),
			                        LayoutMath::mul(b, radius * std::cos(angle) * kappa));
			if (shape == 3)
				k.out[axis] += rise / segments / 3;
			k.in = LayoutMath::mul(k.out, -1);
			p.knots.push_back(k);
		}
	}
	paths.push_back(p);
	active = (int)paths.size() - 1;
	selected = shape == 1 ? 1 : 0;
	handle = 0;
	show = editing = true;
	changed = true;
	finishEdit();
}
} // namespace

void ResetLayoutPaths()
{
	paths.clear();
	owned.clear();
	active = selected = -1;
	handle = 0;
	hoverPoint = -1;
	visible = false;
	show = true;
	before.reset();
	direct = -1;
	picking = editing = false;
	nextId = 1;
	changed = false;
}
void RestoreLayoutPaths(const std::shared_ptr<void> &value)
{
	if (value) {
		restore(*(const State *)value.get());
		before.reset();
		changed = false;
		direct = -1;
	}
}
int LayoutPathCount()
{
	return (int)paths.size();
}
void LayoutPathsReferencedObjects(std::vector<int> &out)
{
	for (const auto &p : paths)
		if (p.live)
			out.insert(out.end(), p.objects.begin(), p.objects.end());
}
float LayoutPathPointHeight(int i)
{
	auto *p = current();
	return p && i >= 0 && i < (int)p->knots.size() ? p->knots[i].p[2] : 0;
}
int LayoutPathObjectCount()
{
	auto *p = current();
	return p ? (int)p->objects.size() : 0;
}
int LayoutPathUpsideDownCount()
{
	int n = 0;
	if (auto *p = current())
		for (int id : p->objects)
			if (auto *o = GetInstanceByID(id))
				if (!o->m_isDeleted && o->m_matrix.at.z < -.9f)
					n++;
	return n;
}
bool LayoutPathObjectsMatch()
{
	auto *p = current();
	if (!p || !p->live)
		return false;
	auto result = samples(*p);
	if (result.size() != p->objects.size())
		return false;
	for (size_t i = 0; i < result.size(); i++) {
		auto *o = GetInstanceByID(p->objects[i]);
		if (!o || o->m_isDeleted || o->m_objectId != p->model)
			return false;
		rw::V3d expected = placement(*p, result[i]);
		if (length(sub(o->m_translation, expected)) > .005f)
			return false;
		if (o->m_lod && (o->m_lod->m_isDeleted || length(sub(o->m_lod->m_translation, expected)) > .005f))
			return false;
		rw::Quat q = rotation(*p, result[i]);
		if (std::abs(q.x * o->m_rotation.x + q.y * o->m_rotation.y + q.z * o->m_rotation.z +
		             q.w * o->m_rotation.w) < .99999f)
			return false;
	}
	return true;
}
bool LayoutPathPointScreen(ImVec2 *out, int i, int which)
{
	auto *p = current();
	return p && i >= 0 && i < (int)p->knots.size() && project(handlePosition(*p, i, which), out);
}
bool LayoutPathsOwnsMouse()
{
	return visible && editing && (picking || direct >= 0 || hoverPoint >= 0);
}

bool LayoutPathsHandleViewport()
{
	if (!visible || !editing)
		return false;
	auto *p = current();
	if (!p)
		return false;
	if (ShortcutPressed(SC_CANCEL)) {
		cancelEdit();
		editing = false;
		return true;
	}
	ImGuiIO &io = ImGui::GetIO();
	if (io.WantCaptureMouse && direct < 0)
		return false;
	if (picking && CPad::IsMButtonClicked(1)) {
		Ray ray;
		ray.start = TheCamera.m_position;
		ray.dir = normalize(TheCamera.m_mouseDir);
		rw::V3d pos;
		if (!GetVisibleInstUnderRay(ray, &pos)) {
			float t = std::abs(ray.dir.z) > .0001f ? (TheCamera.m_target.z - ray.start.z) / ray.dir.z : 50;
			pos = add(ray.start, scale(ray.dir, clamp(t, 1.f, 5000.f)));
		}
		if (p->knots.size() < 256) {
			beginEdit();
			Knot k;
			k.p = point(pos);
			p->knots.push_back(k);
			selected = (int)p->knots.size() - 1;
			touched(*p);
			finishEdit();
		}
		return true;
	}
	if (CPad::IsMButtonClicked(3) && picking) {
		picking = false;
		return true;
	}
	if (hoverPoint >= 0 && CPad::IsMButtonJustDown(1) && !gGizmoUsing) {
		beginEdit();
		selected = hoverPoint;
		handle = hoverHandle;
		if (handle == 2 && p->knots.size() < 256)
			insertMiddle(*p, selected);
		direct = selected;
		mouseStart = io.MousePos;
		dragStart = handlePosition(*p, selected, handle);
		planeNormal = normalize(sub(TheCamera.m_target, TheCamera.m_position));
	}
	if (direct >= 0) {
		float dx = io.MousePos.x - mouseStart.x, dy = io.MousePos.y - mouseStart.y;
		if (dx * dx + dy * dy > 4) {
			rw::V3d pos = dragStart;
			if (io.KeyShift) {
				float depth = std::max(1.f, length(sub(dragStart, TheCamera.m_position)));
				pos.z -= dy * 2 * depth * std::tan(TheCamera.m_fov * 3.14159265f / 360) /
				         ImGui::GetMainViewport()->Size.y;
			} else {
				rw::V3d dir = normalize(TheCamera.m_mouseDir);
				float d = dot(dir, planeNormal);
				if (std::abs(d) > .0001f) {
					float t = dot(sub(dragStart, TheCamera.m_position), planeNormal) / d;
					if (t > 0)
						pos = add(TheCamera.m_position, scale(dir, t));
				}
			}
			if (gGizmoSnap && io.KeyCtrl)
				for (int a = 0; a < 3; a++)
					(&pos.x)[a] = std::round((&pos.x)[a] / gGizmoSnapTranslate) * gGizmoSnapTranslate;
			moveHandle(*p, pos);
		}
		if (!CPad::IsMButtonDown(1)) {
			direct = -1;
			finishEdit();
		}
		return true;
	}
	return picking || hoverPoint >= 0;
}

bool LayoutPathsGizmo()
{
	LayoutGuide *p = current();
	if (!visible || !editing || !p || selected < 0 || selected >= (int)p->knots.size())
		return false;
	if (direct >= 0 || picking) {
		gGizmoUsing = direct >= 0;
		return true;
	}
	rw::V3d original = handlePosition(*p, selected, handle);
	rw::Matrix matrix;
	matrix.setIdentity();
	matrix.pos = original;
	rw::RawMatrix raw;
	rw::convMatrix(&raw, &matrix);
	rw::Camera *cam = Scene.camera;
	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGuizmo::SetRect(vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y);
	float snap[3] = {gGizmoSnapTranslate, gGizmoSnapTranslate, gGizmoSnapTranslate};
	ImGuizmo::Manipulate((float *)&cam->devView, (float *)&cam->devProj, ImGuizmo::TRANSLATE, ImGuizmo::WORLD,
	                     (float *)&raw, nullptr, gGizmoSnap && CPad::IsShiftDown() ? snap : nullptr);
	gGizmoHovered = ImGuizmo::IsOver();
	gGizmoUsing = ImGuizmo::IsUsing();
	rw::V3d pos = {raw.pos.x, raw.pos.y, raw.pos.z};
	if (length(sub(pos, original)) > .00001f) {
		beginEdit();
		moveHandle(*p, pos);
	}
	return true;
}

void LayoutPathsOverlay(bool enabled)
{
	visible = enabled && show;
	hoverPoint = -1;
	if (!enabled) {
		if (before)
			cancelEdit();
		editing = picking = false;
		return;
	}
	if (picking && ImGui::GetFrameCount() - panelFrame > 2)
		picking = false;
	if (before && direct < 0 && !ImGuizmo::IsUsing() && !ImGui::IsAnyItemActive())
		finishEdit();
	if (!visible)
		return;
	auto *draw = EditorForegroundDrawList();
	float ui = GetUiScale();
	ImVec2 mouse = ImGui::GetIO().MousePos;
	float best = 100 * ui * ui;
	auto line = [&](Point a, Point b, ImU32 color, float width) {
		ImVec2 x, y;
		if (project(vector(a), &x) && project(vector(b), &y))
			draw->AddLine(x, y, color, width * ui);
	};
	auto marker = [&](LayoutGuide &p, int i, int h, Point pos, float r, ImU32 color) {
		ImVec2 s;
		if (!project(vector(pos), &s))
			return;
		draw->AddCircleFilled(s, r * ui, color, 16);
		draw->AddCircle(s, (r + 1) * ui, IM_COL32(15, 25, 30, 230), 16, ui);
		float d = (mouse.x - s.x) * (mouse.x - s.x) + (mouse.y - s.y) * (mouse.y - s.y);
		if (&p == current() && editing && !ImGui::GetIO().WantCaptureMouse && d < best) {
			best = d;
			hoverPoint = i;
			hoverHandle = h;
		}
	};
	for (size_t index = 0; index < paths.size(); index++) {
		LayoutGuide &p = paths[index];
		if (!gNoAreaCull && p.area != currentArea && p.area != 13)
			continue;
		bool edit = editing && (int)index == active;
		ImU32 col = edit ? IM_COL32(70, 215, 220, 235) : IM_COL32(100, 155, 165, 140);
		size_t segments = p.closed ? p.knots.size() : (p.knots.empty() ? 0 : p.knots.size() - 1);
		for (size_t i = 0; i < segments; i++) {
			const Knot &a = p.knots[i], &b = p.knots[(i + 1) % p.knots.size()];
			Point prev = a.p;
			int count = p.curved ? 40 : 1;
			for (int j = 1; j <= count; j++) {
				Point at = p.curved ? LayoutMath::bezier(a, b, (float)j / count) : b.p;
				line(prev, at, col, edit ? 2 : 1);
				prev = at;
			}
			if (edit)
				marker(p, i, 2, point(handlePosition(p, i, 2)), 3, IM_COL32(150, 180, 185, 200));
		}
		if (edit)
			for (size_t i = 0; i < p.knots.size(); i++) {
				const Knot &k = p.knots[i];
				if ((int)i == selected && p.curved)
					for (int h : {-1, 1}) {
						Point v = LayoutMath::add(k.p, h == 1 ? k.out : k.in);
						line(k.p, v, IM_COL32(165, 180, 190, 210), 1);
						marker(p, i, h, v, 4, IM_COL32(240, 195, 100, 255));
					}
				marker(p, i, 0, k.p, (int)i == selected ? 7 : 5,
				       (int)i == selected ? IM_COL32(255, 210, 100, 255) : col);
			}
	}
	if (hoverPoint >= 0) {
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		if (direct < 0)
			ImGui::SetTooltip(
			    "%s", hoverHandle == 2
			              ? _("Drag to bend this segment. Shift drags vertically.")
						  : _("Drag to move. Shift drags vertically; use the gizmo for an exact axis."));
	}
}

// Panel layout helpers, defined in maptools.cpp.
float MapToolsButtonWidth(int n);
float MapToolsWidthBeforeButton(const char *label);
bool MapToolsMainButton(const char *label, const ImVec2 &size = ImVec2(-FLT_MIN, 0.0f));

void LayoutPathsPanel()
{
	panelFrame = ImGui::GetFrameCount();
	static int shape = 1, axis = 0;
	static float radius = 15, turns = 1, rise = 15;
	const char *shapes[] = {_("Line"), _("Curve"), _("Loop"), _("Ramp / helix")};
	UiCombo(L("Shape"), &shape, shapes, 4);
	if (shape >= 2) {
		ImGui::SetNextItemWidth(MapToolsWidthBeforeButton(L("Fit to selection")));
		UiInputFloat(L("Radius"), &radius, 1, 5, "%.2f m");
		radius = std::isfinite(radius) ? clamp(radius, .5f, 1000.f) : 15.f;
		// A whole number of pieces around one turn, so the loop closes without
		// a gap.
		ImGui::SameLine();
		ImGui::BeginDisabled(!selection.first);
		if (ImGui::Button(L("Fit to selection"))) {
			float piece = MapToolsModelLength(((ObjectInst *)selection.first->item)->m_objectId, nullptr);
			if (piece > .05f) {
				float pieces = std::max(3.f, std::round(6.283185307f * radius / piece));
				radius = clamp(pieces * piece / 6.283185307f, .5f, 1000.f);
			}
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip(_("Rounds the radius so a whole number of the selected model's lengths make one turn."));
		const char *axes[] = {_("Around X"), _("Around Y"), _("Around Z")};
		UiCombo(L("Plane"), &axis, axes, 3);
		if (shape == 3) {
			UiInputFloat(L("Turns"), &turns, .25f, 1, "%.2f");
			turns = std::isfinite(turns) ? clamp(turns, .25f, 8.f) : 1.f;
			UiInputFloat(L("Rise"), &rise, 1, 5, "%.2f m");
			rise = std::isfinite(rise) ? clamp(rise, -1000.f, 1000.f) : 15.f;
		}
	}
	ImGui::BeginDisabled(paths.size() >= 128);
	if (ImGui::Button(L("New path"), ImVec2(-FLT_MIN, 0)))
		createShape(shape, axis, radius, turns, rise);
	ImGui::EndDisabled();
	if (paths.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("%s", _("Create a path, then drag its points or segment midpoints in the 3D view."));
		ImGui::PopStyleColor();
		return;
	}
	// The path being edited.
	ImGui::Spacing();
	if (UiBeginCombo(L("Path"), current() ? current()->name.c_str() : "")) {
		for (size_t i = 0; i < paths.size(); i++)
			if (ImGui::Selectable((paths[i].name + "##" + std::to_string(paths[i].id)).c_str(),
			                      (int)i == active)) {
				finishEdit();
				active = i;
				selected = 0;
				handle = 0;
			}
		ImGui::EndCombo();
	}
	LayoutGuide *p = current();
	if (!p)
		return;
	auto old = paths; // Snapshot the values before an activated field changes them.
	bool change = false;
	char name[96];
	snprintf(name, sizeof(name), "%s", p->name.c_str());
	if (UiInputText(L("Name"), name, sizeof(name))) {
		p->name = name;
		change = true;
	}
	// Its on/off options together, then its actions on one row.
	UiCheckbox(L("Show paths"), &show);
	if (UiCheckbox(L("Edit in view"), &editing)) {
		if (editing) {
			SpawnExitPlaceMode();
			ExitBrushMode();
			ExitPrefabPlaceMode();
			gRectSelectActive = false;
		}
		show = true;
		selected = std::max(selected, 0);
		picking = false;
	}
	change |= UiCheckbox(L("Smooth curve"), &p->curved);
	change |= UiCheckbox(L("Closed"), &p->closed);
	float third = MapToolsButtonWidth(3);
	if (ImGui::Button(picking ? L("Stop picking") : L("Add points"), ImVec2(third, 0))) {
		picking = !picking;
		editing = show = true;
		SpawnExitPlaceMode();
		ExitBrushMode();
		ExitPrefabPlaceMode();
	}
	ImGui::SameLine();
	if (ImGui::Button(L("Frame path"), ImVec2(third, 0))) {
		rw::V3d c = {0, 0, 0};
		for (auto &k : p->knots)
			c = add(c, vector(k.p));
		if (!p->knots.empty())
			c = scale(c, 1.f / p->knots.size());
		rw::V3d d = sub(c, TheCamera.m_target);
		TheCamera.m_target = c;
		TheCamera.m_position = add(TheCamera.m_position, d);
	}
	ImGui::SameLine();
	if (ImGui::Button(L("Delete path"), ImVec2(third, 0))) {
		beginEdit();
		p->live = false;
		paths.erase(paths.begin() + active);
		active = paths.empty() ? -1 : std::min(active, (int)paths.size() - 1);
		selected = -1;
		changed = true;
		finishEdit();
		return;
	}
	ImGui::SetItemTooltip(_("Deletes the guide and keeps its generated objects."));
	if (!p->knots.empty() && ImGui::TreeNode(L("Point settings"))) {
		selected = std::max(0, std::min(selected, (int)p->knots.size() - 1));
		std::string label = std::to_string(selected + 1) + " / " + std::to_string(p->knots.size());
		if (UiBeginCombo(L("Control point"), label.c_str())) {
			for (size_t i = 0; i < p->knots.size(); i++)
				if (ImGui::Selectable(std::to_string(i + 1).c_str(), (int)i == selected)) {
					selected = i;
					handle = 0;
				}
			ImGui::EndCombo();
		}
		Knot &k = p->knots[selected];
		change |= UiDragFloat3(L("Point position"), k.p.data(), .1f, -100000.f, 100000.f, "%.3f");
		if (p->curved) {
			const char *modes[] = {_("Automatic"), _("Aligned"), _("Free"), _("Corner")};
			change |= UiCombo(L("Handles"), &k.handles, modes, LayoutMath::NUM_HANDLES);
			ImGui::SetItemTooltip(_("Automatic: smooth through the neighbours. Aligned: opposite each other, each "
			                        "with its own length. Free: independent. Corner: straight at the neighbours."));
			if (k.handles != LayoutMath::HANDLES_AUTO && k.handles != LayoutMath::HANDLES_VECTOR &&
			    ImGui::TreeNode(L("Curve handles"))) {
				change |= UiDragFloat3(L("Incoming"), k.in.data(), .1f);
				change |= UiDragFloat3(L("Outgoing"), k.out.data(), .1f);
				ImGui::TreePop();
			}
		}
		change |= UiDragFloat(L("Tilt"), &k.tilt, .5f, -360, 360, "%.1f");
		ImGui::SetItemTooltip(_("Banks the objects at this point, easing to the next point's tilt."));
		float half = MapToolsButtonWidth(2);
		ImGui::BeginDisabled(p->knots.size() >= 256 || (!p->closed && selected + 1 >= (int)p->knots.size()));
		if (ImGui::Button(L("Insert midpoint"), ImVec2(half, 0))) {
			if (!before) {
				beginEdit();
				before->paths = old;
			}
			insertMiddle(*p, selected);
			change = true;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(p->knots.size() <= 2);
		if (ImGui::Button(L("Remove point"), ImVec2(half, 0))) {
			p->knots.erase(p->knots.begin() + selected);
			selected = std::min(selected, (int)p->knots.size() - 1);
			handle = 0;
			change = true;
		}
		ImGui::EndDisabled();
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx(L("Objects along path"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::BeginDisabled(p->live);
		int model = PawnModelId(p->model);
		ImGui::SetNextItemWidth(MapToolsWidthBeforeButton(L("Use selection")));
		if (UiInputInt(L("Model ID"), &model, 0, 0)) {
			p->model = EditorModelId(model);
			change = true;
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!selection.first);
		if (ImGui::Button(L("Use selection")) && selection.first) {
			p->model = ((ObjectInst *)selection.first->item)->m_objectId;
			change = true;
		}
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		int byCount = p->count >= 2;
		const char *spacingModes[] = {_("Distance"), _("Count")};
		if (UiCombo(L("Spacing by"), &byCount, spacingModes, 2)) {
			p->count = byCount ? std::max(2, (int)samples(*p).size()) : 0;
			change = true;
		}
		if (byCount) {
			change |= UiInputInt(L("Objects"), &p->count, 1, 10);
			p->count = clamp(p->count, 2, 2000);
		} else {
			change |= UiInputFloat(L("Spacing"), &p->spacing, .5f, 2, "%.2f m");
			p->spacing = clamp(p->spacing, .1f, 1000.f);
		}
		float range[2] = {p->from * 100, p->to * 100};
		if (UiDragFloatRange2(L("Range"), &range[0], &range[1], .2f, 0, 100, "%.1f%%", nullptr,
		                           ImGuiSliderFlags_AlwaysClamp)) {
			p->from = clamp(range[0] / 100, 0.f, 1.f);
			p->to = clamp(range[1] / 100, p->from, 1.f);
			change = true;
		}
		ImGui::SetItemTooltip(_("The stretch of the path that gets objects, from its first point to its last."));
		if (ImGui::TreeNode(L("Advanced###path_placement"))) {
			change |= UiCheckbox(L("Along its length"), &p->alongLength);
			change |= UiCheckbox(L("Follow curve rotation"), &p->follow);
			change |= UiCheckbox(L("On ground"), &p->ground);
			ImGui::SetItemTooltip(
			    _("Off keeps the curve's height, so lifting a point also lifts its objects."));
			change |= UiDragFloat(L("Heading"), &p->heading, .5f, -360, 360, "%.1f");
			change |= UiDragFloat(L("Bank"), &p->bank, .5f, -180, 180, "%.1f");
			change |= UiDragFloat(L("Side offset"), &p->sideOffset, .1f, -2000, 2000, "%.2f m");
			ImGui::SetItemTooltip(_("Moves the objects across the path, to its right; negative for its left."));
			change |= UiDragFloat(L("Height offset"), &p->heightOffset, .1f, -2000, 2000, "%.2f m");
			ImGui::TreePop();
		}
		size_t count = samples(*p).size();
		ImGui::TextDisabled(_("%d objects along this path"), (int)count);
		if (count == 2000 && p->count < 2) {
			ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(UI_COLOR_WARNING));
			ImGui::TextWrapped(
			    "%s", _("The layout is limited to 2,000 objects. Increase spacing for a longer path."));
			ImGui::PopStyleColor();
		}
		// Generate (or Rebuild) is the section's main action; Detach shares
		// its row once the objects exist.
		ImVec2 size(p->live ? MapToolsButtonWidth(2) : -FLT_MIN, 0);
		ImGui::BeginDisabled(count == 0 || !GetObjectDef(p->model));
		if (MapToolsMainButton(p->live ? L("Rebuild objects") : L("Generate objects"), size)) {
			if (!before) {
				beginEdit();
				before->paths = old;
			}
			if (!p->live)
				ClearSelection();
			p->live = true;
			change = true;
		}
		ImGui::EndDisabled();
		if (p->live) {
			ImGui::SameLine();
			if (ImGui::Button(L("Detach objects"), size)) {
				p->live = false;
				p->objects.clear();
				change = true;
			}
			ImGui::SetItemTooltip(_("Keeps the objects where they are and stops the path moving them."));
		}
		ImGui::TreePop();
	}
	if (change) {
		bool valid = std::isfinite(p->spacing) && std::isfinite(p->heading) && std::isfinite(p->bank) &&
		             std::isfinite(p->sideOffset) && std::isfinite(p->heightOffset);
		for (const Knot &k : p->knots)
			valid &= std::isfinite(k.tilt);
		for (const Knot &k : p->knots)
			for (Point v : {k.p, k.in, k.out})
				for (float x : v)
					valid &= std::isfinite(x) && std::abs(x) <= 1000000;
		if (!valid) {
			paths = std::move(old);
			return;
		}
		if (!before) {
			beginEdit();
			before->paths = std::move(old);
		}
		touched(*p);
	}
}

EditorXML::Node WriteLayoutPathsXML()
{
	Node root("layout-paths");
	root.set("show", show);
	for (const LayoutGuide &p : paths) {
		Node &n = root.add("path");
		n.set("id", p.id);
		n.set("name", p.name);
		n.set("area", p.area);
		n.set("model", PawnModelId(p.model));
		n.set("curved", p.curved);
		n.set("closed", p.closed);
		n.set("live", p.live);
		n.set("follow", p.follow);
		n.set("ground", p.ground);
		n.set("along-length", p.alongLength);
		n.set("spacing", p.spacing);
		n.set("heading", p.heading);
		n.set("bank", p.bank);
		n.set("count", p.count);
		n.set("from", p.from);
		n.set("to", p.to);
		n.set("side-offset", p.sideOffset);
		n.set("height-offset", p.heightOffset);
		for (const Knot &k : p.knots) {
			Node &c = n.add("knot");
			XMLFields::write(c, "position", vector(k.p));
			XMLFields::write(c, "in", vector(k.in));
			XMLFields::write(c, "out", vector(k.out));
			// "automatic" still, for older Haumea builds.
			c.set("automatic", k.handles == LayoutMath::HANDLES_AUTO);
			c.set("handles", k.handles);
			c.set("tilt", k.tilt);
		}
		for (int id : p.objects)
			if (ObjectInst *o = GetInstanceByID(id))
				if (!o->m_isDeleted && o->m_editorDocumentId)
					n.add("object").set("ref", o->m_editorDocumentId);
	}
	return root;
}
bool ReadLayoutPathsXML(const Node &root, std::string *error, bool apply)
{
	using XMLFields::read;
	std::vector<LayoutGuide> result;
	std::set<int> ids;
	std::set<unsigned> claimed;
	bool shown = true;
	std::map<unsigned, int> documentIds;
	if (apply)
		for (CPtrNode *it = instances.first; it; it = it->next) {
			auto *o = (ObjectInst *)it->item;
			if (!o->m_isDeleted && o->m_editorDocumentId && IsPawnObjectEditable(o) &&
			    !IsPawnWorldObject(o) && !o->m_isVehicle && GetSampElementKind(o) == SAMP_ELEMENT_OBJECT)
				documentIds[o->m_editorDocumentId] = o->m_id;
		}
	if (!read(root, "show", shown, error))
		return false;
	auto bad = [&]() {
		if (error)
			*error = "Invalid layout path data";
		return false;
	};
	for (const Node &n : root.children)
		if (n.name == "path") {
			if (result.size() >= 128)
				return bad();
			LayoutGuide p;
			if (!read(n, "id", p.id, error) || !read(n, "name", p.name, error) ||
			    !read(n, "area", p.area, error) || !read(n, "model", p.model, error) ||
			    !read(n, "curved", p.curved, error) || !read(n, "closed", p.closed, error) ||
			    !read(n, "live", p.live, error) || !read(n, "follow", p.follow, error) ||
			    !read(n, "ground", p.ground, error) || !read(n, "along-length", p.alongLength, error) ||
			    !read(n, "spacing", p.spacing, error) || !read(n, "heading", p.heading, error) ||
			    !read(n, "bank", p.bank, error) || !read(n, "count", p.count, error) ||
			    !read(n, "from", p.from, error) || !read(n, "to", p.to, error) ||
			    !read(n, "side-offset", p.sideOffset, error) || !read(n, "height-offset", p.heightOffset, error))
				return false;
			if (p.id <= 0 || p.id > 1000000 || !ids.insert(p.id).second || p.name.size() > 95 || p.area < 0 ||
			    p.area > 255 || p.spacing < .1f || p.spacing > 1000 || p.count < 0 || p.count > 2000 ||
			    !(p.from >= 0 && p.from <= p.to && p.to <= 1) || !(std::abs(p.sideOffset) <= 2000) ||
			    !(std::abs(p.heightOffset) <= 2000))
				return bad();
			p.model = EditorModelId(p.model);
			size_t references = 0;
			for (const Node &c : n.children) {
				if (c.name == "knot") {
					if (p.knots.size() >= 256)
						return bad();
					Knot k;
					rw::V3d pos = {}, in = {}, out = {};
					bool automatic = true;
					if (!read(c, "position", pos, error) || !read(c, "in", in, error) ||
					    !read(c, "out", out, error) || !read(c, "automatic", automatic, error))
						return false;
					// Files from before handle modes: not automatic meant mirrored.
					k.handles = automatic ? LayoutMath::HANDLES_AUTO : LayoutMath::HANDLES_ALIGNED;
					if (!read(c, "handles", k.handles, error) || !read(c, "tilt", k.tilt, error))
						return false;
					if (k.handles < 0 || k.handles >= LayoutMath::NUM_HANDLES || !(std::abs(k.tilt) <= 360))
						return bad();
					for (rw::V3d v : {pos, in, out})
						if (std::abs(v.x) > 1000000 || std::abs(v.y) > 1000000 || std::abs(v.z) > 1000000)
							return bad();
					k.p = point(pos);
					k.in = point(in);
					k.out = point(out);
					p.knots.push_back(k);
				} else if (c.name == "object") {
					unsigned ref = 0;
					if (!read(c, "ref", ref, error))
						return false;
					if (++references > 2000 || !ref || !claimed.insert(ref).second)
						return bad();
					auto found = documentIds.find(ref);
					if (apply && found != documentIds.end())
						p.objects.push_back(found->second);
				}
			}
			if (p.live && p.knots.size() < 2)
				return bad();
			result.push_back(p);
		}
	if (apply) {
		ResetLayoutPaths();
		paths = std::move(result);
		show = shown;
		active = paths.empty() ? -1 : 0;
		for (const LayoutGuide &p : paths) {
			nextId = std::max(nextId, p.id + 1);
			for (int id : p.objects) {
				owned.insert(id);
				auto *o = GetInstanceByID(id);
				if (o && o->m_lod)
					owned.insert(o->m_lod->m_id);
			}
		}
	}
	return true;
}

// ---- Namaka ----------------------------------------------------------------
// The panel's actions without the panel. The fields are what the bridge got,
// so every value is checked here and an error says which one is wrong.
namespace
{
std::string jsonText(const std::string &text)
{
	std::string out;
	for (unsigned char c : text) {
		if (c == '"' || c == '\\')
			out += '\\', out += (char)c;
		else if (c < 0x20) {
			char hex[8];
			snprintf(hex, sizeof(hex), "\\u%04x", c);
			out += hex;
		} else
			out += (char)c;
	}
	return out;
}
std::string jsonNumber(float v)
{
	char text[32];
	snprintf(text, sizeof(text), "%.4g", std::isfinite(v) ? v : 0.0f);
	return text;
}
std::string pathJson(int index, bool withPoints)
{
	const LayoutGuide &p = paths[index];
	std::string json = "{\"index\":" + std::to_string(index) + ",\"id\":" + std::to_string(p.id) + ",\"name\":\"" +
	                   jsonText(p.name) + "\",\"model\":" + std::to_string(PawnModelId(p.model)) +
	                   ",\"points\":" + std::to_string(p.knots.size()) +
	                   ",\"length\":" + jsonNumber(LayoutMath::curveLength(p.knots, p.curved, p.closed)) +
	                   ",\"curved\":" + (p.curved ? "true" : "false") + ",\"closed\":" + (p.closed ? "true" : "false") +
	                   ",\"live\":" + (p.live ? "true" : "false") + ",\"follow\":" + (p.follow ? "true" : "false") +
	                   ",\"ground\":" + (p.ground ? "true" : "false") +
	                   ",\"along_length\":" + (p.alongLength ? "true" : "false") + ",\"spacing\":" + jsonNumber(p.spacing) +
	                   ",\"count\":" + std::to_string(p.count) + ",\"from\":" + jsonNumber(p.from) +
	                   ",\"to\":" + jsonNumber(p.to) + ",\"heading\":" + jsonNumber(p.heading) +
	                   ",\"bank\":" + jsonNumber(p.bank) + ",\"side_offset\":" + jsonNumber(p.sideOffset) +
	                   ",\"height_offset\":" + jsonNumber(p.heightOffset) + ",\"interior\":" + std::to_string(p.area) +
	                   ",\"samples\":" + std::to_string(samples(p).size()) + ",\"objects\":[";
	for (size_t i = 0; i < p.objects.size(); i++)
		json += (i ? "," : "") + std::to_string(p.objects[i]);
	json += "]";
	if (withPoints) {
		json += ",\"knots\":[";
		for (size_t i = 0; i < p.knots.size(); i++) {
			const Knot &k = p.knots[i];
			json += std::string(i ? "," : "") + "[" + jsonNumber(k.p[0]) + "," + jsonNumber(k.p[1]) + "," +
			        jsonNumber(k.p[2]) + "]";
		}
		json += "]";
	}
	return json + "}";
}
bool parseNumber(const std::string &text, float *out)
{
	char *end = nullptr;
	float v = strtof(text.c_str(), &end);
	if (end == text.c_str() || *end != '\0' || !std::isfinite(v))
		return false;
	*out = v;
	return true;
}
bool parseVector(const std::string &text, rw::V3d *out)
{
	float v[3];
	size_t at = 0;
	for (int i = 0; i < 3; i++) {
		size_t end = text.find(',', at);
		if ((i < 2) != (end != std::string::npos) || !parseNumber(text.substr(at, end - at), &v[i]) ||
		    std::abs(v[i]) > 1000000)
			return false;
		at = end + 1;
	}
	*out = {v[0], v[1], v[2]};
	return true;
}
bool parsePoints(const std::string &text, std::vector<Knot> &out)
{
	out.clear();
	for (size_t at = 0; at <= text.size();) {
		size_t end = text.find(';', at);
		if (end == std::string::npos)
			end = text.size();
		rw::V3d v;
		if (!parseVector(text.substr(at, end - at), &v))
			return false;
		Knot k;
		k.p = point(v);
		out.push_back(k);
		at = end + 1;
	}
	return true;
}
bool parseFlag(const std::string &text, bool *out)
{
	if (text == "1" || text == "true" || text == "yes")
		*out = true;
	else if (text == "0" || text == "false" || text == "no")
		*out = false;
	else
		return false;
	return true;
}
} // namespace

std::shared_ptr<void> LayoutPathsSnapshot()
{
	return snapshot();
}

bool LayoutPathsAgent(const std::string &op, const std::map<std::string, std::string> &f, std::string *json,
                      std::string *error)
{
	auto has = [&](const char *key) { return f.count(key) > 0; };
	auto get = [&](const char *key) { return has(key) ? f.at(key) : std::string(); };
	auto fail = [&](const std::string &message) {
		*error = message;
		return false;
	};
	if (op == "list") {
		*json = "\"paths\":[";
		for (size_t i = 0; i < paths.size(); i++)
			*json += (i ? "," : "") + pathJson((int)i, false);
		*json += "]";
		return true;
	}
	if (op == "new") {
		if (paths.size() >= 128)
			return fail("a map holds at most 128 paths");
		static const char *shapes[] = {"line", "curve", "loop", "helix"};
		int shape = -1;
		std::string name = has("shape") ? get("shape") : "curve";
		for (int i = 0; i < 4; i++)
			if (name == shapes[i])
				shape = i;
		if (shape < 0)
			return fail("shape is line, curve, loop or helix");
		std::string axisName = has("axis") ? get("axis") : "z";
		int axis = axisName == "x" ? 0 : axisName == "y" ? 1 : axisName == "z" ? 2 : -1;
		float radius = 10, turns = 1, rise = 15;
		rw::V3d centre;
		std::vector<Knot> knots;
		if (axis < 0)
			return fail("axis is x, y or z (the loop's or helix's turning axis)");
		if ((has("radius") && !parseNumber(get("radius"), &radius)) ||
		    (has("turns") && !parseNumber(get("turns"), &turns)) || (has("rise") && !parseNumber(get("rise"), &rise)))
			return fail("radius, turns and rise are numbers");
		if (has("centre") && !parseVector(get("centre"), &centre))
			return fail("centre is x,y,z");
		if (has("points") && (!parsePoints(get("points"), knots) || knots.size() < 2 || knots.size() > 256))
			return fail("points are 2-256 x,y,z separated by ;");
		// Fit: a whole number of the model's lengths around one turn, so a loop closes without a gap.
		if (has("fit_model")) {
			float model;
			if (!parseNumber(get("fit_model"), &model) || !GetObjectDef(EditorModelId((int)model)))
				return fail("fit_model is a model id");
			float piece = MapToolsModelLength(EditorModelId((int)model), nullptr);
			if (piece > .05f)
				radius = std::max(3.f, std::round(6.283185307f * radius / piece)) * piece / 6.283185307f;
		}
		radius = clamp(radius, .5f, 1000.f);
		turns = clamp(turns, .25f, 8.f);
		rise = clamp(rise, -1000.f, 1000.f);
		createShape(shape, axis, radius, turns, rise, has("centre") ? &centre : nullptr);
		if (f.size() > (size_t)(has("shape") + has("axis") + has("radius") + has("turns") + has("rise") +
		                        has("centre") + has("fit_model"))) {
			// Its other settings (points, name, model...) go through set: a second undo step.
			std::map<std::string, std::string> rest(f);
			rest["index"] = std::to_string(active);
			for (const char *key : {"shape", "axis", "radius", "turns", "rise", "centre", "fit_model"})
				rest.erase(key);
			return LayoutPathsAgent("set", rest, json, error);
		}
		*json = "\"path\":" + pathJson(active, true);
		return true;
	}
	float indexValue;
	if (!has("index") || !parseNumber(get("index"), &indexValue) || indexValue < 0 ||
	    (int)indexValue >= (int)paths.size())
		return fail("index: no such path (list shows them)");
	int index = (int)indexValue;
	if (op == "get") {
		*json = "\"path\":" + pathJson(index, true);
		return true;
	}
	if (op == "delete") {
		active = index;
		beginEdit();
		paths[index].live = false;
		paths.erase(paths.begin() + index);
		active = paths.empty() ? -1 : std::min(index, (int)paths.size() - 1);
		selected = -1;
		changed = true;
		finishEdit();
		*json = "\"deleted\":" + std::to_string(index) + ",\"paths\":" + std::to_string(paths.size());
		return true;
	}
	std::vector<LayoutGuide> old = paths;
	LayoutGuide &p = paths[index];
	auto point = [&](const char *key, int *out) {
		float v;
		if (!has(key) || !parseNumber(get(key), &v) || v < 0 || (int)v >= (int)p.knots.size())
			return false;
		*out = (int)v;
		return true;
	};
	if (op == "set") {
		bool liveBefore = p.live;
		for (const auto &field : f) {
			const std::string &key = field.first, &value = field.second;
			float v;
			bool b;
			if (key == "index")
				continue;
			if (key == "name") {
				if (value.empty() || value.size() >= 64)
					return paths = old, fail("name: 1-63 characters");
				p.name = value;
			} else if (key == "model") {
				if (!parseNumber(value, &v) || !GetObjectDef(EditorModelId((int)v)))
					return paths = old, fail("model: not a model id");
				if (liveBefore && EditorModelId((int)v) != p.model)
					return paths = old, fail("model: detach the objects first; a live path keeps its model");
				p.model = EditorModelId((int)v);
			} else if (key == "points") {
				std::vector<Knot> knots;
				if (!parsePoints(value, knots) || knots.size() < 2 || knots.size() > 256)
					return paths = old, fail("points are 2-256 x,y,z separated by ;");
				p.knots = knots;
			} else if (key == "curved" || key == "closed" || key == "follow" || key == "ground" ||
			           key == "along_length") {
				if (!parseFlag(value, &b))
					return paths = old, fail(key + ": true or false");
				(key == "curved" ? p.curved : key == "closed" ? p.closed : key == "follow" ? p.follow
				 : key == "ground" ? p.ground : p.alongLength) = b;
			} else if (key == "count") {
				if (!parseNumber(value, &v) || v < 0 || v > 2000 || v == 1)
					return paths = old, fail("count: 0 (every spacing metres) or 2-2000");
				p.count = (int)v;
			} else if (key == "spacing" || key == "from" || key == "to" || key == "heading" || key == "bank" ||
			           key == "side_offset" || key == "height_offset") {
				if (!parseNumber(value, &v) || std::abs(v) > 1000000)
					return paths = old, fail(key + ": a number");
				if (key == "spacing" && v < .05f)
					return paths = old, fail("spacing: at least 0.05 m");
				if ((key == "from" || key == "to") && (v < 0 || v > 1))
					return paths = old, fail(key + ": a fraction of the length, 0-1");
				(key == "spacing" ? p.spacing : key == "from" ? p.from : key == "to" ? p.to : key == "heading"
				 ? p.heading : key == "bank" ? p.bank : key == "side_offset" ? p.sideOffset : p.heightOffset) = v;
			} else
				return paths = old, fail("unknown path setting " + key);
		}
		if (p.from >= p.to)
			return paths = old, fail("from must be below to");
	} else if (op == "add_point" || op == "move_point") {
		rw::V3d v;
		int at = -1;
		if (!has("position") || !parseVector(get("position"), &v))
			return fail("position is x,y,z");
		if (op == "move_point" && !point("point", &at))
			return fail("point: no such point on this path");
		if (op == "add_point") {
			if (p.knots.size() >= 256)
				return fail("a path holds at most 256 points");
			Knot k;
			k.p = ::point(v);
			if (has("after")) {
				if (!point("after", &at))
					return fail("after: no such point on this path");
				p.knots.insert(p.knots.begin() + at + 1, k);
			} else
				p.knots.push_back(k);
		} else
			p.knots[at].p = ::point(v);
	} else if (op == "insert_point") {
		int segment;
		if (!point("point", &segment) || (!p.closed && segment == (int)p.knots.size() - 1))
			return fail("point: the segment's first point (not the last point of an open path)");
		if (p.knots.size() >= 256)
			return fail("a path holds at most 256 points");
		insertMiddle(p, segment);
	} else if (op == "remove_point") {
		int at;
		if (!point("point", &at))
			return fail("point: no such point on this path");
		if (p.knots.size() <= 2)
			return fail("a path keeps at least 2 points");
		p.knots.erase(p.knots.begin() + at);
	} else if (op == "generate") {
		if (samples(p).empty() || !GetObjectDef(p.model))
			return fail("nothing to generate: the path is too short or its model is missing");
		if (!p.live)
			ClearSelection();
		p.live = true;
	} else if (op == "detach") {
		if (!p.live)
			return fail("the path has no objects to detach");
		p.live = false;
		p.objects.clear();
	} else
		return fail("unknown path operation (list, get, new, set, add_point, insert_point, remove_point, "
		            "move_point, generate, detach, delete)");
	active = index;
	selected = std::min(selected, (int)paths[index].knots.size() - 1);
	beginEdit();
	before->paths = std::move(old);
	touched(paths[index]);
	finishEdit();
	*json = "\"path\":" + pathJson(index, true);
	return true;
}
