// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "coordinatepin.h"
#include "layoutpaths.h"
#include "mapcleanup.h"
#include "alignmentmath.h"
#include "i18n.h"
#include "maptools.h"
#include "shortcuts.h"

#include <math.h>
#include <random>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

// Transform tools for the selection. Everything goes through the editor's
// transform targets (CaptureObjectTransformTargets and friends), so linked
// LODs follow their objects, and each action is one undo group.

enum { MAX_TOOL_OBJECTS = 2000 };

static float
comp(const rw::V3d &v, int axis)
{
	return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}

static void
setComp(rw::V3d &v, int axis, float value)
{
	(axis == 0 ? v.x : axis == 1 ? v.y : v.z) = value;
}

// The selection in the order it was picked (the list keeps the newest
// first), without LODs whose building is selected too.
static std::vector<ObjectInst*>
selectedObjects(void)
{
	std::vector<ObjectInst*> objs;
	std::unordered_set<ObjectInst*> lods;
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted)
			continue;
		objs.push_back(inst);
		if(inst->m_lod)
			lods.insert(inst->m_lod);
	}
	std::reverse(objs.begin(), objs.end());
	objs.erase(std::remove_if(objs.begin(), objs.end(),
		[&](ObjectInst *inst){ return lods.count(inst) != 0; }), objs.end());
	return objs;
}

// World axes of a stored (conjugated) rotation: right, up, at = local x, y, z.
static rw::Matrix
basisOf(const rw::Quat &stored)
{
	rw::Matrix m;
	m.rotate(rw::conj(stored), rw::COMBINEREPLACE);
	return m;
}

static rw::V3d
basisApply(const rw::Matrix &m, const rw::V3d &v)
{
	return add(add(scale(m.right, v.x), scale(m.up, v.y)), scale(m.at, v.z));
}

static void
modelBox(int model, rw::V3d *mn, rw::V3d *mx)
{
	ObjectDef *obj = GetObjectDef(model);
	if(obj && obj->m_colModel){
		*mn = obj->m_colModel->boundingBox.min;
		*mx = obj->m_colModel->boundingBox.max;
	}else{
		*mn = { -0.5f, -0.5f, -0.5f };
		*mx = { 0.5f, 0.5f, 0.5f };
	}
}

// The world-space box around an object's collision box at pos/rot.
static void
worldBox(int model, const rw::V3d &pos, const rw::Quat &rot, rw::V3d *outMin, rw::V3d *outMax)
{
	rw::V3d mn, mx;
	modelBox(model, &mn, &mx);
	rw::Matrix m = basisOf(rot);
	for(int i = 0; i < 8; i++){
		rw::V3d corner = { i & 1 ? mx.x : mn.x, i & 2 ? mx.y : mn.y, i & 4 ? mx.z : mn.z };
		rw::V3d w = add(pos, basisApply(m, corner));
		if(i == 0)
			*outMin = *outMax = w;
		*outMin = { std::min(outMin->x, w.x), std::min(outMin->y, w.y), std::min(outMin->z, w.z) };
		*outMax = { std::max(outMax->x, w.x), std::max(outMax->y, w.y), std::max(outMax->z, w.z) };
	}
}

static void
instBox(ObjectInst *inst, rw::V3d *mn, rw::V3d *mx)
{
	worldBox(inst->m_objectId, inst->m_translation, inst->m_rotation, mn, mx);
}

// The centre of the box around every object's box.
static rw::V3d
groupCentre(const std::vector<ObjectInst*> &objs)
{
	rw::V3d gmin = { 0, 0, 0 }, gmax = { 0, 0, 0 };
	for(size_t i = 0; i < objs.size(); i++){
		rw::V3d mn, mx;
		instBox(objs[i], &mn, &mx);
		if(i == 0){
			gmin = mn;
			gmax = mx;
		}
		gmin = { std::min(gmin.x, mn.x), std::min(gmin.y, mn.y), std::min(gmin.z, mn.z) };
		gmax = { std::max(gmax.x, mx.x), std::max(gmax.y, mx.y), std::max(gmax.z, mx.z) };
	}
	return scale(add(gmin, gmax), 0.5f);
}

rw::V3d MapToolsSelectionCentre() { return groupCentre(selectedObjects()); }

// A world rotation of degrees around X, then Y, then Z.
static rw::Quat
worldRotation(const rw::V3d &degrees)
{
	const float d2r = 3.14159265f / 180.0f;
	rw::Quat qx = rw::Quat::rotation(degrees.x * d2r, { 1.0f, 0.0f, 0.0f });
	rw::Quat qy = rw::Quat::rotation(degrees.y * d2r, { 0.0f, 1.0f, 0.0f });
	rw::Quat qz = rw::Quat::rotation(degrees.z * d2r, { 0.0f, 0.0f, 1.0f });
	return rw::normalize(rw::mult(qz, rw::mult(qy, qx)));
}

// An object turned by the world rotation w around pivot. Stored rotations
// are conjugated, so the stored delta is conj(w).
static void
turnAround(const rw::V3d &pivot, const rw::Quat &w, rw::V3d *pos, rw::Quat *rot)
{
	*pos = add(pivot, rw::rotate(sub(*pos, pivot), w));
	*rot = rw::normalize(rw::mult(*rot, rw::conj(w)));
}

// Puts one object (and its LOD) at pos/rot; recorded in the open undo group.
static bool
placeObject(ObjectInst *inst, const rw::V3d &pos, const rw::Quat &rot)
{
	std::vector<UndoTransform> transforms;
	if(!CaptureObjectTransformTargets(inst, false, transforms))
		return false;
	PreviewObjectTransformTargets(inst, transforms, pos, rot, UNDO_TRANSFORM_POS | UNDO_TRANSFORM_ROT);
	return CommitObjectTransformTargets(transforms) > 0;
}

static bool
needSelection(const std::vector<ObjectInst*> &objs, int minimum, std::string *error)
{
	if((int)objs.size() >= minimum)
		return true;
	if(error)
		*error = minimum <= 1 ? N_("select something first") :
			"select at least " + std::to_string(minimum) + " objects";
	return false;
}

int MapToolsCenterParent(std::string *error)
{
	auto selected = selectedObjects();
	if(!needSelection(selected, 1, error)) return -1;
	ObjectInst *parent = selected[0];
	for(int depth=0; depth<256 && GetAttachParent(parent); depth++) parent=GetAttachParent(parent);
	std::vector<ObjectInst*> family = { parent }, direct;
	for(CPtrNode *it=instances.first; it; it=it->next){
		auto *o=(ObjectInst*)it->item;
		if(o->m_isDeleted || o==parent) continue;
		ObjectInst *ancestor=GetAttachParent(o);
		if(ancestor==parent) direct.push_back(o);
		for(int depth=0; ancestor && depth<256; depth++,ancestor=GetAttachParent(ancestor))
			if(ancestor==parent){ family.push_back(o); break; }
	}
	if(direct.empty() || !IsPawnObjectEditable(parent)){
		if(error) *error=N_("Select a parent with attached objects, or one of its children.");
		return -1;
	}
	for(auto *o:family) if(!IsPawnObjectEditable(o)){
		if(error) *error=N_("The attachment contains read-only objects.");
		return -1;
	}
	rw::V3d centre=groupCentre(family);
	UndoBeginGroup();
	placeObject(parent,centre,parent->m_rotation);
	// Children have not moved: recompute the direct children's offsets before
	// UpdateObjectAttachments runs, preserving the whole descendant tree in world space.
	for(auto *child:direct) RebaseObjectAttachment(child);
	UndoEndGroup();
	return (int)family.size();
}

