// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

// Transform tools for the selection, as in Texture Studio and MTA's editor.
// Each action is one undo step and returns how many objects it changed or
// made, or -1 with *error set.

struct MapArrayParams {
	int count;	// copies, not counting the selection
	bool radial;	// false: linear
	// Linear: copy k is moved k * offset and turned k * rotate (degrees,
	// world axes) around the selection's centre.
	rw::V3d offset;
	rw::V3d rotate;
	// Radial: copies turned around a vertical axis through centre, spread
	// over angle degrees (360: a full circle with the selection included).
	rw::V3d centre;
	float angle;
	// Linear only: the line (the selection and its copies) repeated rows
	// times, each row_offset metres from the one before, for a grid.
	int rows = 1;
	rw::V3d rowOffset = { 5.0f, 0.0f, 0.0f };
};

// Where each copy's objects would go, for previews: for every copy, the
// selection's objects in selection order (position, stored rotation).
bool MapToolsArrayPreview(const MapArrayParams &params, std::vector<rw::V3d> &positions,
	std::vector<rw::Quat> &rotations, std::string *error);
int MapToolsArray(const MapArrayParams &params, std::string *error);

// axis: 0 = X (flip east-west), 1 = Y (flip north-south). The plane goes
// through *at on that axis, or the selection's centre when at is nil.
// keepOriginal mirrors a copy and leaves the selection where it is.
// Duplicate (Ctrl+D) with an offset:
// each copy moved and turned by a fixed amount or a random one within
// ranges, in world axes or the first selected object's own.
struct MapCloneOffset {
	bool enabled = false;
	bool random = false;
	bool local = false;
	float fixed[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };	// x y z metres, rx ry rz degrees
	float low[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }, high[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
};
MapCloneOffset &MapToolsCloneOffset(void);
// Duplicates the selection, then moves the copies by the offset when it's
// on; one undo step. Returns the copies, or -1.
int MapToolsDuplicate(std::string *error);

int MapToolsMirror(int axis, const float *at, bool keepOriginal, std::string *error);
// Where the selection's objects would go (selection order), and the mirror's
// coordinate on that axis, for previews.
bool MapToolsMirrorPreview(int axis, const float *at, std::vector<rw::V3d> &positions,
	std::vector<rw::Quat> &rotations, float *plane, std::string *error);

// axis 0-2 (X, Y, Z); mode 0 = min, 1 = centre, 2 = max of the bounding
// boxes, to the first selected object.
int MapToolsAlign(int axis, int mode, std::string *error);
// axes is an XYZ bitmask; anchor is min/centre/max/origin (0..3).
// With an end, project individual anchors onto the infinite ruler or distribute
// them between its ends. Without one, align the whole selection to the pin.
int MapToolsAlignToGuide(const rw::V3d &start, const rw::V3d *end, int axes, int anchor,
	bool distribute, bool faceDirection, std::string *error);
// Even centre spacing between the two outermost objects along axis.
int MapToolsDistribute(int axis, std::string *error);
int MapToolsSnapRotations(float step, std::string *error);

int MapToolsDropToGround(std::string *error);
// The selection relative to the first selected object, as Pawn:
// AttachObjectToObject calls (attach) or a table of offsets.
std::string MapToolsRelativeOffsetsPawn(bool attach, std::string *error);

// Copies of model every spacing metres along the polyline (0: the model's
// length), facing along it (heading + headingOffset; alongLength turns a
// model that is longer in X, like most fences, to run along the line;
// followSlope also tilts them), optionally dropped to the ground.
int MapToolsLayAlong(const std::vector<rw::V3d> &points, int model, float spacing,
	float headingOffset, bool alongLength, bool followSlope, bool toGround, std::string *error);
bool MapToolsLayAlongPreview(const std::vector<rw::V3d> &points, int model, float spacing,
	float headingOffset, bool alongLength, bool followSlope, bool toGround,
	std::vector<rw::V3d> &positions, std::vector<rw::V3d> &degrees);

// The model's length along its longer sideways axis, and whether that is X.
float MapToolsModelLength(int model, bool *alongX);

struct MapToolsLineFeedback {
	int points = 0, markers = 0;
	bool picking = false, cursorVisible = false;
	rw::V3d cursor = {0, 0, 0};
};
const MapToolsLineFeedback &GetMapToolsLineFeedback();
bool MapToolsPickingLine();
void MapToolsLineOverlay(bool enabled);
void ResetMapToolsLine();

// Viewport clicks while picking a line for "Along a line"; true when used.
bool MapToolsHandleViewportClick(void);
// The Tools window's sections.
void MapToolsPanel(void);
rw::V3d MapToolsSelectionCentre();
int MapToolsCenterParent(std::string *error);
