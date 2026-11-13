// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "i18n.h"
#include "telemetry.h"
#include "workspace.h"
#include "startscreen.h"
#include "icons.h"
#include "version.h"
#include "versionutil.h"
#include "updater.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <algorithm>
#ifdef _WIN32
#include <direct.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

// The start screen: pick a workspace (the server being mapped) and a map in
// it, make a new workspace, or change the game folder. It replaces the
// first-run window and shows on every launch unless "Open the last
// workspace automatically" is on; File > Switch Workspace brings it back.

static bool gOpen;
static bool gOpenRequested;	// open the modal on the next frame
static bool gShownThisRun;	// first run: chosen before loading, so not again after
static std::string gSelected;	// the workspace whose maps are listed
static std::string gSelectedMap;
static char gGameField[1024];
static bool gRestartNeeded;
static bool gLoaded;	// the game is loaded: maps can open
static float gBodyBottom;	// screen y where the two lists end, above the game folder

// What is left of the lists' height from here.
static ImVec2
listSize(void)
{
	return ImVec2(0.0f, std::max(ImGui::GetFrameHeight() * 2.0f, gBodyBottom - ImGui::GetCursorScreenPos().y));
}
static const char *TITLE = N_("Welcome to Haumea Studio");

// ---- When ----

static bool
hasArgument(const char *name)
{
	for(int i = 1; i < sk::args.argc; i++)
		if(strcmp(sk::args.argv[i], name) == 0)
			return true;
	return false;
}

bool
StartScreenWantedAtLaunch(void)
{
	static int wanted = -1;
	if(wanted < 0){
		// Tests, scripts, the agent and a map given on the command line go
		// straight to the editor, as before. HAUMEA_START_SCREEN=1 shows it
		// anyway (its own UI script), 0 never.
		const char *env = getenv("HAUMEA_START_SCREEN");
		if(SettingsAreReadOnly())	// command-line modes (--pawn-roundtrip...), whatever the env
			wanted = 0;
		else if(env && env[0])
			wanted = !gShownThisRun && atoi(env) != 0;
		else
			wanted = !gShownThisRun && !GetOpenLastWorkspace() && !IsAutomatedRun() && !UiScriptActive() &&
				!hasArgument("--agent-socket") && !hasArgument("--open") && !hasArgument("--textdraw-roundtrip");
	}
	return wanted == 1;
}

void
ShowStartScreen(void)
{
	gOpenRequested = true;
}

bool
StartScreenOpen(void)
{
	return gOpen || gOpenRequested;
}

// ---- Files ----

static std::string
baseName(const std::string &path)
{
	size_t end = path.find_last_not_of("/\\");
	if(end == std::string::npos)
		return path;
	size_t slash = path.find_last_of("/\\", end);
	return path.substr(slash == std::string::npos ? 0 : slash + 1, end - (slash == std::string::npos ? 0 : slash + 1) + 1);
}