static void
reselect(const std::vector<ObjectInst*> &objs)
{
	ClearSelection();
	for(size_t i = 0; i < objs.size(); i++)
		if(!objs[i]->m_isDeleted)
			objs[i]->Select();
}

// ---- array ----------------------------------------------------------------

// Copies made, not counting the selection: count, or a grid of rows lines
// of count + 1 less the selection itself.
static int
arrayCopies(const MapArrayParams &params)
{
	return params.radial || params.rows <= 1 ? params.count : (params.count + 1) * params.rows - 1;
}

static bool
arrayCopyTransform(const MapArrayParams &params, const rw::V3d &centre, int k,
	rw::V3d *pos, rw::Quat *rot)
{
	if(params.radial){
		float step = fabsf(params.angle) >= 359.99f ? params.angle / (params.count + 1) :
			params.angle / params.count;
		turnAround(params.centre, worldRotation({ 0.0f, 0.0f, step * k }), pos, rot);
	}else{
		int rows = std::max(1, params.rows);
		int column = rows > 1 ? k % (params.count + 1) : k, row = rows > 1 ? k / (params.count + 1) : 0;
		turnAround(centre, worldRotation(scale(params.rotate, (float)column)), pos, rot);
		*pos = add(*pos, add(scale(params.offset, (float)column), scale(params.rowOffset, (float)row)));
	}
	return true;
}

static bool
arrayCheck(const MapArrayParams &params, const std::vector<ObjectInst*> &objs, std::string *error)
{
	if(!needSelection(objs, 1, error))
		return false;
	if(params.count < 1){
		if(error) *error = N_("make at least one copy");
		return false;
	}
	if(!params.radial && (params.rows < 1 || params.rows > 500)){
		if(error) *error = "rows is 1 to 500";
		return false;
	}
	if((long long)arrayCopies(params) * objs.size() > MAX_TOOL_OBJECTS){
		if(error) *error = "that would make more than " + std::to_string((int)MAX_TOOL_OBJECTS) + " objects";
		return false;
	}
	return true;
}

bool
MapToolsArrayPreview(const MapArrayParams &params, std::vector<rw::V3d> &positions,
	std::vector<rw::Quat> &rotations, std::string *error)
{
	positions.clear();
	rotations.clear();
	std::vector<ObjectInst*> objs = selectedObjects();
	if(!arrayCheck(params, objs, error))
		return false;
	rw::V3d centre = groupCentre(objs);
	for(int k = 1; k <= arrayCopies(params); k++)
		for(size_t i = 0; i < objs.size(); i++){
			rw::V3d pos = objs[i]->m_translation;
			rw::Quat rot = objs[i]->m_rotation;
			arrayCopyTransform(params, centre, k, &pos, &rot);
			positions.push_back(pos);
			rotations.push_back(rot);
		}
	return true;
}

int
MapToolsArray(const MapArrayParams &params, std::string *error)
{
	std::vector<ObjectInst*> originals = selectedObjects();
	if(!arrayCheck(params, originals, error))
		return -1;
	rw::V3d centre = groupCentre(originals);
	std::vector<ObjectInst*> all = originals;
	int made = 0;
	UndoBeginGroup();
	for(int k = 1; k <= arrayCopies(params); k++){
		// A copy of the originals in place, then moved to copy k.
		reselect(originals);
		if(DuplicateSelected() <= 0)
			break;
		std::vector<ObjectInst*> copies = selectedObjects();
		for(size_t i = 0; i < copies.size(); i++){
			rw::V3d pos = copies[i]->m_translation;
			rw::Quat rot = copies[i]->m_rotation;
			arrayCopyTransform(params, centre, k, &pos, &rot);
			placeObject(copies[i], pos, rot);
		}
		all.insert(all.end(), copies.begin(), copies.end());
		made += (int)copies.size();
	}
	UndoEndGroup();
	reselect(all);
	if(made == 0 && error)
		*error = N_("could not copy the selection");
	return made > 0 ? made : -1;
}

// ---- duplicate offset -----------------------------------------------------

static MapCloneOffset gCloneOffset;

MapCloneOffset &
MapToolsCloneOffset(void)
{
	return gCloneOffset;
}

int
MapToolsDuplicate(std::string *error)
{
	UndoBeginGroup();
	int made = DuplicateSelected();
	std::vector<ObjectInst*> copies = selectedObjects();
	const MapCloneOffset &o = gCloneOffset;
	if(made > 0 && o.enabled && !copies.empty()){
		// One sample for the whole duplicate, so a group stays together.
		static std::mt19937 random(std::random_device{}());
		float v[6];
		for(int i = 0; i < 6; i++){
			float lo = std::min(o.low[i], o.high[i]), hi = std::max(o.low[i], o.high[i]);
			v[i] = !o.random ? o.fixed[i] : lo == hi ? lo : std::uniform_real_distribution<float>(lo, hi)(random);
			if(!std::isfinite(v[i]))
				v[i] = 0.0f;
		}
		rw::V3d move = { v[0], v[1], v[2] };
		rw::Quat turn = worldRotation({ v[3], v[4], v[5] });
		if(o.local){
			// The first object's axes: its world rotation is its stored one conjugated.
			rw::Quat axes = rw::conj(copies[0]->m_rotation);
			move = rw::rotate(move, axes);
			turn = rw::normalize(rw::mult(axes, rw::mult(turn, rw::conj(axes))));
		}
		rw::V3d centre = groupCentre(copies);
		for(ObjectInst *inst : copies){
			rw::V3d pos = inst->m_translation;
			rw::Quat rot = inst->m_rotation;
			turnAround(centre, turn, &pos, &rot);
			placeObject(inst, add(pos, move), rot);
		}
		reselect(copies);
	}
	UndoEndGroup();
	if(made <= 0 && error)
		*error = N_("could not copy the selection");
	return made > 0 ? made : -1;
}

static void
cloneOffsetSection(void)
{
	MapCloneOffset &o = gCloneOffset;
	ImGui::Checkbox(_("Offset each duplicate"), &o.enabled);
	ImGui::SetItemTooltip(_("Duplicate (Ctrl+D) moves and turns the copies by this much."));
	if(!o.enabled)
		return;
	int mode = o.random ? 1 : 0;
	static const char *modes[] = { N_("Fixed"), N_("Random") };
	if(UiCombo(_("Mode##cloneoffset"), &mode, modes, IM_ARRAYSIZE(modes)))
		o.random = mode == 1;
	ImGui::SetItemTooltip(_("Random: a new amount within the ranges each time."));
	ImGui::Checkbox(_("Along the object's own axes"), &o.local);
	ImGui::SetItemTooltip(_("Off: world X, Y and Z. On: the first selected object's axes."));
	if(!o.random){
		ImGui::InputFloat3(_("Move##cloneoffset"), o.fixed, "%.3f");
		ImGui::InputFloat3(_("Turn##cloneoffset"), o.fixed + 3, "%.2f");
	}else{
		static const char *labels[6] = { "X", "Y", "Z", "RX", "RY", "RZ" };
		for(int i = 0; i < 6; i++){
			ImGui::PushID(i);
			float range[2] = { o.low[i], o.high[i] };
			if(ImGui::InputFloat2(labels[i], range, i < 3 ? "%.3f" : "%.2f")){
				o.low[i] = range[0];
				o.high[i] = range[1];
			}
			ImGui::PopID();
		}
		ImGui::TextDisabled(_("From and to: metres for X, Y and Z, degrees for the turns."));
	}
}

