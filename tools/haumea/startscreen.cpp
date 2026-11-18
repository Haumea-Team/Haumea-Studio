// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "i18n.h"
#include "telemetry.h"
#include "workspace.h"
#include "startscreen.h"
#include "serverdownload.h"
#include "icons.h"
#include "lucide_icons.h"
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

// Custom models in the workspace's models folder: Haumea's list of them and
// a SA-MP style artconfig.txt.
static int
readCustomModelCount(const std::string &workspace)
{
	if(workspace.empty())
		return 0;
	int count = 0;
	for(const char *name : { "/models/haumea_models.txt", "/models/artconfig.txt" }){
		FILE *f = fopen((workspace + name).c_str(), "r");
		if(f == nil)
			continue;
		char line[1024];
		while(fgets(line, sizeof(line), f)){
			const char *p = line;
			while(*p == ' ' || *p == '\t') p++;
			if(strncmp(p, "AddSimpleModel", 14) == 0 || strncmp(p, "AddCharModel", 12) == 0)
				count++;
		}
		fclose(f);
	}
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
		UiHeading(TI(ICON_FA_GAMEPAD, "GTA San Andreas"));
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
		ImGui::SetItemTooltip(_("In the workspace's models folder."));
	}else
		ImGui::TextColored(UiSemanticColor(UI_COLOR_SUCCESS), TI(ICON_FA_CHECK, "GTA San Andreas found."));
	if(gRestartNeeded){
		ImGui::TextColored(UiSemanticColor(UI_COLOR_WARNING), _("The new game folder loads when Haumea restarts."));
		ImGui::SameLine();
		if(ImGui::SmallButton(_("Restart now")))
			RestartHaumea();
	}
}

// ---- Looks: the ribbon's flat style ----

// The theme's colour for a row's state: hovered, held, selected.
static ImU32
rowFill(bool hovered, bool held, bool selected)
{
	if(held) return ImGui::GetColorU32(ImGuiCol_HeaderActive);
	if(selected) return ImGui::GetColorU32(ImGuiCol_Header);
	if(hovered) return ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.6f);
	return 0;
}

static void
lucideIcon(ImDrawList *draw, const char *icon, float size, ImVec2 centre, ImU32 col)
{
	ImFont *font = gIconFont ? gIconFont : ImGui::GetFont();
	ImVec2 s = font->CalcTextSizeA(size, FLT_MAX, 0.0f, icon);
	draw->AddText(font, size, ImVec2(floorf(centre.x - s.x*0.5f), floorf(centre.y - s.y*0.5f)), col, icon);
}

// A section's title in small capitals.
static void
capsTitle(const char *text)
{
	std::string caps = text;
	for(char &c : caps)
		if((unsigned char)c < 128)
			c = (char)toupper((unsigned char)c);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::SetWindowFontScale(0.82f);
	ImGui::TextUnformatted(caps.c_str());
	ImGui::SetWindowFontScale(1.0f);
	ImGui::PopStyleColor();
}

// A flat row with an icon and a name: hover lights it.
static bool
actionRow(const char *id, const char *icon, const char *label, float width)
{
	float fs = ImGui::GetFontSize(), h = floorf(fs*2.0f);
	ImVec2 at = ImGui::GetCursorScreenPos();
	// The name it shows is its label too (UI scripts click it by name); the id keeps it unique.
	std::string button = std::string(label) + "#" + id;
	bool pressed = ImGui::InvisibleButton(button.c_str(), ImVec2(width, h));
	bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
	ImDrawList *draw = ImGui::GetWindowDrawList();
	if(ImU32 bg = rowFill(hovered, held, false))
		draw->AddRectFilled(at, ImVec2(at.x + width, at.y + h), bg, 6.0f*GetUiScale());
	lucideIcon(draw, icon, fs*1.1f, ImVec2(at.x + fs*1.2f, at.y + h*0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled));
	draw->AddText(ImVec2(at.x + fs*2.4f, at.y + (h - fs)*0.5f), ImGui::GetColorU32(ImGuiCol_Text, 0.85f), label);
	return pressed;
}

