// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdio.h>
#include <string>
#include <vector>

// SA-MP textdraws: designed on the client's 640x448 canvas and written to the
// map as TextDrawCreate / CreatePlayerTextDraw calls.

enum TextDrawCodeTarget {
	TEXTDRAW_CODE_SAMP,
	TEXTDRAW_CODE_OPENMP,
};

enum {
	TD_FONT_BECKETT,	// 0..3 are text fonts
	TD_FONT_AHARONI,
	TD_FONT_BANK_GOTHIC,
	TD_FONT_PRICEDOWN,
	TD_FONT_SPRITE,	// text is "txd:texture"
	TD_FONT_PREVIEW,	// a 3D model
	// Not a textdraw: an image to build on (text is its file), drawn in the
	// editor only and kept in the XML companion.
	TD_FONT_REFERENCE,
};

struct TextDraw
{
	char name[32];	// the Pawn variable
	bool perPlayer;	// CreatePlayerTextDraw
	float x, y;
	std::string text;
	float letterX, letterY;	// TextDrawLetterSize
	float sizeX, sizeY;	// TextDrawTextSize
	int alignment;	// 1 left, 2 centre, 3 right
	unsigned int colour, boxColour, backgroundColour;	// 0xRRGGBBAA
	bool useBox;
	int shadow, outline;
	int font;
	bool proportional;
	bool selectable;
	int previewModel;
	float previewRotX, previewRotY, previewRotZ, previewZoom;
	int previewColour1, previewColour2;
	// Editor properties saved in XML; legacy // Haumea: comments are still read.
	bool locked;	// can't be dragged on the canvas
	bool hidden;	// not drawn in the editor (it is still in the file)
	std::string group;	// a name shared by textdraws edited together
	std::string board;	// its artboard, a screen of its own ("" is the first, Main)
	// A shape's settings, on each of the sprites it is made of, so it can be
	// changed after it is added: "rounded x y w h ..." (textdraws_ui.cpp).
	std::string shape;
	// Preview source for a server sprite whose registration is built dynamically
	// by Pawn. Relative to the server's artwork folder; Pawn still uses mdl-ID.
	std::string serverTxd;
};

// A ruler guide on a board: a line at x (vertical) or y, saved with the file.
struct TextDrawGuide
{
	std::string board;
	bool vertical;
	float at;
};
std::vector<TextDrawGuide> &GetTextDrawGuides(void);
void TextDrawGuidesChanged(void);	// marks the file unsaved, without an undo step

// The file's artboards in order, "" (Main) first when it has textdraws.
void TextDrawBoards(const std::vector<TextDraw> &tds, std::vector<std::string> &out);
// The Pawn name a board's functions end in: Main, Login, Speedo2...
std::string TextDrawBoardSuffix(const std::string &board);

void InitTextDraw(TextDraw *td);	// the client's defaults

// The textdraw file: textdraws live in their own .pwn/.inc, apart from the map.
std::vector<TextDraw> &GetTextDraws(void);
void TextDrawsChanged(void);	// after an edit: marks the file unsaved; an undo step follows
void TextDrawsReplacedByOthers(void);	// multiplayer: marks it unsaved, and undo starts again
unsigned int TextDrawsChangeSeq(void);	// moves with every edit, undo and save
const char *GetTextDrawFilePath(void);	// "" while untitled
const char *GetTextDrawFileName(void);	// the file name, or "Untitled textdraws"
bool TextDrawFileHasUnsavedChanges(void);
void NewTextDrawFile(void);
bool OpenTextDrawFile(const char *path, std::string *error);
bool SaveTextDrawFile(const char *path, std::string *error);	// nil or "": its own path
TextDrawCodeTarget GetTextDrawCodeTarget(void);
void SetTextDrawCodeTarget(TextDrawCodeTarget target);
void TextDrawCodeTargetChanged(void);	// marks the file unsaved without adding an undo step
// The file's code (UTF-8), with the function prefix its path gives.
std::string BuildTextDrawFileCode(const char *path);
std::string BuildTextDrawFileCode(const char *path, TextDrawCodeTarget target);
std::string TextDrawFilePrefix(const char *path);	// Haumea_<name>

// Undo for the textdraw file, apart from the map's.
void UpdateTextDrawsUndo(void);	// every frame: a finished edit becomes one step
bool UndoTextDraws(void);
bool RedoTextDraws(void);

// Textdraws read from a map made before they had their own file. They are
// read-only and saved back with the map until moved to a textdraw file.
std::vector<TextDraw> &GetMapTextDraws(void);
void ResetMapTextDraws(void);
std::string SuggestedTextDrawFilePath(const char *mapPath);
// Appends them to the textdraw file and saves it: to path when it is untitled,
// else to its own file (a different path is refused). The map then saves
// without them. Returns how many moved, or -1.
int MoveMapTextDrawsToFile(const char *path, std::string *error);

// Pawn for the map's own (read-only) textdraws: the header line, the globals, the two functions.
bool HasTextDraws(void);
void WriteTextDrawsPawnHeader(FILE *f, const char *prefix);
void WriteTextDrawsPawnGlobals(FILE *f, const char *prefix);
void WriteTextDrawsPawnFunctions(FILE *f, const char *prefix);
// Reads TextDrawCreate / CreatePlayerTextDraw and their setters into out.
// Returns how many textdraws were added.
int ImportTextDrawsFromSource(const std::string &source, std::vector<TextDraw> &out,
	std::vector<TextDrawGuide> *guides = nil);

// textdraws_ui.cpp: TextDraw mode, where the textdraw file is edited on a
// canvas instead of the world and map editing is off.
bool InTextDrawMode(void);
bool TextDrawModeHidesWorld(void);	// false for the frame the still frame is taken in
void EnterTextDrawMode(void);
void ExitTextDrawMode(void);
void DrawTextDrawsToolbar(void);
void DrawTextDrawsWindow(void);
float TextDrawCanvasToScreen(float at, bool vertical, bool ruler);	// for UI scripts
float TextDrawCanvasZoom(void);	// 1 when it fits
bool AddTextDrawImageLayer(const char *path);	// a dropped picture, in TextDraw mode	// the mode's panel, banner and popups
void DrawTextDrawsOverlay(void);	// the canvas
// Each line's text colours as RRGGBBAA, after the ~colour~, ~h~ and ~n~
// tokens (tests: --textdraw-colours).
std::string DescribeTextDrawColours(const TextDraw &td);
bool TextDrawSpriteTextureAvailable(const TextDraw &td);
void TextDrawsBeforeWorldRender(void);	// model previews, offscreen
void TextDrawsAfterWorldRender(void);	// takes the still frame
bool TextDrawModeShortcut(int action);	// true when the mode handled it
void SelectTextDraw(int index);	// enters TextDraw mode with it selected, e.g. from Map Check
// Asks about unsaved textdraws, then runs then (or not, on Cancel).
void RequestTextDrawFileClose(void (*then)(void));
void OfferMoveMapTextDraws(const char *mapPath);	// after a map with textdraws is opened
// Settings, kept with the editor's own: recent files (quoted by the caller) and numbers.
const std::vector<std::string> &GetRecentTextDrawFiles(void);
void AddRecentTextDrawFile(const char *path);	// on top
bool LoadTextDrawSetting(const char *key, const char *value);	// false: not a textdraw setting
void SaveTextDrawSettings(FILE *f);
