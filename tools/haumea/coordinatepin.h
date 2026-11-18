// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <vector>
#include <string>
namespace rw { struct V3d; }
struct ImVec2;
namespace EditorXML { struct Node; }

void CoordinatePinPanel();
void CoordinatePinOverlay(bool enabled);
bool CoordinatePinHandleViewport();
bool CoordinatePinOwnsMouse();
bool CoordinatePinPosition(rw::V3d *position, rw::V3d *end = nullptr);
// handle: 0 pin head (move), 1 base (measure), 2 endpoint (move).
bool CoordinatePinScreen(ImVec2 *point, int handle = 0, int index = -1);
int CoordinatePinCount();
// A new active pin (with a ruler to rulerEnd when given), or removing the
// active one or all: for Namaka, so it can point at places on the user's map.
void CoordinatePinPlace(const rw::V3d &position, const rw::V3d *rulerEnd = nullptr);
int CoordinatePinClear(bool all);
int CoordinatePinSnapModel();
bool CoordinatePinAnchorSnapped();
// The same guides used by the alignment panel are targets for N / object snap.
void CoordinatePinSnapTargets(int area, const rw::V3d &centre, std::vector<rw::V3d> &targets);
EditorXML::Node WriteCoordinatePinXML();
bool ReadCoordinatePinXML(const EditorXML::Node &node, std::string *error, bool apply = true);
void ResetCoordinatePins();