// The main action of the sidebar: its icon in an accent circle.
static bool
accentRow(const char *id, const char *icon, const char *label, float width)
{
	float fs = ImGui::GetFontSize(), h = floorf(fs*2.2f);
	ImVec2 at = ImGui::GetCursorScreenPos();
	// The name it shows is its label too (UI scripts click it by name); the id keeps it unique.
	std::string button = std::string(label) + "#" + id;
	bool pressed = ImGui::InvisibleButton(button.c_str(), ImVec2(width, h));
	bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
	ImDrawList *draw = ImGui::GetWindowDrawList();
	if(ImU32 bg = rowFill(hovered, held, false))
		draw->AddRectFilled(at, ImVec2(at.x + width, at.y + h), bg, 6.0f*GetUiScale());
	ImVec2 c(at.x + fs*1.2f, at.y + h*0.5f);
	draw->AddCircleFilled(c, fs*0.72f, ImGui::GetColorU32(UiAccentFill()));
	lucideIcon(draw, LU_PLUS, fs*0.95f, c, IM_COL32_WHITE);
	(void)icon;
	draw->AddText(ImVec2(at.x + fs*2.4f, at.y + (h - fs)*0.5f), ImGui::GetColorU32(ImGuiCol_Text), label);
	return pressed;
}

// Buttons: a flat outlined one, and the solid accent one for the main action.
static bool
flatButton(const char *label, ImVec2 size)
{
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
	ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_Border));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f*GetUiScale());
	bool pressed = ImGui::Button(label, size);
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(4);
	return pressed;
}