static bool
isDir(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static long long
modifiedTime(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 ? (long long)st.st_mtime : 0;
}

static bool
makeDir(const std::string &path)
{
#ifdef _WIN32
	return _mkdir(path.c_str()) == 0 || isDir(path);
#else
	return mkdir(path.c_str(), 0755) == 0 || isDir(path);
#endif
}

// "today", "3 days ago", or the date.
static std::string
whenText(long long when)
{
	if(when <= 0)
		return "";
	long long days = ((long long)time(nil) - when) / 86400;
	if(days <= 0) return _("today");
	if(days == 1) return _("yesterday");
	if(days < 30){
		char ago[64];
		snprintf(ago, sizeof(ago), _("%d days ago"), (int)days);
		return ago;
	}
	time_t t = (time_t)when;
	char buf[32];
	strftime(buf, sizeof(buf), "%Y-%m-%d", localtime(&t));
	return buf;
}

void
OpenInFileManager(const std::string &path)
{
#ifdef _WIN32
	ShellExecuteA(nil, "open", path.c_str(), nil, nil, SW_SHOWNORMAL);
#else
	std::string quoted = "'";
	for(char c : path)
		quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
	quoted += "'";
#ifdef __APPLE__
	std::string command = "open " + quoted + " >/dev/null 2>&1 &";
#else
	std::string command = "xdg-open " + quoted + " >/dev/null 2>&1 &";
#endif
	if(system(command.c_str()) != 0)
		log("StartScreen: could not open %s\n", path.c_str());
#endif
}

// Under dir, on a path boundary.
static bool
isInside(const std::string &path, const std::string &dir)
{
	if(dir.empty() || path.size() <= dir.size() || path.compare(0, dir.size(), dir) != 0)
		return false;
	char c = path[dir.size()];
	return c == '/' || c == '\\';
}

// ---- The game folder ----

// SA-MP's client in the game folder: its version string ("0.3.7-R5",
// "0.3.DL-R1"), "" when it has none, or nil-equivalent false without it.
static bool
readSampClientVersion(std::string *version)
{
	version->clear();
	FILE *f = fopen(GamePath("samp.dll").c_str(), "rb");
	if(f == nil)
		return false;
	std::string data;
	char buffer[65536];
	size_t n;
	while((n = fread(buffer, 1, sizeof(buffer), f)) > 0)
		data.append(buffer, n);
	fclose(f);
	// The longest of the "0.3.x" strings the client prints, e.g. "SA-MP 0.3.7-R5 Started".
	for(size_t at = data.find("0.3."); at != std::string::npos; at = data.find("0.3.", at + 4)){
		size_t end = at;
		while(end < data.size() && end - at < 16 && (isalnum((unsigned char)data[end]) || data[end] == '.' || data[end] == '-'))
			end++;
		std::string candidate = data.substr(at, end - at);
		bool known = candidate.compare(0, 5, "0.3.7") == 0 || candidate.compare(0, 6, "0.3.DL") == 0;
		if(known && candidate.size() > version->size())
			*version = candidate;
	}
	return true;
}

// Read once per game folder: samp.dll is a few MB.
static bool
sampClientVersion(std::string *version)
{
	static std::string forGame, cached;
	static bool found;
	if(forGame != GetGameDir()){
		forGame = GetGameDir();
		found = readSampClientVersion(&cached);
	}
	*version = cached;
	return found;
}

// Custom models the workspace's models/artconfig.txt adds.
static int
readCustomModelCount(const std::string &workspace)
{
	if(workspace.empty())
		return 0;
	FILE *f = fopen((workspace + "/models/artconfig.txt").c_str(), "r");
	if(f == nil)
		return 0;
	int count = 0;
	char line[1024];
	while(fgets(line, sizeof(line), f)){
		const char *p = line;
		while(*p == ' ' || *p == '\t') p++;
		if(strncmp(p, "AddSimpleModel", 14) == 0 || strncmp(p, "AddCharModel", 12) == 0)
			count++;
	}
	fclose(f);
	return count;
}

// The same, read again every few seconds rather than every frame.
static int
countCustomModels(const std::string &workspace)
{
	static std::string forWorkspace;
	static int cached;
	static double readAt = -100.0;
	if(forWorkspace == workspace && ImGui::GetTime() - readAt < 3.0)
		return cached;
	forWorkspace = workspace;
	readAt = ImGui::GetTime();
	cached = readCustomModelCount(workspace);
	return cached;
}

static void
gameSection(bool gameLoaded)
{
	if(!gameLoaded)
		ImGui::SeparatorText(TI(ICON_FA_GAMEPAD, "GTA San Andreas"));
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(_("Browse...")).x -
		ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x);
	ImGui::InputText("##game", gGameField, sizeof(gGameField));
	ImGui::SameLine();
	ImGui::PushID("game");
	bool browse = ImGui::Button(_("Browse..."));
	ImGui::PopID();
	if(browse){
		char picked[1024];
		if(PickFolderDialog(picked, sizeof(picked), _("GTA San Andreas folder"), gGameField))
			snprintf(gGameField, sizeof(gGameField), "%s", picked);
	}
	std::string problem;
	bool ok = CheckGameDir(gGameField, &problem);
	bool changed = gameLoaded && ok && strcmp(gGameField, GetGameDir()) != 0;
	if(!ok){
		ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(UI_COLOR_ERROR));
		ImGui::TextWrapped("%s", problem.c_str());
		ImGui::PopStyleColor();
	}
	else if(changed){
		// Picking a folder changes nothing until it is confirmed here.
		ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), TI(ICON_FA_CIRCLE_INFO,
			"Not applied yet: confirm to use this folder (Haumea restarts to load it)."));
		ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x, accent.y, accent.z, 0.45f));
		if(ImGui::Button(TI(ICON_FA_CHECK, "Confirm and use this game folder"))){
			SetGameDir(gGameField, nil);
			gRestartNeeded = true;
		}
		ImGui::PopStyleColor();
		ImGui::SameLine();
		if(ImGui::Button(_("Cancel")))
			snprintf(gGameField, sizeof(gGameField), "%s", GetGameDir());
	}else if(gameLoaded){
		// What the editor found in the folder it loaded.
		std::string version;
		bool hasClient = sampClientVersion(&version);
		if(hasClient)
			ImGui::TextColored(UiSemanticColor(UI_COLOR_SUCCESS), TI(ICON_FA_CHECK, "SA-MP %s found"),
				version.empty() ? _("client") : version.c_str());
		else{
			ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("SA-MP client not found"));
			ImGui::SetItemTooltip(_("SA-MP's own objects won't show without its client files."));
		}
		if(hasClient && version.empty()){
			ImGui::SameLine();
			ImGui::TextDisabled(_("(version unknown)"));
		}
		ImGui::SameLine();
		int custom = countCustomModels(gSelected);
		ImGui::TextDisabled(_("|  Custom models: %d"), custom);
		ImGui::SetItemTooltip(_("From the workspace's models/artconfig.txt."));
	}else
		ImGui::TextColored(UiSemanticColor(UI_COLOR_SUCCESS), TI(ICON_FA_CHECK, "GTA San Andreas found."));
	if(gRestartNeeded){
		ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("The new game folder loads when Haumea restarts."));
		ImGui::SameLine();
		if(ImGui::SmallButton(_("Restart now")))
			RestartHaumea();
	}
}