// ---- mirror ---------------------------------------------------------------

// Which of a model's sideways axes it is symmetric across: 0 for local X
// (left and right alike), 1 for local Y. Taken from where its collision
// mesh's area lies: a ramp rising along Y has it off the middle along Y.
static int
modelSymmetryAxis(int model)
{
	ObjectDef *obj = GetObjectDef(model);
	CColModel *col = obj ? obj->m_colModel : nil;
	if(col == nil)
		return 0;
	rw::V3d sum = { 0.0f, 0.0f, 0.0f };
	float total = 0.0f;
	for(int i = 0; i < col->numTriangles; i++){
		const CColTriangle *tri = &col->triangles[i];
		rw::V3d v[3];
		if(col->flags & 0x80){
			v[0] = col->compVertices[tri->a].Uncompress();
			v[1] = col->compVertices[tri->b].Uncompress();
			v[2] = col->compVertices[tri->c].Uncompress();
		}else{
			v[0] = col->vertices[tri->a];
			v[1] = col->vertices[tri->b];
			v[2] = col->vertices[tri->c];
		}
		float area = length(cross(sub(v[1], v[0]), sub(v[2], v[0]))) * 0.5f;
		sum = add(sum, scale(add(add(v[0], v[1]), v[2]), area / 3.0f));
		total += area;
	}
	for(int i = 0; i < col->numBoxes; i++){
		const CBox &b = col->boxes[i].box;
		rw::V3d size = sub(b.max, b.min);
		float area = 2.0f * (size.x * size.y + size.y * size.z + size.x * size.z);
		sum = add(sum, scale(add(b.min, b.max), area * 0.5f));
		total += area;
	}
	if(total <= 0.0f)
		return 0;
	rw::V3d centre = scale(sum, 1.0f / total);
	const CBox &box = col->boundingBox;
	float offX = fabsf(centre.x - (box.min.x + box.max.x) * 0.5f) / std::max(box.max.x - box.min.x, 0.01f);
	float offY = fabsf(centre.y - (box.min.y + box.max.y) * 0.5f) / std::max(box.max.y - box.min.y, 0.01f);
	return offY < offX - 0.02f ? 1 : 0;
}

// Objects can't be mirrored, only turned. A turned object can match the
// mirror image in all but one sideways axis, so it reverses the one its
// model is symmetric across: a ramp or stair then rises the mirrored way.
static rw::Quat
mirroredRotation(const rw::Quat &stored, int axis, int symmetryAxis)
{
	rw::Matrix m = basisOf(stored);
	auto flip = [axis](rw::V3d v){ setComp(v, axis, -comp(v, axis)); return v; };
	rw::Matrix w;
	w.setIdentity();
	w.right = flip(m.right);
	w.up = flip(m.up);
	w.at = flip(m.at);
	if(symmetryAxis == 0)
		w.right = scale(w.right, -1.0f);
	else
		w.up = scale(w.up, -1.0f);
	return QuatFromMatrix(w);
}

static bool
mirrorCheck(int axis, const std::vector<ObjectInst*> &objs, std::string *error)
{
	if(!needSelection(objs, 1, error))
		return false;
	if(axis < 0 || axis > 1){
		if(error) *error = N_("mirror across X or Y");
		return false;
	}
	return true;
}

static void
mirrorTransform(ObjectInst *inst, int axis, float plane, rw::V3d *pos, rw::Quat *rot)
{
	*pos = inst->m_translation;
	setComp(*pos, axis, 2.0f * plane - comp(*pos, axis));
	*rot = mirroredRotation(inst->m_rotation, axis, modelSymmetryAxis(inst->m_objectId));
}

bool
MapToolsMirrorPreview(int axis, const float *at, std::vector<rw::V3d> &positions,
	std::vector<rw::Quat> &rotations, float *plane, std::string *error)
{
	positions.clear();
	rotations.clear();
	std::vector<ObjectInst*> objs = selectedObjects();
	if(!mirrorCheck(axis, objs, error))
		return false;
	*plane = at ? *at : comp(groupCentre(objs), axis);
	for(size_t i = 0; i < objs.size(); i++){
		rw::V3d pos;
		rw::Quat rot;
		mirrorTransform(objs[i], axis, *plane, &pos, &rot);
		positions.push_back(pos);
		rotations.push_back(rot);
	}
	return true;
}

int
MapToolsMirror(int axis, const float *at, bool keepOriginal, std::string *error)
{
	std::vector<ObjectInst*> objs = selectedObjects();
	if(!mirrorCheck(axis, objs, error))
		return -1;
	float plane = at ? *at : comp(groupCentre(objs), axis);
	int changed = 0;
	UndoBeginGroup();
	std::vector<ObjectInst*> originals = objs;
	if(keepOriginal){
		// A copy in place, mirrored; the originals stay selected with it.
		reselect(objs);
		if(DuplicateSelected() <= 0){
			UndoEndGroup();
			if(error) *error = N_("could not copy the selection");
			return -1;
		}
		objs = selectedObjects();
	}
	for(size_t i = 0; i < objs.size(); i++){
		rw::V3d pos;
		rw::Quat rot;
		mirrorTransform(objs[i], axis, plane, &pos, &rot);
		if(placeObject(objs[i], pos, rot))
			changed++;
	}
	UndoEndGroup();
	if(keepOriginal){
		originals.insert(originals.end(), objs.begin(), objs.end());
		reselect(originals);
		return (int)objs.size();	// copies made, also those on the mirror itself
	}
	return changed;
}

// ---- align and distribute ---------------------------------------------------

int
MapToolsAlign(int axis, int mode, std::string *error)
{
	std::vector<ObjectInst*> objs = selectedObjects();
	if(!needSelection(objs, 2, error))
		return -1;
	axis = clamp(axis, 0, 2);
	mode = clamp(mode, 0, 2);
	auto edge = [&](ObjectInst *inst){
		rw::V3d mn, mx;
		instBox(inst, &mn, &mx);
		return mode == 0 ? comp(mn, axis) : mode == 2 ? comp(mx, axis) :
			(comp(mn, axis) + comp(mx, axis)) * 0.5f;
	};
	float target = edge(objs[0]);
	int changed = 0;
	UndoBeginGroup();
	for(size_t i = 1; i < objs.size(); i++){
		rw::V3d pos = objs[i]->m_translation;
		setComp(pos, axis, comp(pos, axis) + target - edge(objs[i]));
		if(placeObject(objs[i], pos, objs[i]->m_rotation))
			changed++;
	}
	UndoEndGroup();
	return changed;
}

