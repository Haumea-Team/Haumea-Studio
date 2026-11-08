// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

// Where Haumea finds things. Haumea runs from its own folder: the game folder
// (GTA San Andreas) is only ever read, the workspace is the SA-MP/open.mp
// server being mapped, and settings and editor data live in the config
// folder (~/.config/haumea, %APPDATA%\Haumea).

// Reads --game/--workspace, the saved settings and the old drop-in layout,
// and moves <GTA>/haumea_data to the config folder once. Call first.
void InitWorkspace(void);

const char *GetSuggestedGameDir(void); // chosen/saved path even when it needs repair
const char *GetGameDir(void);	// absolute; empty until one is chosen
const char *GetWorkspaceDir(void);	// absolute; empty when none is set
const char *GetConfigDir(void);	// absolute, created on demand

// game/<relative>, case-corrected like the game's own loader. Absolute paths
// are returned as they are.
std::string GamePath(const char *relative);
bool GamePath(const char *relative, char *dst, size_t size);
// workspace/<relative> (no case correction), or "" without a workspace.
std::string WorkspacePath(const char *relative);
// The workspace folder for relative if it exists, else the workspace itself,
// else "": a default folder for file dialogs.
std::string WorkspaceDefaultDir(const char *relative);
// Whether path is inside the game folder, which Haumea never writes to.
bool IsInGameDir(const char *path);

// Check the readable core GTA files needed before loading the scene.
bool CheckGameDir(const char *dir, std::string *problem);
// A server folder has gamemodes/, filterscripts/, scriptfiles/ or a server
// executable. Only a warning: any folder can be the workspace.
bool CheckWorkspaceDir(const char *dir, std::string *warning);

// Saved for the next start; the game folder is loaded once, so a new one
// applies after a restart.
bool SetGameDir(const char *dir, std::string *problem);
void SetWorkspaceDir(const char *dir);	// nil or "" for none
const std::vector<std::string> &GetRecentWorkspaces(void);	// last opened first
// The same with when each was last opened (0: unknown, from an older build)
// and pins, pinned first. Pinned ones never drop off the list.
struct RecentWorkspace { std::string path; long long lastOpened; bool pinned; };
std::vector<RecentWorkspace> GetRecentWorkspaceList(void);
void PinRecentWorkspace(const char *path, bool pinned);
void RemoveRecentWorkspace(const char *path);
// "Open the last workspace automatically": no start screen on launch.
bool GetOpenLastWorkspace(void);
void SetOpenLastWorkspace(bool open);

// True for --pawn-roundtrip and --no-save-settings: developer and test runs
// read the settings but never write them (settings, recent maps, window
// state, autosaves, the workspace).
bool SettingsAreReadOnly(void);
// --background: a hidden editor that an agent drives (agentbridge.cpp).
bool IsBackgroundWorker(void);
// Test runs and command-line modes (SettingsAreReadOnly, --ui-script, or any
// editor a test starts with HAUMEA_TEST_XVFB=1): no update check, telemetry,
// Namaka listening or start screen, whatever else the environment says.
bool IsAutomatedRun(void);

// Linux file dialogs run zenity or kdialog. Without either this shows a
// toast saying so and returns false; elsewhere it is always true.
bool FileDialogAvailable(void);
// Native folder picker; false when cancelled.
bool PickFolderDialog(char *dst, size_t size, const char *title, const char *startDir);
// Native file picker for files of the extensions given ("png bmp tga"), under
// a filter called kind ("Images"); false when cancelled.
bool PickFileDialog(char *dst, size_t size, const char *title, const char *kind, const char *extensions);

// First-run setup: true while no valid game folder is known. The start
// screen (startscreen.cpp) asks for one and calls FinishWorkspaceSetup.
bool WorkspaceSetupNeeded(void);
void FinishWorkspaceSetup(const char *game, const char *workspace);
// Preferences / File > Workspace widgets. Returns true when the game folder
// changed and Haumea needs a restart.
bool uiWorkspaceSettings(void);