// ---- Workspaces ----

static bool gNewWorkspace;
static char gNewParent[1024], gNewName[128];

static void
newWorkspaceForm(void)
{
	ImGui::TextDisabled(_("Create a workspace folder."));
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(_("Browse...")).x -
		ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x);
	ImGui::InputTextWithHint("##parent", _("In this folder"), gNewParent, sizeof(gNewParent));
	ImGui::SameLine();
	ImGui::PushID("parent");
	bool browse = ImGui::Button(_("Browse..."));
	ImGui::PopID();
	if(browse){
		char picked[1024];
		if(PickFolderDialog(picked, sizeof(picked), _("Where the workspace goes"), gNewParent))
			snprintf(gNewParent, sizeof(gNewParent), "%s", picked);
	}
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputTextWithHint("##name", _("Name, e.g. my-server"), gNewName, sizeof(gNewName));
	bool nameOk = gNewName[0] && strpbrk(gNewName, "/\\:*?\"<>|") == nil && strcmp(gNewName, ".") && strcmp(gNewName, "..");
	bool parentOk = isDir(gNewParent);
	std::string path = std::string(gNewParent) + "/" + gNewName;
	if(gNewName[0] && !nameOk)
		ImGui::TextColored(UiSemanticColor(UI_COLOR_ERROR), _("A name can't have / \\ : * ? \" < > |"));
	else if(gNewParent[0] && !parentOk)
		ImGui::TextColored(UiSemanticColor(UI_COLOR_ERROR), _("That folder doesn't exist."));
	else if(nameOk && parentOk && isDir(path))
		ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("%s exists: it is used as it is."), gNewName);
	ImGui::BeginDisabled(!nameOk || !parentOk);
	if(ImGui::Button(_("Create workspace"))){
		bool made = makeDir(path);
		static const char *folders[] = { "gamemodes", "filterscripts", "scriptfiles" };
		for(const char *folder : folders)
			made = made && makeDir(path + "/" + folder);
		if(made){
			SetWorkspaceDir(path.c_str());
			gSelected = GetWorkspaceDir();
			gNewWorkspace = false;
			gNewName[0] = '\0';
		}else
			Toast(TOAST_SAVE, _("Could not create %s"), path.c_str());
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::PushID("new");
	if(ImGui::Button(_("Cancel")))
		gNewWorkspace = false;
	ImGui::PopID();
}

