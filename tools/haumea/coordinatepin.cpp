// SPDX-License-Identifier: GPL-2.0-or-later
#include "coordinatepin.h"
#include "alignmentmath.h"
#include "haumea.h"
#include "editorxml.h"
#include "xmlfields.h"
#include "i18n.h"
#include "maptools.h"
#include "shortcuts.h"
#include <algorithm>
#include <cmath>
#include <climits>
#include <unordered_set>

namespace
{
struct Pin {
	int id, area;
	rw::V3d position, end, normal = {0, 0, 1};
	bool measured = false;
	int endPin = 0;
};
std::vector<Pin> pins;
int active = -1, nextId = 1, picking, dragged = -1, dragHandle;
int hovered = -1, hoverHandle, panelFrame = -100;
bool visible, showPins = true, snapSurface = true, snapPoints = true, snapObjects = true;
bool guides[3] = {true, true, false}, alignAxes[3] = {true, true, false}, faceRuler;
int alignAnchor = 1;
float offset, angleStep = 15;
Pin saved;
rw::V3d dragReference, snapPoint;
int lastSnapModel = -1;
bool anchorSnapped, dragMoved;
ImVec2 dragStart;
std::string documentBaseline;
Pin *current()
{
	return active >= 0 && active < (int)pins.size() ? &pins[active] : nullptr;
}
rw::V3d fromArray(Alignment::Point p)
{
	return {p[0], p[1], p[2]};
}
Alignment::Point array(rw::V3d p)
{
	return {p.x, p.y, p.z};
}
bool sameArea(int a, int b)
{
	return gNoAreaCull || a == b || a == 13 || b == 13;
}
void resolveEndpoints()
{
	for(Pin &pin : pins)
		if(pin.endPin) {
			bool found = false;
			for(const Pin &other : pins)
				if(other.id == pin.endPin) {
					pin.end = other.position;
					found = true;
					break;
				}
			if(!found)
				pin.endPin = 0;
		}
}
bool project(rw::V3d p, ImVec2 *screen)
{
	rw::V3d s;
	float w, h;
	if(!Sprite::CalcScreenCoors(p, &s, &w, &h, false))
		return false;
	*screen = EditorUiPoint(s.x, s.y);
	return true;
}
void cancel()
{
	if(dragged >= 0 && dragged < (int)pins.size())
		pins[dragged] = saved;
	dragged = -1;
	picking = 0;
	anchorSnapped = false;
}
void arm(int kind)
{
	cancel();
	SpawnExitPlaceMode();
	ExitPrefabPlaceMode();
	ExitBrushMode();
	gRectSelectActive = false;
	picking = kind;
	showPins = visible = true;
}
void append(rw::V3d position, rw::V3d normal)
{
	cancel();
	showPins = true;
	Pin pin;
	pin.id = nextId++;
	pin.area = currentArea;
	pin.position = pin.end = position;
	pin.normal = normal;
	pins.push_back(pin);
	active = (int)pins.size() - 1;
}
struct SnapSearch {
	Ray ray;
	float best = 0, depth = 1.0e30f, hitDepth = 1.0e30f;
	rw::V3d point, normal;
	int model = -1;
};
void snapObject(ObjectInst *inst, void *data)
{
	SnapSearch &search = *(SnapSearch *)data;
	if(inst->m_isDeleted || inst->m_editorHidden)
		return;
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(!obj || !obj->m_colModel)
		return;
	const auto &box = obj->m_colModel->boundingBox;
	rw::V3d centre = scale(add(box.min, box.max), .5f), half = scale(sub(box.max, box.min), .5f);
	rw::V3d::transformPoints(&centre, &centre, 1, &inst->m_matrix);
	float radius = length(inst->m_matrix.right) * fabsf(half.x) + length(inst->m_matrix.up) * fabsf(half.y) +
		       length(inst->m_matrix.at) * fabsf(half.z);
	rw::V3d to = sub(centre, search.ray.start);
	float depth = dot(to, search.ray.dir);
	if(depth < -radius || depth - radius > search.hitDepth + depth * .03f)
		return;
	// A small silhouette margin makes corners reachable from either side.
	float margin = std::max(.1f, depth * .03f);
	rw::V3d perpendicular = sub(to, scale(search.ray.dir, depth));
	if(dot(perpendicular, perpendicular) > (radius + margin) * (radius + margin))
		return;
	ImVec2 mouse = ImGui::GetIO().MousePos;
	rw::V3d coordinates[3] = {box.min, scale(add(box.min, box.max), .5f), box.max};
	for(int x = 0; x < 3; x++)
		for(int y = 0; y < 3; y++)
			for(int z = 0; z < 3; z++) {
				rw::V3d p = {coordinates[x].x, coordinates[y].y, coordinates[z].z};
				rw::V3d::transformPoints(&p, &p, 1, &inst->m_matrix);
				ImVec2 s;
				if(!project(p, &s))
					continue;
				float d = (s.x - mouse.x) * (s.x - mouse.x) + (s.y - mouse.y) * (s.y - mouse.y);
				float t = dot(sub(p, search.ray.start), search.ray.dir);
				if(d > search.best || (fabsf(d - search.best) < .01f && t >= search.depth))
					continue;
				search.best = d;
				search.depth = t;
				search.point = p;
				search.model = PawnModelId(inst->m_objectId);
				rw::V3d n = {(float)x - 1, (float)y - 1, (float)z - 1};
				if(dot(n, n) < .1f)
					n = {0, 1, 0};
				rw::V3d::transformVectors(&n, &n, 1, &inst->m_matrix);
				search.normal = normalize(n);
			}
}
bool surface(rw::V3d *point, rw::V3d *outNormal)
{
	Ray ray;
	ray.start = TheCamera.m_position;
	ray.dir = normalize(TheCamera.m_mouseDir);
	bool alt = CPad::IsAltDown(), found = false;
	lastSnapModel = -1;
	anchorSnapped = false;
	rw::V3d normal = {0, 0, 1};
	float hitDepth = 1.0e30f;
	if(!alt && (snapSurface || snapPoints)) {
		rw::V3d hit;
		ObjectInst *inst = GetVisibleInstUnderRay(ray, &hit, &hitDepth);
		if(inst && snapSurface) {
			*point = hit;
			IntersectRayColModelDetailed(ray, inst, point, &normal);
			lastSnapModel = PawnModelId(inst->m_objectId);
			found = true;
		}
	}
	if(!found) {
		Pin *pin = current();
		float z = pin ? pin->position.z : TheCamera.m_target.z;
		if(fabsf(ray.dir.z) > .0001f) {
			float t = (z - ray.start.z) / ray.dir.z;
			if(t > 0 && t <= 5000) {
				*point = add(ray.start, scale(ray.dir, t));
				found = true;
			}
		}
	}
	if(!alt && snapPoints) {
		SnapSearch search;
		search.ray = ray;
		search.hitDepth = hitDepth;
		float pixels = 12 * GetUiScale();
		search.best = pixels * pixels;
		ForEachVisibleInst(snapObject, &search);
		if(search.model != -1) {
			*point = search.point;
			normal = search.normal;
			lastSnapModel = search.model;
			found = anchorSnapped = true;
			snapPoint = *point;
		}
	}
	if(outNormal)
		*outNormal = normal;
	return found;
}
rw::V3d constrain(rw::V3d point, rw::V3d reference)
{
	ImGuiIO &io = ImGui::GetIO();
	rw::V3d result =
	    add(reference,
		fromArray(Alignment::Constrain(array(sub(point, reference)), io.KeyShift, io.KeyCtrl, angleStep)));
	if(length(sub(result, point)) > .001f) {
		anchorSnapped = false;
		lastSnapModel = -1;
	}
	return result;
}
void label(ImDrawList *draw, ImVec2 p, const char *text, ImU32 colour)
{
	ImVec2 size = ImGui::CalcTextSize(text);
	p.x -= size.x * .5f;
	draw->AddRectFilled(ImVec2(p.x - 5, p.y - 3), ImVec2(p.x + size.x + 5, p.y + size.y + 3),
			    IM_COL32(25, 25, 29, 225), 4);
	draw->AddText(p, colour, text);
}
void result(int count, const std::string &error)
{
	if(count < 0)
		Toast(TOAST_SELECTION, "%s", _(error.c_str()));
	else
		Toast(TOAST_SELECTION, _("Aligned %d object(s)"), count);
}
} // namespace