int
MapToolsAlignToGuide(const rw::V3d &start, const rw::V3d *end, int axes, int anchor,
	bool distribute, bool faceDirection, std::string *error)
{
	std::vector<ObjectInst*> objs = selectedObjects();
	if(!needSelection(objs, distribute ? 2 : 1, error)) return -1;
	if(!axes) return 0;
	rw::V3d direction = end ? sub(*end, start) : rw::V3d{0,0,0};
	if(end && length(direction) < .001f){
		if(error) *error = N_("Pick two different points for the ruler.");
		return -1;
	}
	struct Target { ObjectInst *inst; rw::V3d pos, point; rw::Quat rotation; float t; };
	std::vector<Target> targets;
	rw::V3d groupMin = {0,0,0}, groupMax = {0,0,0};
	anchor = clamp(anchor, 0, 3);
	for(ObjectInst *inst : objs){
		if(!IsInstInIplMapDocument(inst)){
			if(error) *error = N_("The selection contains read-only reference objects.");
			return -1;
		}
		Target target;
		target.inst = inst; target.pos = inst->m_translation; target.rotation = inst->m_rotation;
		if(end && faceDirection){
			rw::V3d angles = GetObjectRotationDegrees(inst->m_rotation);
			angles.z = atan2f(direction.y, direction.x) * Alignment::Degrees;
			target.rotation = MakeObjectRotationDegrees(angles);
		}
		rw::V3d mn, mx;
		worldBox(inst->m_objectId, target.pos, target.rotation, &mn, &mx);
		target.point = anchor == 0 ? mn : anchor == 2 ? mx : anchor == 3 ? target.pos : scale(add(mn, mx), .5f);
		if(targets.empty()) groupMin = groupMax = target.point;
		for(int i = 0; i < 3; i++){
			setComp(groupMin, i, std::min(comp(groupMin, i), comp(anchor == 3 ? target.point : mn, i)));
			setComp(groupMax, i, std::max(comp(groupMax, i), comp(anchor == 3 ? target.point : mx, i)));
		}
		rw::V3d projectedDirection = direction, projectedPoint = sub(target.point, start);
		for(int i = 0; i < 3; i++) if(!(axes & (1 << i))){ setComp(projectedDirection, i, 0); setComp(projectedPoint, i, 0); }
		float denominator = dot(projectedDirection, projectedDirection);
		target.t = end && denominator > .000001f ? dot(projectedPoint, projectedDirection) / denominator : 0;
		targets.push_back(target);
	}
	if(distribute) std::stable_sort(targets.begin(), targets.end(), [](const Target &a, const Target &b){ return a.t < b.t; });
	rw::V3d groupAnchor = anchor == 0 ? groupMin : anchor == 2 ? groupMax : scale(add(groupMin, groupMax), .5f);
	int changed = 0;
	UndoBeginGroup();
	for(size_t i = 0; i < targets.size(); i++){
		Target &target = targets[i];
		rw::V3d destination = start;
		if(end) destination = add(start, scale(direction, distribute ? (float)i / (targets.size() - 1) : target.t));
		rw::V3d delta = sub(destination, end ? target.point : groupAnchor);
		for(int axis = 0; axis < 3; axis++) if(!(axes & (1 << axis))) setComp(delta, axis, 0);
		if(placeObject(target.inst, add(target.pos, delta), target.rotation)) changed++;
	}
	UndoEndGroup();
	return changed;
}

int
MapToolsDistribute(int axis, std::string *error)
{
	std::vector<ObjectInst*> objs = selectedObjects();
	if(!needSelection(objs, 3, error))
		return -1;
	axis = clamp(axis, 0, 2);
	auto centre = [&](ObjectInst *inst){
		rw::V3d mn, mx;
		instBox(inst, &mn, &mx);
		return (comp(mn, axis) + comp(mx, axis)) * 0.5f;
	};
	std::stable_sort(objs.begin(), objs.end(), [&](ObjectInst *a, ObjectInst *b){
		return centre(a) < centre(b);
	});
	float first = centre(objs.front()), last = centre(objs.back());
	int changed = 0;
	UndoBeginGroup();
	for(size_t i = 1; i + 1 < objs.size(); i++){
		float target = first + (last - first) * (float)i / (float)(objs.size() - 1);
		rw::V3d pos = objs[i]->m_translation;
		setComp(pos, axis, comp(pos, axis) + target - centre(objs[i]));
		if(placeObject(objs[i], pos, objs[i]->m_rotation))
			changed++;
	}
	UndoEndGroup();
	return changed;
}

int
MapToolsSnapRotations(float step, std::string *error)
{
	std::vector<ObjectInst*> objs = selectedObjects();
	if(!needSelection(objs, 1, error))
		return -1;
	if(step <= 0.0f){
		if(error) *error = N_("the step must be above 0");
		return -1;
	}
	int changed = 0;
	UndoBeginGroup();
	for(size_t i = 0; i < objs.size(); i++){
		rw::V3d deg = GetObjectRotationDegrees(objs[i]->m_rotation);
		deg = { roundf(deg.x / step) * step, roundf(deg.y / step) * step, roundf(deg.z / step) * step };
		if(placeObject(objs[i], objs[i]->m_translation, MakeObjectRotationDegrees(deg)))
			changed++;
	}
	UndoEndGroup();
	return changed;
}

// ---- drop and copy -------------------------------------------------------

int
MapToolsDropToGround(std::string *error)
{
	std::vector<ObjectInst*> objs = selectedObjects();
	if(!needSelection(objs, 1, error))
		return -1;
	return SnapSelectedToGround(false);
}

// Degrees for printing: float noise (44.999996, -0.0) rounded away.
static rw::V3d
cleanDegrees(const rw::V3d &d)
{
	auto clean = [](float v){ v = roundf(v * 10000.0f) / 10000.0f; return v == 0.0f ? 0.0f : v; };
	return { clean(d.x), clean(d.y), clean(d.z) };
}

static const char*
modelName(int model)
{
	ObjectDef *obj = GetObjectDef(model);
	return obj && obj->m_name[0] ? obj->m_name : "unknown";
}

std::string
MapToolsRelativeOffsetsPawn(bool attach, std::string *error)
{
	std::vector<ObjectInst*> objs = selectedObjects();
	if(!needSelection(objs, 2, error))
		return "";
	ObjectInst *ref = objs[0];
	const rw::V3d &rp = ref->m_translation;
	rw::V3d rr = cleanDegrees(GetObjectRotationDegrees(ref->m_rotation));
	std::string code;
	char line[512];
	if(attach){
		// AttachObjectToObject: the offset and rotation in the parent's frame
		// (rotation synced), as the editor's own attachments work it out.
		rw::Matrix parent = basisOf(ref->m_rotation);
		snprintf(line, sizeof(line), "// Attached to %s (model %d)\n"
			"new parent = CreateObject(%d, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f);\n",
			modelName(ref->m_objectId), ref->m_objectId, ref->m_objectId, rp.x, rp.y, rp.z, rr.x, rr.y, rr.z);
		code += line;
		for(size_t i = 1; i < objs.size(); i++){
			ObjectInst *inst = objs[i];
			rw::V3d d = sub(inst->m_translation, rp);
			rw::V3d off = { dot(d, parent.right), dot(d, parent.up), dot(d, parent.at) };
			rw::Matrix child = basisOf(inst->m_rotation);
			rw::Matrix local;
			local.setIdentity();
			local.right = { dot(child.right, parent.right), dot(child.right, parent.up), dot(child.right, parent.at) };
			local.up = { dot(child.up, parent.right), dot(child.up, parent.up), dot(child.up, parent.at) };
			local.at = { dot(child.at, parent.right), dot(child.at, parent.up), dot(child.at, parent.at) };
			rw::V3d lr = cleanDegrees(GetObjectRotationDegrees(QuatFromMatrix(local)));
			const rw::V3d &p = inst->m_translation;
			rw::V3d r = cleanDegrees(GetObjectRotationDegrees(inst->m_rotation));
			snprintf(line, sizeof(line),
				"new child%d = CreateObject(%d, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f); // %s\n"
				"AttachObjectToObject(child%d, parent, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, true);\n",
				(int)i, inst->m_objectId, p.x, p.y, p.z, r.x, r.y, r.z, modelName(inst->m_objectId),
				(int)i, off.x, off.y, off.z, lr.x, lr.y, lr.z);
			code += line;
		}
		return code;
	}
	// World-axis offsets from the first object, rotations as they are.
	snprintf(line, sizeof(line), "// Around %s (model %d), which is at %.6f, %.6f, %.6f\n"
		"stock CreateRelativeObjects(Float:x, Float:y, Float:z)\n{\n",
		modelName(ref->m_objectId), ref->m_objectId, rp.x, rp.y, rp.z);
	code += line;
	for(size_t i = 0; i < objs.size(); i++){
		ObjectInst *inst = objs[i];
		rw::V3d d = sub(inst->m_translation, rp);
		rw::V3d r = cleanDegrees(GetObjectRotationDegrees(inst->m_rotation));
		snprintf(line, sizeof(line),
			"\tCreateObject(%d, x %c %.6f, y %c %.6f, z %c %.6f, %.6f, %.6f, %.6f); // %s\n",
			inst->m_objectId, d.x < 0.0f ? '-' : '+', fabsf(d.x), d.y < 0.0f ? '-' : '+', fabsf(d.y),
			d.z < 0.0f ? '-' : '+', fabsf(d.z), r.x, r.y, r.z, modelName(inst->m_objectId));
		code += line;
	}
	code += "}\n";
	return code;
}