static void
workspaceList(void)
{
	float width = ImGui::GetContentRegionAvail().x;
	if(ImGui::Button(LI(ICON_FA_PLUS, "New workspace"), ImVec2(width, 0)))
		gNewWorkspace = !gNewWorkspace;
	if(ImGui::Button(LI(ICON_FA_FOLDER_OPEN, "Open folder..."), ImVec2(width, 0))){
		char picked[1024];
		if(PickFolderDialog(picked, sizeof(picked), _("Server folder"), gSelected.c_str())){
			SetWorkspaceDir(picked);
			gSelected = GetWorkspaceDir();
		}
	}
	if(gNewWorkspace) newWorkspaceForm();
	ImGui::Spacing();
	ImGui::TextDisabled(_("Recent workspaces"));
	ImGui::BeginChild("workspaces", listSize());
	std::vector<RecentWorkspace> recent = GetRecentWorkspaceList();
	if(recent.empty()) ImGui::TextWrapped(_("Choose or create a workspace."));
	for(const RecentWorkspace &w : recent){
		ImGui::PushID(w.path.c_str());
		bool missing = !isDir(w.path);
		float rowHeight = ImGui::GetFontSize()*4.0f;
		float right = ImGui::GetContentRegionAvail().x;
		ImVec2 at = ImGui::GetCursorScreenPos();
		bool selected = gSelected == w.path;
		if(ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick |
			ImGuiSelectableFlags_AllowOverlap, ImVec2(right, rowHeight))){
			gSelected = w.path;
			gSelectedMap.clear();
			if(ImGui::IsMouseDoubleClicked(0) && !missing && gLoaded){
				SetWorkspaceDir(w.path.c_str()); gOpen = false;
			}
		}
		ImGui::SetItemTooltip(_("%s\nDouble-click to open it."), w.path.c_str());
		ImDrawList *draw = ImGui::GetWindowDrawList();
		float pad = ImGui::GetFontSize()*0.6f;
		float button = ImGui::GetFrameHeight();
		ImVec2 textMax(at.x + right - button*2.0f - pad, at.y + rowHeight);
		draw->PushClipRect(at, textMax, true);
		draw->AddText(ImVec2(at.x + pad, at.y + pad), ImGui::GetColorU32(ImGuiCol_Text), baseName(w.path).c_str());
		std::string detail = missing ? _("Folder missing") : whenText(w.lastOpened);
		draw->AddText(ImVec2(at.x + pad, at.y + pad + ImGui::GetTextLineHeightWithSpacing()),
			ImGui::GetColorU32(missing ? ImGuiCol_PlotHistogram : ImGuiCol_TextDisabled), detail.c_str());
		draw->PopClipRect();
		ImVec2 after = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(at.x + right - button*2.0f, at.y + pad));
		if(ImGui::Button(w.pinned ? ICON_FA_STAR "##unpin" : ICON_FA_STAR "##pin", ImVec2(button, button)))
			PinRecentWorkspace(w.path.c_str(), !w.pinned);
		ImGui::SetItemTooltip(w.pinned ? _("Unpin") : _("Pin: keep it at the top of the list"));
		ImGui::SameLine(0, 0);
		if(ImGui::Button("...##workspace_actions", ImVec2(button, button)))
			ImGui::OpenPopup("workspace_actions");
		if(ImGui::BeginPopup("workspace_actions")){
			ImGui::BeginDisabled(missing);
			if(ImGui::MenuItem(_("Open the folder"))) OpenInFileManager(w.path);
			ImGui::EndDisabled();
			if(ImGui::MenuItem(_("Remove from recent"))){
				RemoveRecentWorkspace(w.path.c_str());
				if(gSelected == w.path) gSelected.clear();
			}
			ImGui::EndPopup();
		}
		ImGui::SetCursorScreenPos(after);
		ImGui::Dummy(ImVec2(0, 0));
		ImGui::PopID();
	}
	ImGui::EndChild();
}

// ---- Maps ----

static void
openSelected(const std::string &map)
{
	if(!gSelected.empty() && gSelected != GetWorkspaceDir())
		SetWorkspaceDir(gSelected.c_str());
	if(!map.empty())
		HandleMapFileDrop(map.c_str());
	gOpen = false;
}