static bool
primaryButton(const char *label, ImVec2 size)
{
	ImVec4 fill = UiAccentFill();
	ImGui::PushStyleColor(ImGuiCol_Button, fill);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(fill.x*1.12f, fill.y*1.12f, fill.z*1.12f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(fill.x*0.9f, fill.y*0.9f, fill.z*0.9f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f*GetUiScale());
	bool pressed = ImGui::Button(label, size);
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(4);
	return pressed;
}

// ---- Workspaces ----

static bool gNewWorkspace;
static char gNewParent[1024], gNewName[128];
static int gNewDownloadServer = -1;	// on, except under UI scripts (no network there)

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
	bool downloading = ServerDownloadStatus(nil, nil, nil) == SERVER_DOWNLOAD_RUNNING;
	// A folder that has a server (open.mp or SA-MP) keeps it: nothing to fetch.
	bool hasServer = nameOk && parentOk && ServerDownloadHasServer(path.c_str());
	ImGui::BeginDisabled(downloading || hasServer);
	if(gNewDownloadServer < 0)
		gNewDownloadServer = !UiScriptActive();
	bool download = gNewDownloadServer != 0 && !hasServer;
	if(UiCheckbox(_("Download the open.mp server into it"), &download))
		gNewDownloadServer = download;
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("%s", hasServer ? _("It already has a server.") :
		_("The latest open.mp release from GitHub, so Test in game and pawncc work in it.\n"
		"Files already in the folder are kept."));
	ImGui::BeginDisabled(!nameOk || !parentOk);
	if(ImGui::Button(_("Create workspace"))){
		bool made = makeDir(path);
		static const char *folders[] = { "gamemodes", "filterscripts", "scriptfiles" };
		for(const char *folder : folders)
			made = made && makeDir(path + "/" + folder);
		if(made){
			if(gNewDownloadServer > 0 && !downloading && !hasServer)
				ServerDownloadStart(path.c_str());
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
workspaceList(float height)
{
	ImGui::BeginChild("workspaces", ImVec2(0, height));
	float megabytes = 0.0f;
	if(ServerDownloadStatus(nil, nil, &megabytes) == SERVER_DOWNLOAD_RUNNING)
		ImGui::TextDisabled(_("Downloading the open.mp server: %.1f MB"), megabytes);
	std::vector<RecentWorkspace> recent = GetRecentWorkspaceList();
	if(recent.empty()) ImGui::TextWrapped(_("Choose or create a workspace."));
	float fs = ImGui::GetFontSize(), radius = 6.0f*GetUiScale();
	for(const RecentWorkspace &w : recent){
		ImGui::PushID(w.path.c_str());
		bool missing = !isDir(w.path);
		float rowHeight = floorf(fs*2.2f);
		float right = ImGui::GetContentRegionAvail().x;
		ImVec2 at = ImGui::GetCursorScreenPos();
		bool selected = gSelected == w.path;
		ImGui::SetNextItemAllowOverlap();
		if(ImGui::InvisibleButton("##row", ImVec2(right, rowHeight))){
			gSelected = w.path;
			gSelectedMap.clear();
		}
		bool held = ImGui::IsItemActive();
		if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !missing && gLoaded){
			SetWorkspaceDir(w.path.c_str()); gOpen = false;
		}
		bool rowHovered = ImGui::IsMouseHoveringRect(at, ImVec2(at.x + right, at.y + rowHeight));
		ImGui::SetItemTooltip(_("%s\nDouble-click to open it."), w.path.c_str());
		ImDrawList *draw = ImGui::GetWindowDrawList();
		if(ImU32 bg = rowFill(rowHovered, held, selected))
			draw->AddRectFilled(at, ImVec2(at.x + right, at.y + rowHeight), bg, radius);
		ImU32 ink = ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_Text, selected ? 1.0f : 0.8f);
		lucideIcon(draw, LU_FOLDER, fs*1.05f, ImVec2(at.x + fs*1.2f, at.y + rowHeight*0.5f),
			missing ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : selected ? ImGui::GetColorU32(ImGuiCol_CheckMark) : ImGui::GetColorU32(ImGuiCol_TextDisabled));
		float button = floorf(fs*1.6f);
		bool tools = rowHovered || selected;
		float textRight = at.x + right - (tools ? button*2.0f + fs*0.3f : fs*0.5f);
		draw->PushClipRect(at, ImVec2(textRight, at.y + rowHeight), true);
		draw->AddText(ImVec2(at.x + fs*2.4f, at.y + (rowHeight - fs)*0.5f), ink, baseName(w.path).c_str());
		draw->PopClipRect();
		ImVec2 after = ImGui::GetCursorScreenPos();
		if(tools || w.pinned){
			// Pin, and the row's menu: on the row under the mouse or the chosen one.
			ImVec2 b0(at.x + right - button*2.0f - fs*0.2f, at.y + (rowHeight - button)*0.5f);
			ImGui::SetCursorScreenPos(b0);
			// ##unpin while pinned: the state is in its ID, as before (UI scripts check it).
			if(ImGui::InvisibleButton(w.pinned ? "##unpin" : "##pin", ImVec2(button, button)))
				PinRecentWorkspace(w.path.c_str(), !w.pinned);
			bool pinHover = ImGui::IsItemHovered();
			ImGui::SetItemTooltip("%s", w.pinned ? _("Unpin") : _("Pin: keep it at the top of the list"));
			if(pinHover)
				draw->AddRectFilled(b0, ImVec2(b0.x + button, b0.y + button), ImGui::GetColorU32(ImGuiCol_HeaderHovered), radius);
			lucideIcon(draw, LU_STAR, fs*0.9f, ImVec2(b0.x + button*0.5f, b0.y + button*0.5f),
				w.pinned ? ImGui::GetColorU32(UiSemanticColor(UI_COLOR_WARNING)) : ImGui::GetColorU32(ImGuiCol_TextDisabled));
			if(tools){
				ImVec2 b1(b0.x + button, b0.y);
				ImGui::SetCursorScreenPos(b1);
				if(ImGui::InvisibleButton("##more", ImVec2(button, button)))
					ImGui::OpenPopup("workspace_actions");
				if(ImGui::IsItemHovered())
					draw->AddRectFilled(b1, ImVec2(b1.x + button, b1.y + button), ImGui::GetColorU32(ImGuiCol_HeaderHovered), radius);
				lucideIcon(draw, LU_ELLIPSIS, fs*0.9f, ImVec2(b1.x + button*0.5f, b1.y + button*0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled));
			}
		}
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
		ImGui::SetCursorScreenPos(ImVec2(after.x, after.y + 2.0f));
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
	float fs = ImGui::GetFontSize();
	if(maps.empty()){
		ImGui::Dummy(ImVec2(0, fs*2.0f));
		ImGui::TextDisabled("%s", _("No recent maps"));
		ImGui::TextWrapped("%s", gSelected.empty() ? _("Choose a workspace to get started.") :
			_("Open a map in this workspace and it will appear here."));
		return;
	}
	if(!gameLoaded){
		ImGui::TextWrapped(_("Maps open once the game folder is loaded."));
		return;
	}
	// Cards in rows: the map's picture on the left, its name and age beside it.
	float gap = fs*0.7f;
	float avail = ImGui::GetContentRegionAvail().x;
	int columns = std::max(1, (int)((avail + gap)/(fs*17.0f + gap)));
	float width = floorf((avail - gap*(columns-1))/columns);
	float height = floorf(fs*4.6f), pad = floorf(fs*0.6f), radius = 7.0f*GetUiScale();
	for(size_t i = 0; i < maps.size(); i++){
		const std::string &path = maps[i];
		ImGui::PushID(path.c_str());
		if(i % columns) ImGui::SameLine(0, gap);
		else if(i) ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
		ImVec2 at = ImGui::GetCursorScreenPos();
		std::string name = baseName(path);
		bool selected = gSelectedMap == path;
		if(ImGui::InvisibleButton(("##" + name).c_str(), ImVec2(width, height), ImGuiButtonFlags_EnableNav))
			gSelectedMap = path;
		if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) openSelected(path);
		bool hovered = ImGui::IsItemHovered();
		ImGui::SetItemTooltip(_("%s\nDouble-click to open it."), path.c_str());
		ImDrawList *draw = ImGui::GetWindowDrawList();
		ImVec2 lo = at, hi(at.x + width, at.y + height);
		draw->AddRectFilled(lo, hi, ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), radius);
		draw->AddRect(lo, hi, ImGui::GetColorU32(selected ? ImGuiCol_CheckMark : ImGuiCol_Border), radius, 0,
			selected ? 1.5f*GetUiScale() : 1.0f);
		// The picture: the map's thumbnail, or a map icon on the darker ground.
		float side = height - pad*2.0f;
		ImVec2 t0(lo.x + pad, lo.y + pad), t1(t0.x + side, t0.y + side);
		draw->AddRectFilled(t0, t1, ImGui::GetColorU32(ImGuiCol_TitleBg), radius - 2.0f);
		if(rw::Texture *thumb = GetPrefabThumbnailTexture(path.c_str()))
			draw->AddImageRounded((ImTextureID)thumb, t0, t1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, radius - 2.0f);
		else
			lucideIcon(draw, LU_MAP, fs*1.5f, ImVec2((t0.x + t1.x)*0.5f, (t0.y + t1.y)*0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled));
		float textX = t1.x + pad*1.4f;
		float lines = fs + ImGui::GetTextLineHeightWithSpacing();
		float top = lo.y + (height - lines)*0.5f;
		draw->PushClipRect(ImVec2(textX, lo.y), ImVec2(hi.x - pad, hi.y), true);
		draw->AddText(ImVec2(textX, top), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
		std::string when = whenText(modifiedTime(path));
		draw->AddText(ImGui::GetFont(), fs*0.88f, ImVec2(textX, top + ImGui::GetTextLineHeightWithSpacing()),
			ImGui::GetColorU32(ImGuiCol_TextDisabled), when.empty() ? _("File missing") : when.c_str());
		draw->PopClipRect();
		ImGui::PopID();
	}
}

// ---- The screen ----

// A card: a bordered panel for a form or a notice in the main area.
static void
beginCard(const char *id)
{
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f*GetUiScale());
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetFontSize()*1.1f, ImGui::GetFontSize()*0.9f));
	ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
}