// ---- along a line -------------------------------------------------------------

float
MapToolsModelLength(int model, bool *alongX)
{
	rw::V3d mn, mx;
	modelBox(model, &mn, &mx);
	float lx = mx.x - mn.x, ly = mx.y - mn.y;
	if(alongX)
		*alongX = lx > ly;
	return std::max(lx, ly);
}

bool
MapToolsLayAlongPreview(const std::vector<rw::V3d> &points, int model, float spacing,
	float headingOffset, bool alongLength, bool followSlope, bool toGround,
	std::vector<rw::V3d> &positions, std::vector<rw::V3d> &degrees)
{
	positions.clear();
	degrees.clear();
	bool alongX = false;
	float modelLength = MapToolsModelLength(model, &alongX);
	if(spacing <= 0.0f)
		spacing = modelLength;
	if(alongLength && alongX)
		headingOffset += 90.0f;
	if(points.size() < 2 || spacing <= 0.01f)
		return false;
	const float r2d = 180.0f / 3.14159265f;
	float carry = 0.0f;	// distance into this segment of the next object
	for(size_t s = 0; s + 1 < points.size(); s++){
		rw::V3d a = points[s], d = sub(points[s + 1], a);
		float len = length(d);
		if(len < 0.001f)
			continue;
		float hlen = sqrtf(d.x * d.x + d.y * d.y);
		float heading = atan2f(-d.x, d.y) * r2d + headingOffset;
		float pitch = followSlope ? atan2f(d.z, hlen) * r2d : 0.0f;
		for(float t = carry; t <= len + 0.0001f; t += spacing){
			if((int)positions.size() >= MAX_TOOL_OBJECTS)
				return true;
			rw::V3d p = add(a, scale(d, t / len));
			if(toGround){
				rw::V3d hit;
				if(GetGroundPlacementSurface(p, &hit, nil, true))
					p.z = hit.z + GetPlacementBaseOffset(model);
			}
			positions.push_back(p);
			degrees.push_back({ pitch, 0.0f, heading });
			carry = t + spacing - len;
		}
	}
	return !positions.empty();
}

int
MapToolsLayAlong(const std::vector<rw::V3d> &points, int model, float spacing,
	float headingOffset, bool alongLength, bool followSlope, bool toGround, std::string *error)
{
	if(GetObjectDef(model) == nil){
		if(error) *error = "model " + std::to_string(model) + " does not exist";
		return -1;
	}
	std::vector<rw::V3d> positions, degrees;
	if(!MapToolsLayAlongPreview(points, model, spacing, headingOffset, alongLength, followSlope,
	                            toGround, positions, degrees)){
		if(error) *error = points.size() < 2 ? N_("the line needs at least two points") :
			N_("the spacing must be above 0");
		return -1;
	}
	// Placed like the Place tool would, then the previous spawn model back.
	int oldModel = GetSpawnObjectId(), oldKind = GetSpawnElementKind();
	SetSpawnObjectId(model);
	ClearSelection();
	std::vector<ObjectInst*> made;
	for(size_t i = 0; i < positions.size(); i++){
		rw::Quat rot = MakeObjectRotationDegrees(degrees[i]);
		ObjectInst *out[2];
		int n = SpawnPlaceObjectNoUndo(positions[i], &rot, out, 2);
		made.insert(made.end(), out, out + n);
	}
	SetSpawnObjectId(oldModel);
	SetSpawnElementKind(oldKind);
	if(made.empty()){
		if(error) *error = "could not place model " + std::to_string(model);
		return -1;
	}
	UndoRecordPaste(made.data(), (int)made.size());
	return (int)positions.size();
}

// ---- UI ----------------------------------------------------------------------

static bool gPicking;
static std::vector<rw::V3d> gPickedLine, gLinePoints;
static int gPanelFrame = -100, gLinePanelFrame = -100;
static MapToolsLineFeedback gLineFeedback;

const MapToolsLineFeedback &
GetMapToolsLineFeedback()
{
	return gLineFeedback;
}

bool
MapToolsPickingLine()
{
	if(gPicking && ImGui::GetFrameCount() - gPanelFrame > 2)
		gPicking = false;
	return gPicking;
}

void
ResetMapToolsLine()
{
	gPicking = false;
	gPickedLine.clear();
	gLinePoints.clear();
	gLineFeedback = {};
	gLinePanelFrame = gPanelFrame = -100;
}

// The cursor indicator and the actual click share the same surface query.
static rw::V3d
linePickPosition()
{
	Ray ray;
	ray.start = TheCamera.m_position;
	ray.dir = normalize(TheCamera.m_mouseDir);
	rw::V3d hit;
	if(GetVisibleInstUnderRay(ray, &hit) == nil){
		float t = fabsf(ray.dir.z) < 0.001f ? 50.0f : (TheCamera.m_target.z - ray.start.z) / ray.dir.z;
		hit = add(ray.start, scale(ray.dir, clamp(t, 1.0f, 5000.0f)));
	}
	return hit;
}

bool
MapToolsHandleViewportClick(void)
{
	if(!MapToolsPickingLine())
		return false;
	if(CPad::IsMButtonClicked(3) || CPad::IsMButtonClicked(2) || ShortcutPressed(SC_CANCEL)){
		gPicking = false;
		return true;
	}
	if(!ImGui::GetIO().WantCaptureMouse && CPad::IsMButtonClicked(1))
		gPickedLine.push_back(linePickPosition());
	return true;
}