static void
mapList(bool gameLoaded)
{
	std::vector<std::string> maps;
	for(const std::string &path : GetRecentMapPaths())
		if(isInside(path, gSelected)) maps.push_back(path);
	if(maps.empty()){
		ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()*3.0f));
		ImGui::TextDisabled(ICON_FA_FILE "  %s", _("No recent maps"));
		ImGui::TextWrapped("%s", gSelected.empty() ? _("Choose a workspace to get started.") :
			_("Open a map in this workspace and it will appear here."));
		return;
	}
	if(!gameLoaded){
		ImGui::TextWrapped(_("Maps open once the game folder is loaded."));
		return;
	}
	float gap = ImGui::GetStyle().ItemSpacing.x;
	float avail = ImGui::GetContentRegionAvail().x;
	int columns = std::max(1, (int)((avail + gap)/(ImGui::GetFontSize()*13.0f + gap)));
	float width = (avail - gap*(columns-1))/columns;
	float imageHeight = ImGui::GetFontSize()*8.0f;
	float height = imageHeight + ImGui::GetFontSize()*4.0f;
	float pad = ImGui::GetFontSize()*0.7f;
	for(size_t i = 0; i < maps.size(); i++){
		const std::string &path = maps[i];
		ImGui::PushID(path.c_str());
		if(i % columns) ImGui::SameLine(0, gap);
		ImVec2 at = ImGui::GetCursorScreenPos();
		std::string name = baseName(path);
		bool selected = gSelectedMap == path;
		if(ImGui::InvisibleButton(("##" + name).c_str(), ImVec2(width, height), ImGuiButtonFlags_EnableNav)){
			gSelectedMap = path;
		}
		if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) openSelected(path);
		bool hovered = ImGui::IsItemHovered();
		ImGui::SetItemTooltip(_("%s\nDouble-click to open it."), path.c_str());
		ImDrawList *draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), ImGui::GetColorU32(
			selected ? ImGuiCol_HeaderActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 8.0f*GetUiScale());
		rw::Texture *thumb = GetPrefabThumbnailTexture(path.c_str());
		if(thumb){
			float side = imageHeight - pad;
			float left = at.x + (width-side)*0.5f;
			draw->AddImage((ImTextureID)thumb, ImVec2(left, at.y + pad), ImVec2(left+side, at.y + pad+side));
		}else{
			ImVec2 icon = ImGui::CalcTextSize(ICON_FA_FILE);
			draw->AddText(ImVec2(at.x + (width-icon.x)*0.5f, at.y + imageHeight*0.5f),
				ImGui::GetColorU32(ImGuiCol_TextDisabled), ICON_FA_FILE);
		}
		draw->PushClipRect(ImVec2(at.x+pad, at.y+imageHeight), ImVec2(at.x+width-pad, at.y+height), true);
		draw->AddText(ImVec2(at.x+pad, at.y+imageHeight+pad), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
		draw->AddText(ImVec2(at.x+pad, at.y+imageHeight+pad+ImGui::GetTextLineHeightWithSpacing()),
			ImGui::GetColorU32(ImGuiCol_TextDisabled), whenText(modifiedTime(path)).c_str());
		draw->PopClipRect();
		if(selected) draw->AddRectFilled(ImVec2(at.x+pad, at.y+height-2*GetUiScale()),
			ImVec2(at.x+width-pad, at.y+height), ImGui::GetColorU32(ImGuiCol_CheckMark));
		ImGui::PopID();
	}
}

// ---- The screen ----