// Namaka's way to the pin (Tools > Coordinate pin): a new pin, with a ruler
// to end when given; it becomes the active one and shows in the 3D view.
void CoordinatePinPlace(const rw::V3d &position, const rw::V3d *rulerEnd)
{
	append(position, {0, 0, 1});
	visible = true;
	if(rulerEnd) {
		pins.back().end = *rulerEnd;
		pins.back().measured = true;
	}
}

// Removes the active pin, or all of them; how many went.
int CoordinatePinClear(bool all)
{
	cancel();
	int removed = 0;
	if(all) {
		removed = (int)pins.size();
		pins.clear();
		active = -1;
	} else if(current()) {
		pins.erase(pins.begin() + active);
		removed = 1;
		active = pins.empty() ? -1 : (int)pins.size() - 1;
	}
	resolveEndpoints();
	return removed;
}

int CoordinatePinCount()
{
	return (int)pins.size();
}
int CoordinatePinSnapModel()
{
	return lastSnapModel;
}
bool CoordinatePinAnchorSnapped()
{
	return anchorSnapped;
}
bool CoordinatePinPosition(rw::V3d *position, rw::V3d *end)
{
	resolveEndpoints();
	Pin *pin = current();
	if(position)
		*position = pin ? pin->position : rw::V3d{0, 0, 0};
	if(end)
		*end = pin ? pin->end : rw::V3d{0, 0, 0};
	return pin && (!end || pin->measured);
}
bool CoordinatePinScreen(ImVec2 *point, int handle, int index)
{
	resolveEndpoints();
	if(index < 0)
		index = active;
	if(index < 0 || index >= (int)pins.size())
		return false;
	Pin &pin = pins[index];
	if(handle == 2 && !pin.measured)
		return false;
	if(!project(handle == 2 ? pin.end : pin.position, point))
		return false;
	if(handle == 0)
		point->y -= 24 * GetUiScale();
	return true;
}
// Panel layout helpers, defined in maptools.cpp.
float MapToolsButtonWidth(int n);
float MapToolsWidthBeforeButton(const char *label);
bool MapToolsMainButton(const char *label, const ImVec2 &size = ImVec2(-FLT_MIN, 0.0f));

