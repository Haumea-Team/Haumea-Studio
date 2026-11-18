// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

enum PawnEditorAction {
	PAWN_EDITOR_NEW,
	PAWN_EDITOR_OPEN,
	PAWN_EDITOR_SAVE,
	PAWN_EDITOR_SAVE_AS,
	PAWN_EDITOR_REFRESH,
	PAWN_EDITOR_UNDO,
	PAWN_EDITOR_REDO,
	PAWN_EDITOR_CUT,
	PAWN_EDITOR_COPY,
	PAWN_EDITOR_PASTE,
	PAWN_EDITOR_SELECT_ALL
};
void PawnEditorRequest(PawnEditorAction action);
void PawnEditorDraw(void);
// The ribbon's Code commands.
void PawnEditorFindReplace(void);
int PawnEditorFormat(void);
void PawnEditorSetFormat(int format);
int PawnEditorEncoding(void);
void PawnEditorSetEncoding(int encoding);
std::string PawnEditorDocumentName(void);
void PawnEditorTick(void);
void PawnEditorFlushDraft(void);
bool PawnEditorHasUnsavedChanges(void);
bool InPawnIdeMode(void);

// UI automation checks the real editor document, not the generated map.
const std::string &PawnEditorText(void);
// For Namaka: the text (generated from the map when nothing is loaded yet),
// replacing it, regenerating it from the map, opening a script and saving it
// (to path, or its own file). Errors are the editor's messages.
const std::string &PawnEditorAgentText(void);
void PawnEditorAgentSetText(const std::string &text);
bool PawnEditorAgentRefresh(std::string *message);
bool PawnEditorAgentOpen(const char *path, std::string *message);
bool PawnEditorAgentSave(const char *path, std::string *message);
std::string PawnEditorAgentFile(void);
bool PawnEditorAgentFollowsMap(void);
