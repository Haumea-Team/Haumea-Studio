// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Every rebindable keyboard shortcut, in one table. Each action has up to two
// chords (a key plus Ctrl/Shift/Alt); the defaults live in shortcuts.cpp and
// the user's changes are saved in savesettings.txt as "shortcut" lines.
// No shortcut fires while an ImGui text field or widget has the keyboard.

enum ShortcutAction
{
	// File
	SC_NEW_MAP,
	SC_OPEN_MAP,
	SC_SAVE_MAP,
	SC_SAVE_MAP_AS,
	SC_EXPORT_PREFAB,
	SC_IMPORT_PREFAB,
	SC_TEST_IN_GAME,
	// Edit
	SC_UNDO,
	SC_REDO,
	SC_COPY,
	SC_CUT,
	SC_PASTE,
	SC_PASTE_IN_PLACE,
	SC_DUPLICATE,
	SC_DELETE,
	SC_RENAME,
	SC_GROUP_SELECTION,
	SC_CANCEL,
	// Transform
	SC_GIZMO_MOVE,
	SC_GIZMO_ROTATE,
	SC_GIZMO_SPACE,
	SC_SNAP_TO_GROUND,
	SC_ALIGN_TO_GROUND,
	SC_OBJECT_SNAP,
	SC_FOCUS_SELECTION,
	SC_GO_TO_COORDINATES,
	SC_COPY_CAMERA_POSITION,
	SC_NUDGE_LEFT,	// the selection one move step, as the camera looks
	SC_NUDGE_RIGHT,
	SC_NUDGE_FORWARD,
	SC_NUDGE_BACK,
	SC_NUDGE_UP,
	SC_NUDGE_DOWN,
	// View
	SC_OUTLINE_OBJECTS,
	SC_VIEWER_CAMERA,
	SC_HIDE_SELECTION,
	SC_ISOLATE_SELECTION,
	SC_SHOW_ALL,
	SC_PLAYER_VIEW,
	SC_FOV_IN,
	SC_FOV_OUT,
	SC_FOV_RESET,
	SC_FLY_FASTER,
	SC_FLY_SLOWER,
	SC_UI_SCALE_UP,
	SC_UI_SCALE_DOWN,
	SC_UI_SCALE_RESET,
	SC_SCROLL_UP,
	SC_SCROLL_DOWN,
	// Windows
	SC_WINDOW_BROWSER,
	SC_WINDOW_OBJECT_INFO,
	SC_WINDOW_MAP_OBJECTS,
	SC_WINDOW_TOOLS,
	SC_WINDOW_VIEW,
	SC_WINDOW_TIME_WEATHER,
	SC_WINDOW_CAMERA,
	SC_WINDOW_CHANGES,
	SC_WINDOW_PAWN_CODE,
	SC_WINDOW_TEXTURES,
	SC_WINDOW_NODES,
	SC_WINDOW_SHORTCUTS,
	SC_WINDOW_LOG,
	SC_WINDOW_PREFERENCES,
	SC_WINDOW_MAP_CHECK,

	SC_NUM_ACTIONS
};

enum ShortcutContext
{
	SHORTCUT_EDITOR,	// anywhere in the editor, unless typing
	SHORTCUT_VIEWPORT,	// with the mouse over the 3D view
	SHORTCUT_PANEL,	// acts on the panel under the mouse
};

struct ShortcutChord
{
	int key;	// ImGuiKey, 0 when unbound
	int mods;	// ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt
};

enum { SHORTCUT_CHORDS = 2 };

struct ShortcutInfo
{
	const char *id;	// settings name
	const char *section;
	const char *name;
	int context;
	bool repeat;	// fires again while held
};

const ShortcutInfo *GetShortcutInfo(int action);
ShortcutChord GetShortcutChord(int action, int slot);
ShortcutChord GetDefaultShortcutChord(int action, int slot);
void SetShortcutChord(int action, int slot, ShortcutChord chord);
void ResetShortcuts(void);
bool ShortcutIsDefault(int action);

// Once per frame, before the editor runs its shortcuts. blocked: something
// outside ImGui has claimed the keys this frame.
void ShortcutsBeginFrame(bool blocked);
bool ShortcutPressed(int action);
// While rebinding in Preferences, nothing fires.
void ShortcutsSetCapturing(bool capturing);

// "Ctrl+S", or "Ctrl+Y / Ctrl+Shift+Z"; "" when unbound.
const char *ShortcutLabel(int action);
void ShortcutChordName(ShortcutChord chord, char *buf, int size);
bool ParseShortcutChord(const char *text, ShortcutChord *chord);
// The chord being pressed right now, for rebinding.
bool ShortcutReadPressedChord(ShortcutChord *chord);
// Another action in a context that can fire at the same time, or -1.
int ShortcutConflict(int action, int slot);

// savesettings.txt: "shortcut <id> <chord|none> <chord|none>" for changed actions.
bool ShortcutParseSetting(const char *value);
void ShortcutWriteSettings(void *file);