void
MapToolsLineOverlay(bool enabled)
{
	gLineFeedback = {};
	if(!enabled){
		gPicking = false;
		return;
	}
	if(ImGui::GetFrameCount() != gLinePanelFrame)
		return;
	gLineFeedback.points = (int)gLinePoints.size();
	gLineFeedback.picking = MapToolsPickingLine();
	ImGuiViewport *viewport = ImGui::GetMainViewport();
	ImDrawList *draw = ImGui::GetBackgroundDrawList(viewport);
	float ui = GetUiScale();
	const ImU32 yellow = IM_COL32(255, 218, 60, 255), cyan = IM_COL32(80, 225, 255, 255);
	const ImU32 dark = IM_COL32(15, 20, 24, 240);
	auto project = [](rw::V3d position, ImVec2 *point){
		rw::V3d screen;
		float w, h;
		if(!Sprite::CalcScreenCoors(position, &screen, &w, &h, false)) return false;
		*point = EditorUiPoint(screen.x, screen.y);
		return true;
	};
	auto segment = [&](rw::V3d a, rw::V3d b, ImU32 colour){
		ImVec2 from, to;
		if(project(a, &from) && project(b, &to)){
			draw->AddLine(from, to, dark, 4 * ui);
			draw->AddLine(from, to, colour, 2 * ui);
		}
	};
	for(size_t i = 1; i < gLinePoints.size(); i++)
		segment(gLinePoints[i - 1], gLinePoints[i], yellow);
	for(size_t i = 0; i < gLinePoints.size(); i++){
		ImVec2 point;
		if(!project(gLinePoints[i], &point)) continue;
		if(point.x < viewport->Pos.x || point.y < viewport->Pos.y ||
		   point.x > viewport->Pos.x + viewport->Size.x || point.y > viewport->Pos.y + viewport->Size.y) continue;
		draw->AddCircleFilled(point, 7 * ui, dark);
		draw->AddCircle(point, 6 * ui, yellow, 0, 2 * ui);
		draw->AddCircleFilled(point, 2 * ui, yellow);
		char number[24];
		snprintf(number, sizeof(number), "%d", (int)i + 1);
		ImVec2 label(point.x + 10 * ui, point.y - ImGui::GetFontSize() - 6 * ui);
		ImVec2 size = ImGui::CalcTextSize(number);
		draw->AddRectFilled(ImVec2(label.x - 3 * ui, label.y - 2 * ui),
			ImVec2(label.x + size.x + 3 * ui, label.y + size.y + 2 * ui), dark, 3 * ui);
		draw->AddText(label, yellow, number);
		gLineFeedback.markers++;
	}
	if(gLineFeedback.picking && !ImGui::GetIO().WantCaptureMouse){
		gLineFeedback.cursor = linePickPosition();
		ImVec2 cursor;
		if(project(gLineFeedback.cursor, &cursor)){
			gLineFeedback.cursorVisible = true;
			if(!gLinePoints.empty()) segment(gLinePoints.back(), gLineFeedback.cursor, cyan);
			draw->AddCircleFilled(cursor, 9 * ui, dark);
			draw->AddCircle(cursor, 8 * ui, cyan, 0, 2 * ui);
			draw->AddLine(ImVec2(cursor.x - 12 * ui, cursor.y), ImVec2(cursor.x + 12 * ui, cursor.y), cyan, ui);
			draw->AddLine(ImVec2(cursor.x, cursor.y - 12 * ui), ImVec2(cursor.x, cursor.y + 12 * ui), cyan, ui);
			ImGui::SetMouseCursor(ImGuiMouseCursor_None);
		}
	}
}

static void
reportResult(int result, const std::string &error, const char *what)
{
	if(result < 0)
		ToastFor(TOAST_SELECTION, 4.0f, "%s: %s", _(what), _(error.c_str()));
	else
		Toast(TOAST_SELECTION, _("%s: %d object(s)"), _(what), result);
}