static void
contents(bool gameLoaded)
{
	gLoaded = gameLoaded;
	if(gSelected.empty() && !GetRecentWorkspaceList().empty())
		gSelected = GetWorkspaceDir()[0] ? GetWorkspaceDir() : GetRecentWorkspaceList()[0].path;
	ImGui::SetWindowFontScale(1.6f);
	ImGui::TextUnformatted("Haumea Studio");
	ImGui::SetWindowFontScale(1.0f);
	if(gameLoaded){
		ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFontSize()*4.5f);
		if(ImGui::Button(_("Skip"), ImVec2(ImGui::GetFontSize()*4.5f, 0))) gOpen = false;
		ImGui::SetItemTooltip(_("Return to the editor."));
	}
	ImGui::TextDisabled(_("Pick up where you left off."));
	ImGui::Spacing();
	if(UpdaterHasNews()){
		UpdaterDrawInline();
		if(UpdaterWhatsNewRequested()){
			ImGui::BeginChild("whatsnew", ImVec2(0, ImGui::GetFontSize()*6));
			UpdaterDrawWhatsNewInline(); ImGui::EndChild();
		}
	}
	TelemetryDrawConsent();
	float footer = ImGui::GetFrameHeightWithSpacing()*(gameLoaded ? 2.0f : 6.0f) + ImGui::GetStyle().ItemSpacing.y*3.0f;
	float bodyHeight = std::max(ImGui::GetFontSize()*8, ImGui::GetContentRegionAvail().y-footer);
	gBodyBottom = ImGui::GetCursorScreenPos().y + bodyHeight - ImGui::GetStyle().WindowPadding.y;
	if(ImGui::BeginTable("start", 2, ImGuiTableFlags_None, ImVec2(0, bodyHeight))){
		ImGui::TableSetupColumn("Workspaces", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize()*17.0f);
		ImGui::TableSetupColumn("Maps", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextColumn();
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
		ImGui::BeginChild("workspace_panel", ImVec2(0, bodyHeight), ImGuiChildFlags_AlwaysUseWindowPadding);
		workspaceList();
		ImGui::EndChild(); ImGui::PopStyleColor();
		ImGui::TableNextColumn();
		ImGui::Indent(ImGui::GetFontSize());
		ImGui::TextUnformatted(gSelected.empty() ? _("Your maps") : baseName(gSelected).c_str());
		ImGui::TextDisabled(_("Recent maps"));
		ImGui::Spacing();
		ImGui::BeginChild("maps", listSize()); mapList(gameLoaded); ImGui::EndChild();
		ImGui::Unindent(ImGui::GetFontSize());
		ImGui::EndTable();
	}
	if(gameLoaded){
		if(ImGui::Button(LI(ICON_FA_GAMEPAD, "Game folder"))) ImGui::OpenPopup("game_folder");
		ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize()*34, 0), ImGuiCond_Appearing);
		if(ImGui::BeginPopup("game_folder")){ gameSection(true); ImGui::EndPopup(); }
	}else gameSection(false);
	bool openLast = GetOpenLastWorkspace();
	if(gameLoaded) ImGui::SameLine();
	if(ImGui::Checkbox(_("Open last workspace at startup"), &openLast)) SetOpenLastWorkspace(openLast);
	ImGui::Spacing();
	ImGui::TextDisabled("%s", HaumeaFullVersion());
	ImGui::SetItemTooltip("%s", HaumeaVersionDisplay());
	std::string problem;
	bool gameOk = gameLoaded || CheckGameDir(gGameField, &problem);
	const char *open = !gameLoaded || gSelected.empty() ? _("Continue") :
		gSelectedMap.empty() ? _("Open workspace") : _("Open map");
	float primaryWidth = ImGui::GetFontSize()*10.0f;
	float secondaryWidth = ImGui::CalcTextSize(_("Continue without workspace")).x + ImGui::GetStyle().FramePadding.x*2;
	ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x-primaryWidth-secondaryWidth-ImGui::GetStyle().ItemSpacing.x));
	ImGui::BeginDisabled(!gameOk);
	if(ImGui::Button(_("Continue without workspace"), ImVec2(secondaryWidth, 0))){
		gSelected.clear();
		if(!gameLoaded) FinishWorkspaceSetup(gGameField, ""); else SetWorkspaceDir(nil);
		gOpen = false;
	}
	ImGui::SameLine();
	ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x, accent.y, accent.z, 0.30f));
	if(ImGui::Button(open, ImVec2(primaryWidth, 0))){
		if(!gameLoaded) FinishWorkspaceSetup(gGameField, gSelected.c_str());
		openSelected(gameLoaded ? gSelectedMap : std::string());
	}
	ImGui::PopStyleColor(); ImGui::EndDisabled();
}

void
DrawStartScreen(bool gameLoaded)
{
	static bool filled;
	if(!filled){
		filled = true;
		snprintf(gGameField, sizeof(gGameField), "%s", GetSuggestedGameDir());
	}
	ImGuiIO &io = ImGui::GetIO();
	ImVec2 size(std::min(io.DisplaySize.x - 40.0f, ImGui::GetFontSize() * 66.0f),
		std::min(io.DisplaySize.y - 40.0f, ImGui::GetFontSize() * 41.0f));
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar;
	if(!gameLoaded){
		// First run, on the splash before loading: the game folder is needed.
		gShownThisRun = true;
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(size, ImGuiCond_Always);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24*GetUiScale(), 24*GetUiScale()));
		ImGui::Begin(L(TITLE), nil, flags);
		contents(false);
		ImGui::End();
		ImGui::PopStyleVar();
		return;
	}
	if(gOpenRequested){
		gOpenRequested = false;
		gOpen = true;
		gShownThisRun = true;
		gSelected = GetWorkspaceDir();
		gSelectedMap.clear();
		ImGui::OpenPopup(L(TITLE));
	}
	if(!gOpen){
		TelemetryStartScreenClosed();
		return;
	}
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(size, ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24*GetUiScale(), 24*GetUiScale()));
	if(!ImGui::BeginPopupModal(L(TITLE), nil, flags)){
		ImGui::PopStyleVar();
		gOpen = false;
		return;
	}
	contents(true);
	if(!gOpen || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive())){
		gOpen = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
	ImGui::PopStyleVar();
}
