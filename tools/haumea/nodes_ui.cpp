// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "sapaths.h"
#include "workspace.h"
#include "icons.h"
#include "shortcuts.h"
#include "i18n.h"

#include <string.h>
#include <stdio.h>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

// The Nodes window: edits copies of NODES0-63.DAT for open.mp NPCs
// (NPC_PlayNode, NPC_OpenNode). Saving writes into a folder of your choice,
// such as a server's scriptfiles/NPCs/nodes, never into the game's files.

// Panel layout helpers, defined in maptools.cpp.
float MapToolsButtonWidth(int n);
float MapToolsWidthBeforeButton(const char *label);
bool MapToolsMainButton(const char *label, const ImVec2 &size = ImVec2(-FLT_MIN, 0.0f));

namespace SAPaths {

enum NodesMode {
	NODES_SELECT,
	NODES_ADD_VEHICLE,
	NODES_ADD_PED,
	NODES_LINK,
};

static int gMode = NODES_SELECT;
static bool gChain = true;
static char gFolder[1024];
static bool gFolderRead;
static int gGotoArea, gGotoPoint;

static void
readFolderSetting(void)
{
	if(gFolderRead)
		return;
	gFolderRead = true;
	char path[1024];
	if(!GetHaumeaDataPath(path, sizeof(path), "nodes_folder.txt"))
		return;
	FILE *f = fopen(path, "r");
	if(f == nil)
		return;
	if(fgets(gFolder, sizeof(gFolder), f)){
		size_t n = strlen(gFolder);
		while(n > 0 && (gFolder[n-1] == '\n' || gFolder[n-1] == '\r'))
			gFolder[--n] = '\0';
	}
	fclose(f);
}

// Without a saved folder, the workspace's scriptfiles/NPCs/nodes.
static void
defaultFolderFromWorkspace(void)
{
	if(gFolder[0] == '\0' && GetWorkspaceDir()[0])
		snprintf(gFolder, sizeof(gFolder), "%s", WorkspacePath("scriptfiles/NPCs/nodes").c_str());
}

static void
writeFolderSetting(void)
{
	char path[1024];
	if(!GetHaumeaDataPath(path, sizeof(path), "nodes_folder.txt"))
		return;
	FILE *f = fopen(path, "w");
	if(f == nil)
		return;
	fprintf(f, "%s\n", gFolder);
	fclose(f);
}

static void
showNodeKind(bool vehicle)
{
	if(vehicle)
		gRenderSaCarPaths = true;
	else
		gRenderSaPedPaths = true;
}

static void
jumpToNode(Node *node)
{
	rw::V3d pos;
	if(!GetNodeInfo(node, nil, nil, nil, &pos))
		return;
	selectedNode = node;
	TheCamera.setTarget(pos);
	TheCamera.setDistanceFromTarget(25.0f);
}

bool
HandleNodesViewportClick(void)
{
	if(gMode == NODES_SELECT)
		return false;
	if(CPad::IsMButtonClicked(3) || CPad::IsMButtonClicked(2) || ShortcutPressed(SC_CANCEL)){
		gMode = NODES_SELECT;
		return true;
	}
	if(!CPad::IsMButtonClicked(1))
		return true;

	if(gMode == NODES_LINK){
		Node *target = hoveredNode;
		if(target == nil){
			Toast(TOAST_SELECTION, "%s", _("Click a node to link it"));
			return true;
		}
		if(selectedNode == nil || selectedNode == target){
			selectedNode = target;
			return true;
		}
		Node *from = selectedNode;
		if(AreNodesLinked(from, target)){
			UnlinkNodes(from, target);
			Toast(TOAST_SPAWN, "%s", _("Unlinked the nodes"));
		}else{
			const char *reason = nil;
			if(LinkNodes(from, target, &reason))
				Toast(TOAST_SPAWN, "%s", _("Linked the nodes"));
			else
				Toast(TOAST_SPAWN, _("Can't link: %s"), reason ? _(reason) : _("unknown"));
		}
		// Linking doesn't move nodes in memory, so target is still valid.
		// The selection moves on, so a route is clicked node by node.
		selectedNode = target;
		return true;
	}

	bool vehicle = gMode == NODES_ADD_VEHICLE;
	rw::V3d hit, normal;
	GetPlacementSurfaceHit(&hit, &normal);
	Node *linkTo = gChain && selectedNode && IsVehicleNode(selectedNode) == vehicle ? selectedNode : nil;
	const char *reason = nil;
	Node *added = AddNode(hit, vehicle, linkTo, &reason);
	if(added == nil)
		Toast(TOAST_SPAWN, _("Can't add a node: %s"), reason ? _(reason) : _("unknown"));
	else if(reason)
		Toast(TOAST_SPAWN, _("Added the node, but couldn't link it: %s"), _(reason));
	showNodeKind(vehicle);
	return true;
}

// One of the tool buttons sharing a row; the tool in use is filled with the
// accent.
static void
drawModeButton(const char *label, int mode, const char *tooltip, float width)
{
	bool active = gMode == mode;
	bool pressed = active ? MapToolsMainButton(label, ImVec2(width, 0.0f))
		: ImGui::Button(label, ImVec2(width, 0.0f));
	if(pressed)
		gMode = active ? NODES_SELECT : mode;
	ImGui::SetItemTooltip("%s", tooltip);
}

// Buttons of the same width for a popup that sizes itself to its contents.
static float
popupButtonWidth(const char *a, const char *b)
{
	float text = std::max(ImGui::CalcTextSize(a, nil, true).x, ImGui::CalcTextSize(b, nil, true).x);
	return std::max(text + ImGui::GetStyle().FramePadding.x * 2.0f, ImGui::GetFontSize() * 6.0f);
}

static void
drawSelectedNode(void)
{
	Node *node = selectedNode;
	int area = 0, point = 0;
	bool vehicle = false;
	rw::V3d pos;
	if(!GetNodeInfo(node, &area, &point, &vehicle, &pos)){
		ImGui::TextDisabled("%s", _("No node selected. Click one in the viewport."));
		return;
	}
	ImGui::Text(vehicle ? _("Vehicle node %d in NODES%d.DAT") : _("Ped node %d in NODES%d.DAT"), point, area);
	ImGui::SetItemTooltip(_("In Pawn: NPC_OpenNode(%d), then NPC_PlayNode(npcid, %d, ...).\n"
		"The point id is the node's place in its file; adding or deleting\n"
		"nodes in that file renumbers the ones after it."), area, area);

	float p[3] = { pos.x, pos.y, pos.z };
	if(UiDragFloat3(L("Position"), p, 0.125f, -3000.0f, 3000.0f, "%.3f")){
		rw::V3d np = { p[0], p[1], p[2] };
		SetSelectedNodePosition(np, false);
	}
	if(ImGui::IsItemDeactivatedAfterEdit())
		CommitSelectedNodeEdit();
	ImGui::SetItemTooltip("%s", _("Stored in steps of 0.125. A node can't leave its 750x750 area;\n"
		"press W to drag it with the gizmo instead."));

	// The links: one row each, the node it goes to filling the row and its
	// Unlink button at the end.
	Node *links[16];
	int numLinks = GetNodeLinks(node, links, 16);
	ImGui::Spacing();
	ImGui::TextDisabled(_("Links: %d / 15"), numLinks);
	const char *unlinkLabel = LI(ICON_FA_XMARK, "Unlink");
	const ImGuiStyle &style = ImGui::GetStyle();
	float unlinkWidth = ImGui::CalcTextSize(unlinkLabel, nil, true).x + style.FramePadding.x * 2.0f;
	for(int i = 0; i < numLinks; i++){
		int la = 0, lp = 0;
		GetNodeInfo(links[i], &la, &lp, nil, nil);
		ImGui::PushID(i);
		const char *otherFile = la != area ? _("(other file)") : nil;
		float rest = unlinkWidth + style.ItemSpacing.x;
		if(otherFile)
			rest += ImGui::CalcTextSize(otherFile).x + style.ItemSpacing.x;
		char label[48];
		snprintf(label, sizeof(label), "%d:%d", la, lp);
		if(ImGui::Button(label, ImVec2(std::max(ImGui::GetContentRegionAvail().x - rest, ImGui::GetFontSize() * 3.0f), 0.0f)))
			jumpToNode(links[i]);
		ImGui::SetItemTooltip("%s", _("Select and go to this node."));
		if(otherFile){
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s", otherFile);
			ImGui::SetItemTooltip(_("An NPC reaching this link leaves NODES%d.DAT;\nopen.mp then reports a node change."), area);
		}
		ImGui::SameLine();
		if(ImGui::Button(unlinkLabel)){
			UnlinkNodes(node, links[i]);
			ImGui::PopID();
			return;
		}
		ImGui::PopID();
	}

	ImGui::Spacing();
	if(ImGui::Button(LI(ICON_FA_TRASH, "Delete node"), ImVec2(-FLT_MIN, 0.0f)))
		DeleteNode(node);
	ImGui::SetItemTooltip(_("Removes the node and its links. Later nodes in NODES%d.DAT move up one point id."), area);
}

// Two jobs, one at a time: Edit (add, link, move and delete nodes) and Go to
// (a node by its file and point id).
enum { NODES_TASK_EDIT, NODES_TASK_GOTO };
static int gTask = NODES_TASK_EDIT;

static void
drawEditTask(void)
{
	// The tools: four equal buttons, the one in use filled.
	float quarter = MapToolsButtonWidth(4);
	drawModeButton(LI(ICON_FA_ARROW_POINTER, "Select"), NODES_SELECT,
		_("Click nodes to select them; W moves the selected node."), quarter);
	ImGui::SameLine();
	drawModeButton(LI(ICON_FA_PLUS, "Vehicle"), NODES_ADD_VEHICLE,
		_("Click the ground to add vehicle nodes. Right-click or Esc stops."), quarter);
	ImGui::SameLine();
	drawModeButton(LI(ICON_FA_PLUS, "Ped"), NODES_ADD_PED,
		_("Click the ground to add ped nodes. Right-click or Esc stops."), quarter);
	ImGui::SameLine();
	drawModeButton(LI(ICON_FA_LINK, "Link"), NODES_LINK,
		_("Click one node, then another, to link them.\n"
		"Clicking two linked nodes unlinks them. Right-click or Esc stops."), quarter);
	if(gMode == NODES_ADD_VEHICLE || gMode == NODES_ADD_PED){
		UiCheckbox(L("Link each new node to the previous one"), &gChain);
		showNodeKind(gMode == NODES_ADD_VEHICLE);
	}else if(gMode == NODES_LINK){
		ImGui::TextDisabled("%s", selectedNode ? _("Click the node to link to.") : _("Click the first node."));
		ImGui::SetItemTooltip("%s", _("Vehicle nodes link to vehicle nodes, ped to ped."));
	}

	UiHeading(_("Selected node"));
	drawSelectedNode();
}

static void
drawGotoTask(void)
{
	if(UiInputInt(L("File##goarea"), &gGotoArea))
		gGotoArea = clamp(gGotoArea, 0, 63);
	ImGui::SetItemTooltip("%s", _("Each 750x750 square is one NODES<id>.DAT file."));
	if(UiInputInt(L("Point##gopoint"), &gGotoPoint))
		gGotoPoint = clamp(gGotoPoint, 0, 65535);
	int veh = 0, ped = 0, lnk = 0;
	GetAreaCounts(gGotoArea, &veh, &ped, &lnk);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped(_("NODES%d.DAT: %d vehicle, %d ped nodes (points 0-%d are vehicle)"),
		gGotoArea, veh, ped, veh > 0 ? veh-1 : 0);
	ImGui::PopStyleColor();
	if(ImGui::Button(LI(ICON_FA_LOCATION_DOT, "Go"), ImVec2(-FLT_MIN, 0.0f))){
		Node *node = FindNode(gGotoArea, gGotoPoint);
		if(node){
			jumpToNode(node);
			showNodeKind(IsVehicleNode(node));
			gTask = NODES_TASK_EDIT;	// the node it went to, selected, to edit
		}else
			Toast(TOAST_SELECTION, _("NODES%d.DAT has no point %d"), gGotoArea, gGotoPoint);
	}
	ImGui::SetItemTooltip("%s", _("Select and go to this node."));
}

static void
loadFromFolder(void)
{
	int loaded = 0;
	char err[512];
	if(LoadAreasFromFolder(gFolder, &loaded, err, sizeof(err)))
		Toast(TOAST_SAVE, _("Loaded %d node files"), loaded);
	else
		Toast(TOAST_SAVE, _("Couldn't load the nodes: %s"), err);
}

// Edit open.mp NPC nodes: pick what the viewport shows, add, link, move or
// delete nodes (or go to one by its id), then save the files to a server.
void
DrawNodesWindow(bool *open)
{
	if(open == nil || !*open)
		return;
	readFolderSetting();
	defaultFolderFromWorkspace();
	// ###nodes, not LI(): the ID the window has always had, so saved layouts stay.
	char title[64];
	snprintf(title, sizeof(title), ICON_FA_LOCATION_DOT " %s###nodes", _("Nodes"));
	ImGuiWindowFlags flags = UiPlaceToolWindow(title, 24.0f, true);
	if(!ImGui::Begin(title, open, flags)){
		ImGui::End();
		return;
	}
	if(!*open)
		gMode = NODES_SELECT;
	UiFitItemsToPanel();

	UiBadge(_("open.mp only"), _("open.mp NPCs walk and drive along NODES0-63.DAT with NPC_PlayNode.\n"
		"SA-MP and the game itself don't read the files you save here."));

	if(!isSA() || !NodesLoaded()){
		ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), "%s", _("The game's NODES*.DAT files couldn't be loaded."));
		ImGui::End();
		return;
	}

	// What the viewport shows, whichever job is on.
	UiCheckbox(L("Vehicle nodes"), &gRenderSaCarPaths);
	UiCheckbox(L("Ped nodes"), &gRenderSaPedPaths);
	UiCheckbox(L("Area grid"), &gRenderSaAreaGrid);
	ImGui::SetItemTooltip("%s", _("Each 750x750 square is one NODES<id>.DAT file."));

	ImGui::Spacing();
	const char *tasks[] = { _("Edit"), _("Go to") };
	if(UiSegmented("##nodes_task", tasks, 2, &gTask) && gTask != NODES_TASK_EDIT)
		gMode = NODES_SELECT;	// no adding or linking while the tools are out of sight
	ImGui::Spacing();
	if(gTask == NODES_TASK_EDIT)
		drawEditTask();
	else
		drawGotoTask();

	// The files: where they go, Load and Game nodes, then Save at the foot.
	int dirty = GetDirtyAreaCount();
	UiHeading(_("Files"));
	const char *source = GetFolderSource();
	if(dirty > 0)
		ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("Files changed and not saved: %d"), dirty);
	else
		ImGui::TextDisabled("%s", _("No unsaved changes"));
	char editing[1200];
	snprintf(editing, sizeof(editing), _("Editing: %s"), source ? source : _("a copy of the game's nodes"));
	ImGui::SetItemTooltip("%s%s%s", editing, source ? "\n" : "", source ? _("Files missing from that folder came from the game.") : "");

	const char *browseLabel = L("Browse...");
	ImGui::SetNextItemWidth(MapToolsWidthBeforeButton(browseLabel));
	if(ImGui::InputTextWithHint("##nodesfolder", "server/scriptfiles/NPCs/nodes", gFolder, sizeof(gFolder)))
		writeFolderSetting();
	ImGui::SetItemTooltip("%s", _("Folder for NODES*.DAT (a server's scriptfiles/NPCs/nodes)"));
	ImGui::SameLine();
	if(ImGui::Button(browseLabel) && PickFolderDialog(gFolder, sizeof(gFolder),
	   _("Folder for NODES*.DAT (a server's scriptfiles/NPCs/nodes)"), gFolder))
		writeFolderSetting();
	bool gameFolder = gFolder[0] && IsGameNodesFolder(gFolder);
	if(gameFolder){
		ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(UI_COLOR_WARNING));
		ImGui::TextWrapped("%s", _("That's the game's own data. Pick a server's scriptfiles/NPCs/nodes."));
		ImGui::PopStyleColor();
	}
	bool canWrite = gFolder[0] != '\0' && !gameFolder;

	float half = MapToolsButtonWidth(2);
	ImGui::BeginDisabled(!canWrite);
	if(ImGui::Button(LI(ICON_FA_FOLDER_OPEN, "Load"), ImVec2(half, 0.0f))){
		if(dirty > 0)
			ImGui::OpenPopup(L("Discard node changes?"));
		else
			loadFromFolder();
	}
	ImGui::SetItemTooltip("%s", _("Edit the NODES*.DAT files already in that folder."));
	ImGui::EndDisabled();
	ImGui::SameLine();
	if(ImGui::Button(LI(ICON_FA_ROTATE_LEFT, "Game nodes"), ImVec2(half, 0.0f))){
		if(dirty > 0)
			ImGui::OpenPopup(L("Discard node changes?##game"));
		else
			ReloadGameNodes();
	}
	ImGui::SetItemTooltip("%s", _("Start again from the game's own nodes."));

	if(UiFooterButton(LI(ICON_FA_FLOPPY_DISK, "Save"), canWrite)){
		int saved = 0;
		char err[512];
		if(SaveAreasToFolder(gFolder, &saved, err, sizeof(err)))
			Toast(TOAST_SAVE, _("Saved %d node files to %s"), saved, gFolder);
		else
			Toast(TOAST_SAVE, _("Couldn't save the nodes: %s"), err);
	}
	ImGui::SetItemTooltip("%s", _("Writes all 64 NODES*.DAT files to this folder. open.mp loads them when the\n"
		"server starts."));

	if(ImGui::BeginPopupModal(L("Discard node changes?"), nil, ImGuiWindowFlags_AlwaysAutoResize)){
		ImGui::Text(_("Changed files not saved: %d"), dirty);
		ImGui::Spacing();
		float width = popupButtonWidth(L("Discard and load"), L("Cancel"));
		if(ImGui::Button(L("Discard and load"), ImVec2(width, 0.0f))){
			loadFromFolder();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if(ImGui::Button(L("Cancel"), ImVec2(width, 0.0f)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if(ImGui::BeginPopupModal(L("Discard node changes?##game"), nil, ImGuiWindowFlags_AlwaysAutoResize)){
		ImGui::Text(_("Changed files not saved: %d"), dirty);
		ImGui::Spacing();
		float width = popupButtonWidth(L("Discard"), L("Cancel"));
		if(ImGui::Button(L("Discard"), ImVec2(width, 0.0f))){
			ReloadGameNodes();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if(ImGui::Button(L("Cancel"), ImVec2(width, 0.0f)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	ImGui::End();
}

}