static void
drawBox(int model, const rw::V3d &pos, const rw::Quat &rot, rw::RGBA col)
{
	rw::V3d mn, mx;
	modelBox(model, &mn, &mx);
	rw::Matrix m = basisOf(rot);
	rw::V3d c[8];
	for(int i = 0; i < 8; i++)
		c[i] = add(pos, basisApply(m, { i & 1 ? mx.x : mn.x, i & 2 ? mx.y : mn.y, i & 4 ? mx.z : mn.z }));
	static const int edges[12][2] = {
		{ 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
		{ 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
	};
	for(int e = 0; e < 12; e++)
		RenderLine(c[edges[e][0]], c[edges[e][1]], col, col);
}

static const char *axisNames[] = { "X", "Y", "Z" };

static void
arraySection(const std::vector<ObjectInst*> &objs)
{
	static MapArrayParams params = { 3, false, { 0.0f, 5.0f, 0.0f }, { 0.0f, 0.0f, 0.0f },
		{ 0.0f, 0.0f, 0.0f }, 360.0f };
	static bool preview = true;
	int mode = params.radial ? 1 : 0;
	static const char *modes[] = { N_("Linear"), N_("Radial") };
	if(UiCombo(_("Mode"), &mode, modes, IM_ARRAYSIZE(modes))){
		params.radial = mode == 1;
		if(params.radial && length(params.centre) < 0.001f)
			params.centre = TheCamera.m_target;
	}
	ImGui::SetItemTooltip(_("Linear: each copy moves and turns by the same step.\n"
		"Radial: copies go around a centre point."));
	ImGui::InputInt(_("Copies"), &params.count);
	params.count = clamp(params.count, 1, 500);
	ImGui::SetItemTooltip(_("Copies of the selection, not counting it."));
	if(!params.radial){
		ImGui::InputFloat3(_("Offset"), &params.offset.x, "%.3f");
		ImGui::SetItemTooltip(_("How far each copy moves from the one before, in metres."));
		ImGui::InputFloat3(_("Rotate"), &params.rotate.x, "%.2f");
		ImGui::SetItemTooltip(_("How much each copy turns from the one before, in degrees\n"
			"around the world X, Y and Z axes, about the selection's centre."));
		ImGui::InputInt(_("Rows"), &params.rows);
		params.rows = clamp(params.rows, 1, 500);
		ImGui::SetItemTooltip(_("Repeats the whole line for a grid. 1 is a single line."));
		if(params.rows > 1){
			ImGui::InputFloat3(_("Row offset"), &params.rowOffset.x, "%.3f");
			ImGui::SetItemTooltip(_("How far each row moves from the one before, in metres."));
		}
	}else{
		ImGui::InputFloat3(_("Centre"), &params.centre.x, "%.3f");
		ImGui::SetItemTooltip(_("The point the copies go around (a vertical axis)."));
		if(ImGui::SmallButton(_("Selection centre")) && !objs.empty())
			params.centre = groupCentre(objs);
		ImGui::SameLine();
		if(ImGui::SmallButton(_("Camera target")))
			params.centre = TheCamera.m_target;
		ImGui::InputFloat(_("Angle"), &params.angle, 15.0f, 90.0f, "%.1f");
		ImGui::SetItemTooltip(_("The angle the copies cover. 360 spreads them evenly around\n"
			"a full circle together with the selection."));
	}
	ImGui::Checkbox(_("Preview"), &preview);
	ImGui::SetItemTooltip(_("Shows where the copies go before you apply."));
	ImGui::BeginDisabled(objs.empty());
	if(ImGui::Button(_("Apply##array"))){
		std::string error;
		reportResult(MapToolsArray(params, &error), error, N_("Array"));
	}
	ImGui::EndDisabled();
	if(preview && !objs.empty()){
		std::vector<rw::V3d> positions;
		std::vector<rw::Quat> rotations;
		if(MapToolsArrayPreview(params, positions, rotations, nil))
			for(size_t i = 0; i < positions.size(); i++)
				drawBox(objs[i % objs.size()]->m_objectId, positions[i], rotations[i],
					{ 90, 200, 255, 220 });
	}
}

static void
mirrorSection(const std::vector<ObjectInst*> &objs)
{
	static int axis = 0, around = 0;
	static float at[2] = { 0.0f, 0.0f };
	static bool keepOriginal = false, preview = true;
	static const char *axes[] = { N_("X (east-west)"), N_("Y (north-south)") };
	static const char *pivots[] = { N_("Selection centre"), N_("World origin"), N_("At") };
	UiCombo(_("Axis##mirror"), &axis, axes, IM_ARRAYSIZE(axes));
	ImGui::SetItemTooltip(_("Which coordinate flips: X mirrors east and west."));
	UiCombo(_("Around"), &around, pivots, IM_ARRAYSIZE(pivots));
	ImGui::SetItemTooltip(_("Where the mirror is: through the selection's centre, the\n"
		"world origin, or a coordinate you set."));
	if(around == 2){
		ImGui::InputFloat(axis == 0 ? "X##mirrorat" : "Y##mirrorat", &at[axis], 0.5f, 5.0f, "%.3f");
		ImGui::SetItemTooltip(_("The mirror's %s coordinate."), axis == 0 ? "X" : "Y");
		ImGui::SameLine();
		if(ImGui::SmallButton(_("Camera target")))
			at[axis] = axis == 0 ? TheCamera.m_target.x : TheCamera.m_target.y;
	}
	ImGui::Checkbox(_("Keep original"), &keepOriginal);
	ImGui::SetItemTooltip(_("Mirrors a copy, for symmetric builds: the selection stays."));
	ImGui::Checkbox(_("Preview##mirror"), &preview);
	ImGui::SetItemTooltip(_("Shows the mirror and where the objects go before you apply."));
	float origin = 0.0f;
	const float *plane = around == 0 ? nil : around == 1 ? &origin : &at[axis];
	ImGui::BeginDisabled(objs.empty());
	if(ImGui::Button(_("Apply##mirror_apply"))){
		std::string error;
		reportResult(MapToolsMirror(axis, plane, keepOriginal, &error), error, N_("Mirror"));
	}
	ImGui::SetItemTooltip(_("Objects are turned, not flipped: ramps and stairs rise the\n"
		"mirrored way, but a model that is lopsided both ways keeps its left and right."));
	ImGui::EndDisabled();
	std::vector<rw::V3d> positions;
	std::vector<rw::Quat> rotations;
	float where;
	if(preview && !objs.empty() &&
	   MapToolsMirrorPreview(axis, plane, positions, rotations, &where, nil)){
		// The mirror: a line across the selection at its height, then a box
		// where each object goes.
		rw::V3d mn = objs[0]->m_translation, mx = mn;
		for(ObjectInst *inst : objs)
			for(int c = 0; c < 3; c++){
				setComp(mn, c, std::min(comp(mn, c), comp(inst->m_translation, c)));
				setComp(mx, c, std::max(comp(mx, c), comp(inst->m_translation, c)));
			}
		int other = 1 - axis;
		float from = comp(mn, other) - 10.0f, to = comp(mx, other) + 10.0f;
		rw::RGBA lineCol = { 255, 200, 40, 230 };
		for(float z : { mn.z, mx.z + 2.0f }){
			rw::V3d a = { 0.0f, 0.0f, z }, b = a;
			setComp(a, axis, where); setComp(a, other, from);
			setComp(b, axis, where); setComp(b, other, to);
			RenderLine(a, b, lineCol, lineCol);
		}
		for(size_t i = 0; i < positions.size(); i++)
			drawBox(objs[i]->m_objectId, positions[i], rotations[i], { 90, 200, 255, 220 });
	}
}

static void
alignSection(const std::vector<ObjectInst*> &objs)
{
	static int axis = 2;
	static float step = 90.0f;
	ImGui::Combo(_("Axis##align"), &axis, axisNames, IM_ARRAYSIZE(axisNames));
	ImGui::BeginDisabled(objs.size() < 2);
	static const char *modes[] = { N_("Min"), N_("Centre"), N_("Max") };
	for(int m = 0; m < 3; m++){
		if(m > 0)
			ImGui::SameLine();
		if(ImGui::Button(_(modes[m]))){
			std::string error;
			reportResult(MapToolsAlign(axis, m, &error), error, N_("Align"));
		}
		ImGui::SetItemTooltip(_("Lines up the %s of each object's box with the first\n"
			"selected object's, along the axis."), m == 0 ? _("lowest side") : m == 1 ? _("centre") : _("highest side"));
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(objs.size() < 3);
	if(ImGui::Button(_("Distribute"))){
		std::string error;
		reportResult(MapToolsDistribute(axis, &error), error, N_("Distribute"));
	}
	ImGui::SetItemTooltip(_("Spaces the objects evenly along the axis between the two\n"
		"outermost ones, which stay where they are."));
	ImGui::EndDisabled();
	ImGui::InputFloat(_("Step##snaprot"), &step, 5.0f, 15.0f, "%.1f");
	step = clamp(step, 0.1f, 180.0f);
	ImGui::SameLine();
	ImGui::BeginDisabled(objs.empty());
	if(ImGui::Button(_("Snap rotations"))){
		std::string error;
		reportResult(MapToolsSnapRotations(step, &error), error, N_("Snap rotations"));
	}
	ImGui::SetItemTooltip(_("Rounds every rotation angle to this step."));
	ImGui::EndDisabled();
}

static void
dropSection(const std::vector<ObjectInst*> &objs)
{
	ImGui::BeginDisabled(objs.empty());
	if(ImGui::Button(_("Drop to ground"))){
		std::string error;
		reportResult(MapToolsDropToGround(&error), error, N_("Drop to ground"));
	}
	ImGui::SetItemTooltip(_("Puts each object on the ground below it."));
	ImGui::EndDisabled();
	ImGui::BeginDisabled(objs.size() < 2);
	for(int attach = 1; attach >= 0; attach--){
		if(attach == 0)
			ImGui::SameLine();
		if(ImGui::Button(attach ? _("Copy as attachments") : _("Copy as offsets"))){
			std::string error;
			std::string code = MapToolsRelativeOffsetsPawn(attach == 1, &error);
			if(code.empty())
				ToastFor(TOAST_COPY_PASTE, 4.0f, _("Copy: %s"), _(error.c_str()));
			else{
				ImGui::SetClipboardText(code.c_str());
				Toast(TOAST_COPY_PASTE, _("Copied %d object(s) as Pawn"), (int)objs.size());
			}
		}
		ImGui::SetItemTooltip("%s", attach ?
			_("Copies the selection as AttachObjectToObject calls on the first\n"
			"selected object, with offsets in its frame.") :
			_("Copies the selection as CreateObject calls at x, y, z plus each\n"
			"object's offset from the first selected one."));
	}
	ImGui::EndDisabled();
}

static void
lineSection(void)
{
	gLinePanelFrame = ImGui::GetFrameCount();
	static int source = 0;	// 0 = picked, else NPC path source-1
	static int model = 1228;	// roadworkbarrier1
	static float spacing = 3.0f, heading = 0.0f;
	static bool alongLength = true, followSlope = false, toGround = true, preview = true;
	std::vector<std::string> paths;
	GetNpcPathNames(paths);
	if(source > (int)paths.size())
		source = 0;
	const char *current = source == 0 ? _("Picked points") : paths[source - 1].c_str();
	if(ImGui::BeginCombo(_("Line"), current)){
		if(ImGui::Selectable(_("Picked points"), source == 0))
			source = 0;
		for(size_t i = 0; i < paths.size(); i++){
			char label[96];
			snprintf(label, sizeof(label), _("NPC path %s"), paths[i].c_str());
			if(ImGui::Selectable(label, source == (int)i + 1))
				source = (int)i + 1;
		}
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip(_("Points you click in the world, or an NPC path's points in order."));
	std::vector<rw::V3d> points;
	if(source == 0){
		if(ImGui::Button(gPicking ? _("Stop picking") : _("Pick points"))){
			gPicking = !gPicking;
			if(gPicking){
				SpawnExitPlaceMode();
				ExitPrefabPlaceMode();
				ExitBrushMode();
				gRectSelectActive = false;
			}
		}
		ImGui::SetItemTooltip(_("Click points in the world one after another.\n"
			"Right-click or Esc stops."));
		ImGui::SameLine();
		if(ImGui::Button(_("Clear")))
			gPickedLine.clear();
		ImGui::SameLine();
		ImGui::TextDisabled(_("%d points"), (int)gPickedLine.size());
		points = gPickedLine;
	}else{
		std::vector<ObjectInst*> pathPoints;
		GetNpcPathPoints(paths[source - 1].c_str(), pathPoints);
		for(size_t i = 0; i < pathPoints.size(); i++)
			points.push_back(pathPoints[i]->m_translation);
		gPicking = false;
	}
	ImGui::InputInt(_("Model"), &model, 0, 0);
	ImGui::SetItemTooltip("%s", GetObjectDef(model) ? modelName(model) : _("no such model"));
	ImGui::SameLine();
	std::vector<ObjectInst*> objs = selectedObjects();
	ImGui::BeginDisabled(objs.empty());
	if(ImGui::SmallButton(_("Selected")))
		model = objs[0]->m_objectId;
	ImGui::SetItemTooltip(_("Use the first selected object's model."));
	ImGui::EndDisabled();
	ImGui::InputFloat(_("Spacing"), &spacing, 0.5f, 2.0f, "%.2f m");
	spacing = clamp(spacing, 0.1f, 1000.0f);
	ImGui::SameLine();
	ImGui::BeginDisabled(GetObjectDef(model) == nil);
	if(ImGui::SmallButton(_("Fit")))
		spacing = std::max(MapToolsModelLength(model, nil), 0.1f);
	ImGui::SetItemTooltip(_("The model's length, so the pieces just touch."));
	ImGui::EndDisabled();
	ImGui::Checkbox(_("Along its length"), &alongLength);
	ImGui::SetItemTooltip(_("Turns models that are longer sideways (most fences and barriers)\n"
		"so they run along the line."));
	ImGui::InputFloat(_("Heading"), &heading, 15.0f, 90.0f, "%.1f");
	ImGui::SetItemTooltip(_("Added to the line's direction, in degrees: 90 turns them sideways."));
	ImGui::Checkbox(_("Follow slope"), &followSlope);
	ImGui::SetItemTooltip(_("Tilts each object up or down with the line."));
	ImGui::SameLine();
	ImGui::Checkbox(_("On ground"), &toGround);
	ImGui::SetItemTooltip(_("Puts each object on the ground below the line."));
	ImGui::Checkbox(_("Preview##line"), &preview);
	std::vector<rw::V3d> positions, degrees;
	bool ok = MapToolsLayAlongPreview(points, model, spacing, heading, alongLength, followSlope,
		toGround, positions, degrees);
	ImGui::SameLine();
	ImGui::BeginDisabled(!ok || GetObjectDef(model) == nil);
	if(ImGui::Button(_("Apply##line"))){
		std::string error;
		reportResult(MapToolsLayAlong(points, model, spacing, heading, alongLength, followSlope,
			toGround, &error),
			error, N_("Along a line"));
	}
	ImGui::EndDisabled();
	gLinePoints = points;
	if(preview && ok && GetObjectDef(model))
		for(size_t i = 0; i < positions.size(); i++)
			drawBox(model, positions[i], MakeObjectRotationDegrees(degrees[i]), { 90, 200, 255, 220 });
}

void
MapToolsPanel(void)
{
	gPanelFrame = ImGui::GetFrameCount();
	std::vector<ObjectInst*> objs = selectedObjects();
	bool lineOpen = false;
	if(ImGui::CollapsingHeader(L("Coordinate pin"))){
		CoordinatePinPanel();
		if(CoordinatePinOwnsMouse()) gPicking = false;
	}
	if(ImGui::CollapsingHeader(L("Paths & curves"))){
		ImGui::PushID("layout_paths");
		LayoutPathsPanel();
		ImGui::PopID();
	}
	if(ImGui::CollapsingHeader(L("Duplicate cleanup"))){
		ImGui::PushID("duplicate_cleanup");
		MapCleanupPanel();
		ImGui::PopID();
	}
	if(ImGui::CollapsingHeader(L("Pivot"))){
		ImGui::PushID("mapping_pivot");
		const char *pivots[]={_("Active object"),_("Selection centre"),_("Individual origins"),_("Custom point")};
		ImGui::Combo(L("Pivot mode"),&gGizmoPivot,pivots,4);
		if(gGizmoPivot==GIZMO_PIVOT_CUSTOM){
			ImGui::DragFloat3(L("Pivot position"),&gGizmoCustomPivot.x,.1f,-100000.f,100000.f,"%.3f");
			if(ImGui::SmallButton(L("Selection centre"))) gGizmoCustomPivot=MapToolsSelectionCentre();
			ImGui::SameLine();if(ImGui::SmallButton(L("Camera target"))) gGizmoCustomPivot=TheCamera.m_target;
			rw::V3d pin;bool hasPin=CoordinatePinPosition(&pin);
			ImGui::BeginDisabled(!hasPin);if(ImGui::SmallButton(L("Use active pin")))gGizmoCustomPivot=pin;ImGui::EndDisabled();
		}
		ImGui::BeginDisabled(objs.empty());
		if(ImGui::Button(L("Center Parent"))){std::string error;reportResult(MapToolsCenterParent(&error),error,N_("Center Parent"));}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip(_("Moves the attachment root to its family's bounding-box centre while keeping every child's world position and rotation."));
		ImGui::PopID();
	}
	if(ImGui::CollapsingHeader(L("Array")))
		arraySection(objs);
	if(ImGui::CollapsingHeader(L("Mirror")))
		mirrorSection(objs);
	if(ImGui::CollapsingHeader(L("Duplicate offset"), gCloneOffset.enabled ? ImGuiTreeNodeFlags_DefaultOpen : 0))
		cloneOffsetSection();
	if(ImGui::CollapsingHeader(L("Align & distribute")))
		alignSection(objs);
	if(ImGui::CollapsingHeader(L("Drop & copy as Pawn")))
		dropSection(objs);
	if(ImGui::CollapsingHeader(L("Along a line"), gPicking ? ImGuiTreeNodeFlags_DefaultOpen : 0)){
		lineOpen = true;
		lineSection();
	}
	if(!lineOpen)
		gPicking = false;	// no picking behind a closed section
}