namespace
{
// A row's name from a translated "Name:" caption: without the colon (ASCII
// or full-width) and the spaces before it.
std::string rowName(const char *caption)
{
	std::string name = caption;
	if(name.size() >= 3 && name.compare(name.size() - 3, 3, "\xEF\xBC\x9A") == 0)
		name.resize(name.size() - 3);
	else if(!name.empty() && name.back() == ':')
		name.pop_back();
	while(!name.empty() && name.back() == ' ')
		name.pop_back();
	return name;
}
// A row of X, Y and Z checkboxes after the row's name in the left column.
void axisCheckboxes(const char *caption, const char *const labels[3], bool values[3])
{
	char id[128];
	std::string name = rowName(caption);
	UiFieldLabel(name.c_str(), id, sizeof(id));
	for(int i = 0; i < 3; i++) {
		if(i > 0)
			ImGui::SameLine();
		UiCheckbox(labels[i], &values[i]);
	}
}
} // namespace

void CoordinatePinPanel()
{
	panelFrame = ImGui::GetFrameCount();
	resolveEndpoints();
	float half = MapToolsButtonWidth(2);
	if(MapToolsMainButton(_("Place pin"), ImVec2(half, 0)))
		arm(1);
	ImGui::SetItemTooltip(_("Add a pin on a surface. Drag its head to move it; drag its base ring to measure. Both "
				"ends snap to objects."));
	ImGui::SameLine();
	ImGui::BeginDisabled(!current());
	if(ImGui::Button(_("Clear pin"), ImVec2(half, 0))) {
		cancel();
		pins.erase(pins.begin() + active);
		active = pins.empty() ? -1 : std::min(active, (int)pins.size() - 1);
		resolveEndpoints();
	}
	ImGui::EndDisabled();
	ImGui::BeginDisabled(!selection.first);
	if(ImGui::Button(_("Pin at selected centre"), ImVec2(-FLT_MIN, 0)) && selection.first) {
		ObjectInst *inst = (ObjectInst *)selection.first->item;
		ObjectDef *obj = GetObjectDef(inst->m_objectId);
		rw::V3d centre = {0, 0, 0};
		if(obj && obj->m_colModel)
			centre = scale(add(obj->m_colModel->boundingBox.min, obj->m_colModel->boundingBox.max), .5f);
		rw::V3d::transformPoints(&centre, &centre, 1, &inst->m_matrix);
		append(centre, normalize(inst->m_matrix.up));
	}
	ImGui::SetItemTooltip(_("Add a pin at the object's collision-box centre. For an embedded doorway, pick its two "
				"edges and use Midpoint."));
	ImGui::EndDisabled();
	if(picking) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("%s", picking == 1 ? _("Click to place the pin. Esc cancels.")
						      : _("Click the measurement endpoint. Esc cancels."));
		ImGui::PopStyleColor();
	}

	// Showing and snapping: the on/off options together, then the step.
	ImGui::Spacing();
	UiCheckbox(_("Show pins and guides"), &showPins);
	UiCheckbox(_("Snap to surfaces"), &snapSurface);
	UiCheckbox(_("Snap to object points"), &snapPoints);
	ImGui::SetItemTooltip(_("Snap pins and endpoints to collision-box corners, edge midpoints, face centres and "
				"object centres. Alt temporarily releases snapping."));
	if(UiInputFloat(_("Angle step"), &angleStep, 0, 0, "%.0f"))
		angleStep = clamp(angleStep, 1.0f, 180.0f);
	ImGui::SetItemTooltip(_("Shift: straight X/Y/Z. Ctrl: angle steps. Shift+Ctrl: angle steps on a horizontal "
				"plane. Alt: free placement."));

	Pin *pin = current();
	if(!pin)
		return;
	ImGui::Spacing();
	if(pins.size() > 1) {
		char title[64];
		snprintf(title, sizeof(title), _("Pin %d"), pins[active].id);
		ImGui::SetNextItemWidth(-FLT_MIN);
		if(ImGui::BeginCombo("##active_pin", title)) {
			for(int i = 0; i < (int)pins.size(); i++) {
				snprintf(title, sizeof(title), _("Pin %d"), pins[i].id);
				if(ImGui::Selectable(title, active == i)) {
					cancel();
					active = i;
				}
			}
			ImGui::EndCombo();
		}
		pin = current();
	}
	UiInputFloat3(_("Pin XYZ"), &pin->position.x, "%.3f");
	ImGui::SetNextItemWidth(MapToolsWidthBeforeButton(_("Apply offset")));
	UiInputFloat(_("Surface offset"), &offset, 0, 0, "%.3f");
	ImGui::SetItemTooltip(_("Move the pin this many units along the last picked surface normal. Negative values "
				"move to the other side."));
	ImGui::SameLine();
	if(ImGui::Button(_("Apply offset")))
		pin->position = add(pin->position, scale(pin->normal, offset));
	if(pins.size() > 1) {
		char title[64];
		snprintf(title, sizeof(title), "%s", _("Free endpoint"));
		if(pin->endPin)
			snprintf(title, sizeof(title), _("Pin %d"), pin->endPin);
		if(UiBeginCombo(_("Ruler to"), title)) {
			if(ImGui::Selectable(_("Free endpoint"), !pin->endPin))
				pin->endPin = 0;
			for(const Pin &other : pins)
				if(other.id != pin->id) {
					snprintf(title, sizeof(title), _("Pin %d"), other.id);
					if(ImGui::Selectable(title, pin->endPin == other.id)) {
						pin->endPin = other.id;
						pin->end = other.position;
						pin->measured = true;
					}
				}
			ImGui::EndCombo();
		}
	}
	half = MapToolsButtonWidth(2);
	if(ImGui::Button(_("Copy coordinates"), ImVec2(half, 0))) {
		char text[160];
		snprintf(text, sizeof(text), "%.6f, %.6f, %.6f", pin->position.x, pin->position.y, pin->position.z);
		ImGui::SetClipboardText(text);
	}
	ImGui::SameLine();
	if(ImGui::Button(_("Pick endpoint"), ImVec2(half, 0)))
		arm(2);
	if(pin->measured) {
		ImGui::Spacing();
		if(UiInputFloat3(_("Endpoint XYZ"), &pin->end.x, "%.3f"))
			pin->endPin = 0;
		rw::V3d d = sub(pin->end, pin->position);
		ImGui::TextDisabled("dX %.3f   dY %.3f   dZ %.3f", d.x, d.y, d.z);
		// Midpoint, then X, Y and Z to line the pin up with the endpoint.
		char id[128];
		std::string name = rowName(_("Align pin:"));
		UiFieldLabel(name.c_str(), id, sizeof(id));
		float quarter = MapToolsButtonWidth(4);
		if(ImGui::Button(_("Midpoint"), ImVec2(quarter, 0))) {
			pin->position = scale(add(pin->position, pin->end), .5f);
			pin->measured = false;
			pin->endPin = 0;
		}
		ImGui::SetItemTooltip(
		    _("Place the pin halfway between the two picked points, for example the edges of a doorway."));
		for(int axis = 0; axis < 3; axis++) {
			ImGui::SameLine();
			const char *labels[] = {"X##pin_align", "Y##pin_align", "Z##pin_align"};
			if(ImGui::Button(labels[axis], ImVec2(quarter, 0)))
				(&pin->position.x)[axis] = (&pin->end.x)[axis];
		}
		if(ImGui::TreeNode(_("Ruler settings"))) {
			auto angles = Alignment::Angles(array(sub(pin->end, pin->position)));
			bool changed = false;
			changed |= UiInputFloat(_("Distance"), &angles[0], 0, 0, "%.3f");
			changed |= UiInputFloat(_("Heading"), &angles[1], 0, 0, "%.2f");
			changed |= UiInputFloat(_("Pitch"), &angles[2], 0, 0, "%.2f");
			if(changed) {
				pin->end =
				    add(pin->position, fromArray(Alignment::FromAngles(
							   angles[0], angles[1], clamp(angles[2], -90.0f, 90.0f))));
				pin->endPin = 0;
			}
			ImGui::TreePop();
		}
	}
	if(!ImGui::TreeNode(_("Alignment guides")))
		return;
	const char *guideLabels[] = {"X##guide", "Y##guide", "Z##guide"};
	axisCheckboxes(_("Show guides:"), guideLabels, guides);
	const char *axisLabels[] = {"X##align_axis", "Y##align_axis", "Z##align_axis"};
	axisCheckboxes(_("Align axes:"), axisLabels, alignAxes);
	const char *anchors[] = {_("Minimum"), _("Centre"), _("Maximum"), _("Origin")};
	UiCombo(_("Object anchor"), &alignAnchor, anchors, 4);
	UiCheckbox(_("Snap objects to pins"), &snapObjects);
	ImGui::SetItemTooltip(_(
	    "With object snap enabled (N), moving objects snap their edges or centre to the pins and visible guides."));
	if(pin->measured)
		UiCheckbox(_("Face ruler direction"), &faceRuler);
	int axes = (alignAxes[0] ? 1 : 0) | (alignAxes[1] ? 2 : 0) | (alignAxes[2] ? 4 : 0);
	ImGui::BeginDisabled(!selection.first || !axes);
	std::string error;
	if(ImGui::Button(_("Align selection to pin"), ImVec2(-FLT_MIN, 0)))
		result(MapToolsAlignToGuide(pin->position, nullptr, axes, alignAnchor, false, false, &error), error);
	if(pin->measured) {
		if(ImGui::Button(_("Align selection to ruler"), ImVec2(-FLT_MIN, 0)))
			result(
			    MapToolsAlignToGuide(pin->position, &pin->end, axes, alignAnchor, false, faceRuler, &error),
			    error);
		ImGui::SetItemTooltip(_("Project each selected object's anchor onto the extended ruler, preserving its "
					"position along the guide."));
		if(ImGui::Button(_("Distribute between ends"), ImVec2(-FLT_MIN, 0)))
			result(
			    MapToolsAlignToGuide(pin->position, &pin->end, axes, alignAnchor, true, faceRuler, &error),
			    error);
		ImGui::SetItemTooltip(_("Space the selected objects evenly between the two endpoints. Two objects go "
					"to opposite ends. One undo restores the selection."));
	}
	ImGui::EndDisabled();
	ImGui::TreePop();
}
bool CoordinatePinOwnsMouse()
{
	return visible && (picking || dragged >= 0 || hovered >= 0);
}
bool CoordinatePinHandleViewport()
{
	if(!visible || (!picking && dragged < 0 && hovered < 0))
		return false;
	ImGuiIO &io = ImGui::GetIO();
	if(ShortcutPressed(SC_CANCEL) || CPad::IsMButtonClicked(3)) {
		cancel();
		return true;
	}
	if(dragged < 0 && io.WantCaptureMouse)
		return false;
	if(picking && CPad::IsMButtonClicked(1)) {
		rw::V3d p, n;
		if(surface(&p, &n)) {
			if(picking == 1) {
				bool snapped = anchorSnapped;
				append(p, n);
				anchorSnapped = snapped;
			} else if(Pin *pin = current()) {
				pin->end = constrain(p, pin->position);
				pin->measured = true;
				pin->endPin = 0;
			}
			picking = 0;
		}
		return true;
	}
	if(!picking && hovered >= 0 && CPad::IsMButtonJustDown(1)) {
		active = dragged = hovered;
		dragHandle = hoverHandle;
		saved = pins[dragged];
		dragReference = dragHandle == 0 ? (saved.measured ? saved.end : saved.position) : saved.position;
		dragStart = io.MousePos;
		dragMoved = false;
	}
	if(dragged >= 0) {
		float dx = io.MousePos.x - dragStart.x, dy = io.MousePos.y - dragStart.y;
		if(dx * dx + dy * dy > 16 * GetUiScale() * GetUiScale()) dragMoved = true;
		rw::V3d p, n;
		if(dragMoved && !io.WantCaptureMouse && surface(&p, &n)) {
			Pin &pin = pins[dragged];
			p = constrain(p, dragReference);
			if(dragHandle == 0) {
				pin.position = p;
				pin.normal = n;
			} else {
				pin.end = p;
				pin.measured = true;
				pin.endPin = 0;
			}
		}
		if(!CPad::IsMButtonDown(1))
			dragged = -1;
		return true;
	}
	return picking || hovered >= 0;
}
void CoordinatePinSnapTargets(int area, const rw::V3d &centre, std::vector<rw::V3d> &targets)
{
	if(!visible || !snapObjects)
		return;
	resolveEndpoints();
	for(const Pin &pin : pins)
		if(sameArea(pin.area, area)) {
			targets.push_back(pin.position);
			for(int i = 0; i < 3; i++)
				if(guides[i]) {
					rw::V3d p = centre;
					(&p.x)[i] = (&pin.position.x)[i];
					targets.push_back(p);
				}
			if(pin.measured) {
				targets.push_back(pin.end);
				float t = Alignment::Projection(array(centre), array(pin.position), array(pin.end));
				targets.push_back(add(pin.position, scale(sub(pin.end, pin.position), t)));
			}
		}
}
void CoordinatePinOverlay(bool enabled)
{
	// Pins are document content now. Finish a drag/typed edit as one dirty
	// change, independently of which panel is visible.
	EditorXML::Node state = WriteCoordinatePinXML();
	state.attributes.erase("active");
	std::string signature = EditorXML::Serialize(state);
	if(documentBaseline.empty()) documentBaseline = signature;
	else if(signature != documentBaseline && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(0)){
		BumpChangeSeq(); documentBaseline = signature;
	}
	visible = enabled && showPins;
	hovered = -1;
	if(!visible) {
		cancel();
		return;
	}
	if(picking && ImGui::GetFrameCount() - panelFrame > 1) cancel();
	resolveEndpoints();
#ifdef IMGUI_HAS_VIEWPORT
	ImDrawList *draw = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
#else
	ImDrawList *draw = ImGui::GetBackgroundDrawList();
#endif
	const ImU32 yellow = IM_COL32(255, 218, 60, 255), dim = IM_COL32(235, 204, 80, 200);
	float ui = GetUiScale(), best = 12 * ui * 12 * ui;
	ImVec2 mouse = ImGui::GetIO().MousePos;
	for(int i = 0; i < (int)pins.size(); i++) {
		Pin &pin = pins[i];
		if(!sameArea(pin.area, currentArea))
			continue;
		ImU32 colour = active == i ? yellow : dim;
		ImVec2 foot, head, end;
		auto hover = [&](ImVec2 p, int handle) {
			if(gPlaceMode || gPrefabPlaceMode || gBrushMode) return;
			float distance = (mouse.x - p.x) * (mouse.x - p.x) + (mouse.y - p.y) * (mouse.y - p.y);
			if(distance < best || (fabsf(distance - best) < .01f && i == active)) {
				best = distance;
				hovered = i;
				hoverHandle = handle;
			}
		};
		if(project(pin.position, &foot)) {
			head = ImVec2(foot.x, foot.y - 24 * ui);
			draw->AddLine(foot, head, colour, 2 * ui);
			draw->AddCircleFilled(head, 8 * ui, colour);
			draw->AddCircle(foot, 5 * ui, colour, 0, 2 * ui);
			char text[20];
			snprintf(text, sizeof(text), "%d", pin.id);
			draw->AddText(ImVec2(head.x + 10 * ui, head.y - ImGui::GetFontSize() * .5f), colour, text);
			hover(head, 0);
			hover(foot, 1);
		}
		float span = clamp(length(sub(TheCamera.m_position, pin.position)) * 2, 50.0f, 2000.0f);
		for(int axis = 0; axis < 3; axis++)
			if(guides[axis]) {
				rw::V3d direction = axis == 0	? rw::V3d{0, span, 0}
						    : axis == 1 ? rw::V3d{span, 0, 0}
								: rw::V3d{0, 0, span};
				RenderLine(sub(pin.position, direction), add(pin.position, direction),
					   {200, 190, 90, 90}, {200, 190, 90, 90});
			}
		if(pin.measured) {
			rw::V3d delta = sub(pin.end, pin.position);
			RenderLine(pin.position, pin.end, {255, 218, 60, 255}, {255, 218, 60, 255});
			if(length(delta) > .001f) {
				rw::V3d extension = scale(normalize(delta), span);
				RenderLine(sub(pin.position, extension), add(pin.end, extension), {255, 218, 60, 80},
					   {255, 218, 60, 80});
			}
			if(project(pin.end, &end)) {
				draw->AddCircleFilled(end, 5 * ui, IM_COL32(30, 30, 30, 255));
				draw->AddCircle(end, 6 * ui, colour, 0, 2 * ui);
				hover(end, 2);
			}
			ImVec2 middle;
			if(active == i && project(scale(add(pin.position, pin.end), .5f), &middle)) {
				char text[180];
				snprintf(text, sizeof(text), _("%.3f units | dX %.3f  dY %.3f  dZ %.3f"), length(delta),
					 delta.x, delta.y, delta.z);
				label(draw, ImVec2(middle.x, middle.y - 24 * ui), text, colour);
			}
		}
	}
	if(hovered >= 0 && !ImGui::GetIO().WantCaptureMouse) {
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		if(dragged < 0 && !picking)
			ImGui::SetTooltip("%s", hoverHandle == 0   ? _("Drag to move pin")
						: hoverHandle == 1 ? _("Drag to measure")
								   : _("Drag to move endpoint"));
	}
	if(anchorSnapped && (picking || dragged >= 0)) {
		ImVec2 point;
		if(project(snapPoint, &point)) {
			float r = 7 * ui;
			draw->AddQuad(ImVec2(point.x - r, point.y), ImVec2(point.x, point.y - r),
				      ImVec2(point.x + r, point.y), ImVec2(point.x, point.y + r),
				      IM_COL32(90, 225, 255, 255), 2 * ui);
		}
	}
}