static void
endCard(void)
{
	ImGui::EndChild();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
}

static void
contents(bool gameLoaded)
{
	gLoaded = gameLoaded;
	if(gSelected.empty() && !GetRecentWorkspaceList().empty())
		gSelected = GetWorkspaceDir()[0] ? GetWorkspaceDir() : GetRecentWorkspaceList()[0].path;
	float fs = ImGui::GetFontSize();
	ImVec2 avail = ImGui::GetContentRegionAvail();

	// The sidebar: what to do, the workspaces, the game.
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_TitleBg));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs*0.8f, fs*1.2f));
	ImGui::BeginChild("sidebar", ImVec2(floorf(fs*16.0f), avail.y), ImGuiChildFlags_AlwaysUseWindowPadding,
		ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleVar();
	ImGui::Indent(fs*0.4f);
	ImGui::SetWindowFontScale(1.15f);
	ImGui::TextUnformatted("Haumea Studio");
	ImGui::SetWindowFontScale(1.0f);
	ImGui::TextDisabled("%s", HaumeaFullVersion());
	ImGui::SetItemTooltip("%s", HaumeaVersionDisplay());
	ImGui::Unindent(fs*0.4f);
	ImGui::Dummy(ImVec2(0, fs*0.8f));
	float width = ImGui::GetContentRegionAvail().x;
	if(accentRow("##new_workspace", LU_FOLDER_PLUS, _("New workspace"), width))
		gNewWorkspace = !gNewWorkspace;
	if(actionRow("##open_folder", LU_FOLDER_OPEN, _("Open folder..."), width)){
		char picked[1024];
		if(PickFolderDialog(picked, sizeof(picked), _("Server folder"), gSelected.c_str())){
			SetWorkspaceDir(picked);
			gSelected = GetWorkspaceDir();
		}
	}
	ImGui::Dummy(ImVec2(0, fs*0.6f));
	ImGui::Indent(fs*0.4f);
	capsTitle(_("Recent workspaces"));
	ImGui::Unindent(fs*0.4f);
	float bottom = gameLoaded ? fs*6.6f : fs*2.4f;
	workspaceList(std::max(fs*4.0f, ImGui::GetContentRegionAvail().y - bottom));
	// The game the editor loaded, as a small card, and the startup choice.
	if(gameLoaded){
		ImVec2 at = ImGui::GetCursorScreenPos();
		float cw = ImGui::GetContentRegionAvail().x, ch = floorf(fs*3.4f);
		if(ImGui::InvisibleButton("##game_card", ImVec2(cw, ch)))
			ImGui::OpenPopup("game_folder");
		bool hovered = ImGui::IsItemHovered();
		ImGui::SetItemTooltip("%s", GetGameDir());
		ImDrawList *draw = ImGui::GetWindowDrawList();
		float radius = 7.0f*GetUiScale();
		draw->AddRectFilled(at, ImVec2(at.x + cw, at.y + ch), ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), radius);
		draw->AddRect(at, ImVec2(at.x + cw, at.y + ch), ImGui::GetColorU32(ImGuiCol_Border), radius);
		lucideIcon(draw, LU_GAMEPAD_2, fs*1.1f, ImVec2(at.x + fs*1.3f, at.y + ch*0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled));
		draw->AddText(ImVec2(at.x + fs*2.5f, at.y + ch*0.5f - fs), ImGui::GetColorU32(ImGuiCol_Text), "GTA San Andreas");
		std::string version;
		bool client = sampClientVersion(&version);
		char line[96];
		if(client) snprintf(line, sizeof(line), _("SA-MP %s found"), version.empty() ? _("client") : version.c_str());
		else snprintf(line, sizeof(line), "%s", _("SA-MP client not found"));
		draw->AddText(ImGui::GetFont(), fs*0.88f, ImVec2(at.x + fs*2.5f, at.y + ch*0.5f + fs*0.1f),
			ImGui::GetColorU32(client ? UiSemanticColor(UI_COLOR_SUCCESS) : UiSemanticColor(UI_COLOR_WARNING)), line);
		ImGui::SetNextWindowSize(ImVec2(fs*34, 0), ImGuiCond_Appearing);
		if(ImGui::BeginPopup("game_folder")){ gameSection(true); ImGui::EndPopup(); }
		ImGui::Dummy(ImVec2(0, fs*0.3f));
	}
	bool openLast = GetOpenLastWorkspace();
	if(UiCheckbox(_("Open last workspace at startup"), &openLast)) SetOpenLastWorkspace(openLast);
	ImGui::EndChild();
	ImGui::PopStyleColor();

	// The main area: the chosen workspace and its maps.
	ImGui::SameLine(0, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs*1.6f, fs*1.3f));
	ImGui::BeginChild("main", ImVec2(0, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleVar();
	ImGui::SetWindowFontScale(1.55f);
	ImGui::TextUnformatted(gSelected.empty() ? _("Your maps") : baseName(gSelected).c_str());
	ImGui::SetWindowFontScale(1.0f);
	if(gameLoaded){
		ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - fs*4.5f);
		if(flatButton(_("Skip"), ImVec2(fs*4.5f, 0))) gOpen = false;
		ImGui::SetItemTooltip(_("Return to the editor."));
	}
	ImGui::TextDisabled("%s", gSelected.empty() ? _("Pick up where you left off.") : gSelected.c_str());
	ImGui::Dummy(ImVec2(0, fs*0.5f));
	if(UpdaterHasNews()){
		UpdaterDrawInline();
		if(UpdaterWhatsNewRequested()){
			ImGui::BeginChild("whatsnew", ImVec2(0, fs*6));
			UpdaterDrawWhatsNewInline(); ImGui::EndChild();
		}
	}
	TelemetryDrawConsent();
	if(!gameLoaded){
		beginCard("##game_card_first");
		capsTitle("GTA San Andreas");
		gameSection(false);
		endCard();
		ImGui::Dummy(ImVec2(0, fs*0.4f));
	}
	if(gNewWorkspace){
		beginCard("##new_workspace_card");
		capsTitle(_("New workspace"));
		newWorkspaceForm();
		endCard();
		ImGui::Dummy(ImVec2(0, fs*0.4f));
	}
	ImGui::SetWindowFontScale(1.1f);
	ImGui::TextUnformatted(_("Recent maps"));
	ImGui::SetWindowFontScale(1.0f);
	ImGui::Dummy(ImVec2(0, fs*0.2f));
	float footer = ImGui::GetFrameHeight() + fs*1.2f;
	ImGui::BeginChild("maps", ImVec2(0, std::max(fs*5.0f, ImGui::GetContentRegionAvail().y - footer)));
	mapList(gameLoaded);
	ImGui::EndChild();

	// The actions, at the bottom right.
	ImGui::Dummy(ImVec2(0, fs*0.4f));
	std::string problem;
	bool gameOk = gameLoaded || CheckGameDir(gGameField, &problem);
	const char *open = !gameLoaded || gSelected.empty() ? _("Continue") :
		gSelectedMap.empty() ? _("Open workspace") : _("Open map");
	float primaryWidth = fs*10.0f;
	float secondaryWidth = ImGui::CalcTextSize(_("Continue without workspace")).x + ImGui::GetStyle().FramePadding.x*2;
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
		ImGui::GetWindowContentRegionMax().x - primaryWidth - secondaryWidth - ImGui::GetStyle().ItemSpacing.x));
	ImGui::BeginDisabled(!gameOk);
	if(flatButton(_("Continue without workspace"), ImVec2(secondaryWidth, 0))){
		gSelected.clear();
		if(!gameLoaded) FinishWorkspaceSetup(gGameField, ""); else SetWorkspaceDir(nil);
		gOpen = false;
	}
	ImGui::SameLine();
	if(primaryButton(open, ImVec2(primaryWidth, 0))){
		if(!gameLoaded) FinishWorkspaceSetup(gGameField, gSelected.c_str());
		openSelected(gameLoaded ? gSelectedMap : std::string());
	}
	ImGui::EndDisabled();
	ImGui::EndChild();
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
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
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
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
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
