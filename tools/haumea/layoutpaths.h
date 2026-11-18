// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "editorxml.h"
#include <map>
#include <memory>
#include <string>
#include <vector>
struct ImVec2;
void LayoutPathsPanel();
void LayoutPathsOverlay(bool enabled);
bool LayoutPathsHandleViewport();
bool LayoutPathsGizmo();
bool LayoutPathsOwnsMouse();
void ResetLayoutPaths();
EditorXML::Node WriteLayoutPathsXML();
bool ReadLayoutPathsXML(const EditorXML::Node &, std::string *error, bool apply = true);
void RestoreLayoutPaths(const std::shared_ptr<void> &);
int LayoutPathCount();
bool LayoutPathPointScreen(ImVec2 *, int index, int handle = 0); // 0 knot, 1 outgoing, 2 midpoint
float LayoutPathPointHeight(int index);
int LayoutPathObjectCount();
int LayoutPathUpsideDownCount();
bool LayoutPathObjectsMatch();
void LayoutPathsReferencedObjects(std::vector<int> &out);
// Namaka's way in (the agent bridge): one operation on the paths with its
// fields as text (key=value), answered as JSON members in *json. Every change
// is one undo step, like the panel's. ops: list, get, new, set, add_point,
// insert_point, remove_point, move_point, generate, detach, delete.
bool LayoutPathsAgent(const std::string &op, const std::map<std::string, std::string> &fields,
	std::string *json, std::string *error);
// The whole state, for an agent session to put back on rollback (RestoreLayoutPaths).
std::shared_ptr<void> LayoutPathsSnapshot();
