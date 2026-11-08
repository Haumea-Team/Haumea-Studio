// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <vector>

namespace SAPaths {

struct Node;
struct BackupFile {
	char logicalPath[256];
	char sourcePath[1024];
	std::vector<uint8> data;
};

extern Node *hoveredNode;
extern Node *selectedNode;

void Reset(void);
void EnsureLoaded(void);
bool HasSelectedNode(void);
bool GetSelectedNodePosition(rw::V3d *pos);
bool SetSelectedNodePosition(const rw::V3d &pos, bool notifyChange);
void RenderCarPaths(void);
// --dump-zone-settings: the zones' start-up population as read from
// main.scm, one zone per line after a count line; false if it can't write.
bool DumpZoneSettings(const char *path);
void RenderPedPaths(void);
void RenderAreaGrid(void);
void PrepareEditorVehicleVisuals(rw::Clump *clump);
bool PrepareEditorVehicleClump(rw::Clump *clump, int color1, int color2,
	uint8 **colorRoles, int *numColorRoles);
void ApplyEditorVehicleClumpColors(rw::Clump *clump, const uint8 *colorRoles,
	int numColorRoles, int color1, int color2);
bool HasInfoToShow(void);
void DrawInfoPanel(void);
int GetDirtyAreaCount(void);
bool HasDirtyAreas(void);
bool CollectDirtyAreaBackupFiles(std::vector<BackupFile> &files);
void CommitSelectedNodeEdit(void);

// open.mp Nodes tool. Point ids are a node's index in its NODES<area>.DAT.
bool NodesLoaded(void);
bool IsVehicleNode(const Node *node);
bool GetNodeInfo(const Node *node, int *areaId, int *pointId, bool *vehicle, rw::V3d *pos);
int GetNodeLinks(const Node *node, Node **targets, int maxTargets);
Node *FindNode(int areaId, int pointId);
void GetAreaCounts(int areaId, int *vehicleNodes, int *pedNodes, int *links);
bool IsAreaDirty(int areaId);
bool AreNodesLinked(const Node *a, const Node *b);
// reason: why it failed, in English (N_); show it with _(reason).
bool LinkNodes(Node *a, Node *b, const char **reason);
bool UnlinkNodes(Node *a, Node *b);
Node *AddNode(const rw::V3d &pos, bool vehicle, Node *linkTo, const char **reason);
bool DeleteNode(Node *node);
bool IsGameNodesFolder(const char *dir);
bool SaveAreasToFolder(const char *dir, int *saved, char *err, size_t errSize);
bool LoadAreasFromFolder(const char *dir, int *loaded, char *err, size_t errSize);
const char *GetFolderSource(void);
void ReloadGameNodes(void);
void DrawNodesWindow(bool *open);	// nodes_ui.cpp
// Undo: once a frame, records finished node edits on the editor's undo
// stack; RestoreNodeAreas puts areas back from it.
void UpdateNodesUndo(void);
uint64 NodesFingerprint(void);	// of every loaded area, for tests
// Multiplayer: an area's NODES<area>.DAT bytes, when it was edited here or
// came from the session (else false), and the session's put in place.
uint32 NodesEditSerial(void);	// moves with every node edit
bool GetSharedAreaData(int areaId, std::vector<uint8> &data);
bool SetSharedAreaData(int areaId, const std::vector<uint8> &data);
void RestoreNodeAreas(const std::vector<int> &areas, const std::vector<std::shared_ptr<void>> &states);
bool HandleNodesViewportClick(void);	// nodes_ui.cpp

}