EditorXML::Node WriteCoordinatePinXML()
{
	using XMLFields::write;
	EditorXML::Node node("alignment");
	node.set("active", active);
	node.set("next-id", nextId);
	node.set("show", showPins);
	node.set("snap-surfaces", snapSurface);
	node.set("snap-points", snapPoints);
	node.set("snap-objects", snapObjects);
	write(node, "guides", guides);
	write(node, "align-axes", alignAxes);
	node.set("face-ruler", faceRuler);
	node.set("anchor", alignAnchor);
	node.set("offset", offset);
	node.set("angle-step", angleStep);
	for (const Pin &pin : pins) {
		EditorXML::Node &p = node.add("pin");
		p.set("id", pin.id);
		p.set("area", pin.area);
		write(p, "position", pin.position);
		write(p, "end", pin.end);
		write(p, "normal", pin.normal);
		p.set("measured", pin.measured);
		p.set("end-pin", pin.endPin);
	}
	return node;
}
bool ReadCoordinatePinXML(const EditorXML::Node &node, std::string *error, bool apply)
{
	using XMLFields::read;
	std::vector<Pin> loaded;
	int activeValue = -1, next = 1, anchor = 1;
	float distance = 0, step = 15;
	bool show = true, surfaces = true, points = true, objects = true, face = false, guide[3] = {true, true, false},
	     axes[3] = {true, true, false};
	if (!read(node, "active", activeValue, error) || !read(node, "next-id", next, error) ||
	    !read(node, "show", show, error) || !read(node, "snap-surfaces", surfaces, error) ||
	    !read(node, "snap-points", points, error) || !read(node, "snap-objects", objects, error) ||
	    !read(node, "guides", guide, error) || !read(node, "align-axes", axes, error) ||
	    !read(node, "face-ruler", face, error) || !read(node, "anchor", anchor, error) ||
	    !read(node, "offset", distance, error) || !read(node, "angle-step", step, error))
		return false;
	std::unordered_set<int> ids;
	for (const EditorXML::Node &p : node.children)
		if (p.name == "pin") {
			Pin pin = {};
			pin.normal = {0, 0, 1};
			if (!read(p, "id", pin.id, error) || !read(p, "area", pin.area, error) ||
			    !read(p, "position", pin.position, error) || !read(p, "end", pin.end, error) ||
			    !read(p, "normal", pin.normal, error) || !read(p, "measured", pin.measured, error) ||
			    !read(p, "end-pin", pin.endPin, error))
				return false;
			if (pin.id <= 0 || pin.id >= INT_MAX || !ids.insert(pin.id).second) {
				*error = "Invalid alignment pin identifier";
				return false;
			}
			next = std::max(next, pin.id + 1);
			loaded.push_back(pin);
		}
	if (!apply)
		return true;
	cancel();
	pins = std::move(loaded);
	active = pins.empty() ? -1 : clamp(activeValue, 0, (int)pins.size() - 1);
	nextId = std::max(1, next);
	showPins = show;
	snapSurface = surfaces;
	snapPoints = points;
	snapObjects = objects;
	faceRuler = face;
	std::copy(guide, guide + 3, guides);
	std::copy(axes, axes + 3, alignAxes);
	alignAnchor = clamp(anchor, 0, 3);
	offset = distance;
	angleStep = clamp(step, 1.0f, 180.0f);
	resolveEndpoints();
	EditorXML::Node state = WriteCoordinatePinXML();
	state.attributes.erase("active");
	documentBaseline = EditorXML::Serialize(state);
	return true;
}
void ResetCoordinatePins()
{
	cancel();
	pins.clear();
	active = -1;
	nextId = 1;
	documentBaseline.clear();
}
