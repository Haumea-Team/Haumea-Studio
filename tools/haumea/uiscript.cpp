// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "terminal.h"
#include "multiplayer.h"
#include "particles.h"
#include "documentxml.h"
#include "coordinatepin.h"
#include "maptools.h"
#include "layoutpaths.h"
#include "mapcleanup.h"
#include "alignmentmath.h"
#include "browserdiscovery.h"
#include "crashreports.h"
#include "editoricons.h"
#include "trains.h"
#include "workspace.h"
#include "pawneditor.h"
#include "shortcuts.h"
#include "playerview.h"
#include "keycast.h"
#include "radarart.h"
#include "i18n.h"
#include "imgui/imgui_internal.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#ifdef RW_GL3
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

// --ui-script <file>: drives the editor through its own input layer, for
// tests where no input tool can reach the window (Wayland, CI). Events go in
// through sk::EventHandler, so they reach ImGui, CPad and the shortcuts the
// way real ones do. ImGui items are found by window and label through ImGui's
// test-engine hooks. Exits with 0 when every assert passed, else 1.
//
// One command per line ('#' starts a comment; quote arguments with spaces):
//   assert work_area_top < 30    viewport height reserved above the workspace
//   assert item_count "Code" == 1 visible controls with this label
//   assert rendered.map_model 3555 == 1  visible map instances of a model
//   assert rendered.lod_overlaps == 0    visible LODs covering visible children
//   assert render_far_clip < 1000        current weather/editor far clip
//   assert new_dot textdraws == 1         the TextDraws (or code) button's "new" dot
//   wait N                      frames
//   wait_until <assert>         up to 600 frames
//   key Ctrl+Shift+H            press and release a chord
//   shortcut <id|name>          the chord of a shortcut-table action
//   hold KEY / release KEY      also left, right or middle (a mouse button)
//   move X Y                    pixels, or 50% 40% of the window
//   click [X Y] [left|right|middle]
//   drag X1 Y1 X2 Y2 [frames]
//   wheel N
//   click_object N|model=M [right] [jitter]  a map object, where it is on screen
//   camera X Y Z TX TY TZ / mark_camera
//   click_item 'Window' 'Label' [N] [right]   '*' for any window; N: the Nth match, -1 the last
//   hover_item 'Window' 'Label'
//   set_item 'Window' 'Label' "text" [N] [tab] [ctrl] types into a field ('Colour/##X' = red)
//   close_popup                         clicks the empty end of the menu bar
//   reopen                              opens the map file again
//   click_world X Y Z [shift|right]    looks at a world point and clicks it
//   scroll_until 'Window' 'Label'      scrolls the window until the item shows ('*': the open popup)
//   dump_items [filter]                 lists what ImGui drew last frame
//   drag_item 'Window' 'Label' DX DY [N]
//   drag_item_to 'Window' 'Label' 'Window' 'Label'   drops one item on another
//   type "text"
//   clipboard "text"            what a paste will get
//   open <map.pwn>              opens a map as File > Open would
//   $NAME / ${NAME} in arguments: environment variables, and $UI_CONFIG_DIR (the config folder)
//   grab <file.png>
//   assert <what> [op value]    see uiAssert below
//   log "text" / fail "text" / exit

static bool gActive;
static std::vector<std::string> gLines;
static size_t gLine;
static int gFails, gPasses;
static int gFrame;
static FILE *gLog;

// ---- Items seen through the test-engine hooks ----

struct UiItem {
	// seed: the ID stack top, to match items ImGui gives no label (closed combos);
	// parent and grand: the two below it, for parts of a widget ('Colour/##X', 'Size/#1')
	ImGuiID id, seed, parent, grand;
	ImRect bb, clip;	// clip: the visible part of its window when it was drawn
	std::string window, rootWindow, label, rawLabel;
	std::string windowName, rootWindowName;	// with any "##" ID
};
static std::vector<UiItem> gItems, gPrevItems;
static std::map<ImGuiID, size_t> gItemIndex;
// Tabs report their labels before ItemAdd; keep those labels for the later hook.
static std::map<ImGuiID, std::string> gItemLabels;

static std::string
visibleTitle(const char *name)
{
	std::string s = name ? name : "";
	size_t hash = s.find("##");
	if(hash != std::string::npos)
		s.resize(hash);
	return s;
}

void
ImGuiTestEngineHook_ItemAdd(ImGuiContext *ctx, ImGuiID id, const ImRect &bb, const ImGuiLastItemData*)
{
	if(!gActive || id == 0)
		return;
	ImGuiWindow *w = ctx->CurrentWindow;
	UiItem item;
	item.id = id;
	item.seed = item.parent = item.grand = 0;
	item.bb = bb;
	item.clip = bb;
	if(w){
		item.clip = w->ClipRect;
		int n = w->IDStack.Size;
		item.seed = n > 0 ? w->IDStack[n-1] : 0;
		item.parent = n > 1 ? w->IDStack[n-2] : 0;
		item.grand = n > 2 ? w->IDStack[n-3] : 0;
		item.window = visibleTitle(w->Name);
		item.rootWindow = visibleTitle(w->RootWindow ? w->RootWindow->Name : w->Name);
		item.windowName = w->Name;
		item.rootWindowName = w->RootWindow ? w->RootWindow->Name : w->Name;
	}
	auto label = gItemLabels.find(id);
	if(label != gItemLabels.end()){
		item.rawLabel = label->second;
		item.label = visibleTitle(item.rawLabel.c_str());
	}
	gItemIndex[id] = gItems.size();
	gItems.push_back(item);
}

void
ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char *label, ImGuiItemStatusFlags)
{
	if(!gActive || label == nil)
		return;
	gItemLabels[id] = label;
	std::map<ImGuiID, size_t>::iterator it = gItemIndex.find(id);
	if(it != gItemIndex.end()){
		gItems[it->second].label = visibleTitle(label);
		gItems[it->second].rawLabel = label;
	}
}

void
ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...)
{
}

const char*
ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID)
{
	return nil;
}

// Text without leading icon glyphs and spaces.
static std::string
plainLabel(const std::string &s)
{
	// Font Awesome uses private-use characters. Other UTF-8 characters can
	// be the words themselves (Chinese, or an accented first letter).
	size_t i = 0;
	while(i < s.size()){
		unsigned char ch = (unsigned char)s[i];
		if(ch < 0x80){
			if(isalnum(ch)) break;
			i++;
		}else{
			unsigned int cp;
			int length = ImTextCharFromUtf8(&cp, s.c_str() + i, s.c_str() + s.size());
			if(length <= 0 || cp < 0xE000 || cp > 0xF8FF) break;
			i += length;
		}
	}
	return s.substr(i);
}

// Scripts name things in English in every language: a window or item matches
// by the text shown, by the English ID L() puts after the last "###", or by
// the text translated. part: the name may be part of it.
static bool
nameMatches(const std::string &raw, const std::string &name, bool part)
{
	size_t hash = raw.rfind("###");
	std::string shown = visibleTitle(raw.c_str());
	if(name.empty())	// '': an item with no text of its own
		return plainLabel(shown).empty();
	std::string id = hash == std::string::npos ? "" : raw.substr(hash + 3);
	// Translate() caches by pointer: give it one lasting copy per name.
	static std::set<std::string> asked;
	std::string names[2] = { name, Translate(asked.insert(name).first->c_str()) };
	for(const std::string &n : names){
		if(n.empty())
			continue;
		for(const std::string *t : { &shown, &id }){
			if(t->empty())
				continue;
			std::string plain = plainLabel(*t);
			if(*t == n || plain == n || (part && plain.find(n) != std::string::npos))
				return true;
		}
	}
	return false;
}

static bool
windowMatches(const UiItem &item, const std::string &window)
{
	if(window == "*" || window.empty())
		return true;
	return nameMatches(item.windowName, window, true) ||
		nameMatches(item.rootWindowName, window, true);
}

static const UiItem *findItem(const std::string &window, const std::string &label, int nth = 1);

// The ID label gets under seed: the label hashed, or "###label" for an
// L()/LI() label, whose ID is the English text in every language.
static bool
hashesTo(ImGuiID id, const std::string &label, ImGuiID seed)
{
	return ImHashStr(label.c_str(), 0, seed) == id ||
		ImHashStr(("###" + label).c_str(), 0, seed) == id;
}

// An item ImGui labels before it is added (a tab) has only its ID.
static bool
idIs(const UiItem &it, const std::string &label)
{
	return hashesTo(it.id, label, it.seed);
}

// 'Widget/part': a part of a widget that pushes its label as an ID, like a
// colour channel ('Colour/##X') or a DragFloat2 component ('Size (X, Y)/#1').
static const UiItem*
findPart(const std::string &window, const std::string &widget, const std::string &part, int nth)
{
	int seen = 0;
	bool index = part.size() > 1 && part[0] == '#' && isdigit((unsigned char)part[1]);
	int i = index ? atoi(part.c_str() + 1) : 0;
	for(size_t k = 0; k < gPrevItems.size(); k++){
		const UiItem &it = gPrevItems[k];
		if(!windowMatches(it, window))
			continue;
		bool match;
		if(index)
			match = hashesTo(it.parent, widget, it.grand) &&
				it.seed == ImHashData(&i, sizeof(i), it.parent);
		else
			match = (hashesTo(it.seed, widget, it.parent) &&
				(it.rawLabel == part || nameMatches(it.rawLabel, part, false) ||
				 (it.label.empty() && idIs(it, part)))) ||
				// A field one ID deeper: InputInt("##value") inside PushID("Colour 1").
				(it.rawLabel == part && hashesTo(it.parent, widget, it.grand));
		if(match && ++seen == nth)
			return &it;
	}
	return nil;
}

static const UiItem*
findItem(const std::string &window, const std::string &label, int nth)
{
	size_t slash = label.rfind('/');
	if(slash != std::string::npos && slash > 0 && slash + 1 < label.size()){
		const UiItem *part = findPart(window, label.substr(0, slash), label.substr(slash + 1), nth);
		if(part || label[slash+1] == '#')
			return part;
	}
	const UiItem *contains = nil;
	// A label starting with "##" is an ID: matched against the raw label.
	if(label.compare(0, 2, "##") == 0){
		int seen = 0;
		for(size_t i = 0; i < gPrevItems.size(); i++){
			const UiItem &it = gPrevItems[i];
			// The whole ID: '##tile' is not '##tile_size'.
			size_t at = it.rawLabel.find(label), end = at + label.size();
			bool named = it.label.empty() && it.rawLabel.empty() ?
				idIs(it, label) :
				at != std::string::npos && (end == it.rawLabel.size() || it.rawLabel[end] == '#');
			if(windowMatches(it, window) && named && ++seen == nth)
				return &it;
		}
		return nil;
	}
	// A negative N counts from the last one drawn.
	if(nth > 1 || nth < 0){
		std::vector<const UiItem*> matches;
		for(size_t i = 0; i < gPrevItems.size(); i++){
			const UiItem &it = gPrevItems[i];
			bool named = nameMatches(it.rawLabel, label, false) ||
				(it.label.empty() && idIs(it, label));
			if(windowMatches(it, window) && named)
				matches.push_back(&it);
		}
		int at = nth > 0 ? nth - 1 : (int)matches.size() + nth;
		return at >= 0 && at < (int)matches.size() ? matches[at] : nil;
	}
	for(size_t i = 0; i < gPrevItems.size(); i++){
		const UiItem &it = gPrevItems[i];
		if(!windowMatches(it, window))
			continue;
		if(it.label.empty()){
			if(idIs(it, label))
				return &it;
			continue;
		}
		if(nameMatches(it.rawLabel, label, false)){
			// Window diagnostics cover the whole panel. Prefer its dock tab
			// (or another actual control) when a script clicks a panel's name.
			if(it.id != ImHashStr(it.windowName.c_str()))
				return &it;
			contains = &it;
		}
		if(!contains && nameMatches(it.rawLabel, label, true))
			contains = &it;
	}
	return contains;
}

// ---- Logging ----

static void
say(const char *fmt, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	printf("[ui-script] %s\n", buf);
	fflush(stdout);
	if(gLog){
		fprintf(gLog, "%s\n", buf);
		fflush(gLog);
	}
}

// After a failed lookup: what the script could have clicked, to see why.
static void
dumpSeen(const std::string &window)
{
	int shown = 0, total = 0;
	for(size_t i = 0; i < gPrevItems.size(); i++){
		const UiItem &it = gPrevItems[i];
		if(it.label.empty() || !windowMatches(it, window))
			continue;
		if(shown++ < 40)
			say("  seen '%s' in '%s' at %.0f,%.0f", it.label.c_str(), it.window.c_str(), it.bb.Min.x, it.bb.Min.y);
		total++;
	}
	say("  (%d labelled item(s) in '%s', %d in all)", total, window.c_str(), (int)gPrevItems.size());
}

static std::string gLastToast;
static rw::V3d gMarkedCamera;
static bool gObjectFramed;

void
UiScriptNoteToast(const char *text)
{
	if(gActive){
		gLastToast = text ? text : "";
		say("toast: %s", gLastToast.c_str());
	}
}

// ---- Input ----

struct KeyName { const char *name; int key; };
static const KeyName keyNames[] = {
	{ "esc", sk::KEY_ESC }, { "escape", sk::KEY_ESC }, { "enter", sk::KEY_ENTER }, { "return", sk::KEY_ENTER },
	{ "tab", sk::KEY_TAB }, { "backspace", sk::KEY_BACKSP }, { "delete", sk::KEY_DEL }, { "del", sk::KEY_DEL },
	{ "insert", sk::KEY_INS }, { "home", sk::KEY_HOME }, { "end", sk::KEY_END },
	{ "pageup", sk::KEY_PGUP }, { "pagedown", sk::KEY_PGDN },
	{ "up", sk::KEY_UP }, { "down", sk::KEY_DOWN }, { "left", sk::KEY_LEFT }, { "right", sk::KEY_RIGHT },
	{ "uparrow", sk::KEY_UP }, { "downarrow", sk::KEY_DOWN }, { "leftarrow", sk::KEY_LEFT }, { "rightarrow", sk::KEY_RIGHT },
	{ "space", ' ' }, { "equal", '=' }, { "minus", '-' }, { "period", '.' }, { "slash", '/' },
	{ "ctrl", sk::KEY_LCTRL }, { "shift", sk::KEY_LSHIFT }, { "alt", sk::KEY_LALT },
	{ "keypadadd", sk::KEY_PADADD }, { "keypadsubtract", sk::KEY_PADSUBTRACT },
	{ "keypadmultiply", sk::KEY_PADMULTIPLY }, { "keypaddivide", sk::KEY_PADDIVIDE },
	{ "keypaddecimal", sk::KEY_PADDECIMAL }, { "keypadenter", sk::KEY_PADENTER }, { "keypadequal", sk::KEY_PADEQUAL },
};

static int
keyFromName(std::string name)
{
	std::string n;
	for(size_t i = 0; i < name.size(); i++)
		if(name[i] != ' ')
			n += (char)tolower((unsigned char)name[i]);
	if(n.size() == 1){
		char c = n[0];
		return isalpha((unsigned char)c) ? toupper(c) : c;
	}
	if(n.size() >= 2 && n[0] == 'f' && isdigit((unsigned char)n[1])){
		int f = atoi(n.c_str() + 1);
		if(f >= 1 && f <= 12) return sk::KEY_F1 + f - 1;
	}
	for(const char *prefix : { "keypad", "numpad", "num", "pad" }){
		size_t len = strlen(prefix);
		if(n.compare(0, len, prefix) == 0 && n.size() == len + 1 && isdigit((unsigned char)n[len]))
			return sk::KEY_PAD0 + (n[len] - '0');
	}
	if(n == "numpadadd" || n == "padadd" || n == "num+") return sk::KEY_PADADD;
	if(n == "numpadsubtract" || n == "padsubtract" || n == "num-") return sk::KEY_PADSUBTRACT;
	for(const KeyName &k : keyNames)
		if(n == k.name)
			return k.key;
	return -1;
}

#if defined(_WIN32) && defined(RW_D3D9)
// d3d9: ImGui takes its input from Win32 messages, not from sk events, so the
// script's keys and mouse go to ImGui directly as well.
#define UI_FEED_IMGUI
static ImGuiKey
imguiKey(int key)
{
	if(key >= 'A' && key <= 'Z') return (ImGuiKey)(ImGuiKey_A + key - 'A');
	if(key >= '0' && key <= '9') return (ImGuiKey)(ImGuiKey_0 + key - '0');
	if(key >= sk::KEY_F1 && key <= sk::KEY_F12) return (ImGuiKey)(ImGuiKey_F1 + key - sk::KEY_F1);
	if(key >= sk::KEY_PAD0 && key <= sk::KEY_PAD9) return (ImGuiKey)(ImGuiKey_Keypad0 + key - sk::KEY_PAD0);
	static const struct { int sk; ImGuiKey imgui; } keys[] = {
		{ ' ', ImGuiKey_Space }, { '\'', ImGuiKey_Apostrophe }, { ',', ImGuiKey_Comma },
		{ '-', ImGuiKey_Minus }, { '.', ImGuiKey_Period }, { '/', ImGuiKey_Slash },
		{ ';', ImGuiKey_Semicolon }, { '=', ImGuiKey_Equal }, { '[', ImGuiKey_LeftBracket },
		{ '\\', ImGuiKey_Backslash }, { ']', ImGuiKey_RightBracket }, { '`', ImGuiKey_GraveAccent },
		{ sk::KEY_ESC, ImGuiKey_Escape }, { sk::KEY_ENTER, ImGuiKey_Enter }, { sk::KEY_TAB, ImGuiKey_Tab },
		{ sk::KEY_BACKSP, ImGuiKey_Backspace }, { sk::KEY_INS, ImGuiKey_Insert }, { sk::KEY_DEL, ImGuiKey_Delete },
		{ sk::KEY_RIGHT, ImGuiKey_RightArrow }, { sk::KEY_LEFT, ImGuiKey_LeftArrow },
		{ sk::KEY_DOWN, ImGuiKey_DownArrow }, { sk::KEY_UP, ImGuiKey_UpArrow },
		{ sk::KEY_PGUP, ImGuiKey_PageUp }, { sk::KEY_PGDN, ImGuiKey_PageDown },
		{ sk::KEY_HOME, ImGuiKey_Home }, { sk::KEY_END, ImGuiKey_End },
		{ sk::KEY_LSHIFT, ImGuiKey_LeftShift }, { sk::KEY_RSHIFT, ImGuiKey_RightShift },
		{ sk::KEY_LCTRL, ImGuiKey_LeftCtrl }, { sk::KEY_RCTRL, ImGuiKey_RightCtrl },
		{ sk::KEY_LALT, ImGuiKey_LeftAlt }, { sk::KEY_RALT, ImGuiKey_RightAlt },
		{ sk::KEY_PADDECIMAL, ImGuiKey_KeypadDecimal }, { sk::KEY_PADDIVIDE, ImGuiKey_KeypadDivide },
		{ sk::KEY_PADMULTIPLY, ImGuiKey_KeypadMultiply }, { sk::KEY_PADSUBTRACT, ImGuiKey_KeypadSubtract },
		{ sk::KEY_PADADD, ImGuiKey_KeypadAdd }, { sk::KEY_PADENTER, ImGuiKey_KeypadEnter },
	};
	for(const auto &k : keys)
		if(k.sk == key)
			return k.imgui;
	return ImGuiKey_None;
}

static void
feedImGui(sk::Event e, void *param)
{
	ImGuiIO &io = ImGui::GetIO();
	if(e == sk::KEYDOWN || e == sk::KEYUP){
		int key = *(int*)param;
		bool down = e == sk::KEYDOWN;
		ImGuiKey k = imguiKey(key);
		if(k != ImGuiKey_None)
			io.AddKeyEvent(k, down);
		if(key == sk::KEY_LCTRL || key == sk::KEY_RCTRL) io.AddKeyEvent(ImGuiMod_Ctrl, down);
		if(key == sk::KEY_LSHIFT || key == sk::KEY_RSHIFT) io.AddKeyEvent(ImGuiMod_Shift, down);
		if(key == sk::KEY_LALT || key == sk::KEY_RALT) io.AddKeyEvent(ImGuiMod_Alt, down);
	}else if(e == sk::CHARINPUT)
		io.AddInputCharacter((unsigned)(uintptr)param);
	else if(e == sk::MOUSEMOVE || e == sk::MOUSEBTN || e == sk::MOUSEWHEEL){
		sk::MouseState *ms = (sk::MouseState*)param;
		if(e == sk::MOUSEMOVE)
			io.AddMousePosEvent((float)ms->posx, (float)ms->posy);
		else if(e == sk::MOUSEBTN){
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, (ms->buttons & 1) != 0);
			io.AddMouseButtonEvent(ImGuiMouseButton_Middle, (ms->buttons & 2) != 0);
			io.AddMouseButtonEvent(ImGuiMouseButton_Right, (ms->buttons & 4) != 0);
		}else
			io.AddMouseWheelEvent(0.0f, ms->wheelDelta);
	}
}
#endif

// While a script runs, input comes only from it: the X server still sends
// pointer events (a window mapping under the pointer, at times that vary
// with load), and one landing between a hover and an assert moved the mouse
// off the item. AppEventHandler drops those (UiScriptDropsEvent).
static bool gSending;

static void
send(sk::Event e, void *param)
{
	gSending = true;
	sk::EventHandler(e, param);
	gSending = false;
#ifdef UI_FEED_IMGUI
	feedImGui(e, param);
#endif
}

bool
UiScriptDropsEvent(sk::Event e)
{
	return gActive && !gSending &&
		(e == sk::KEYDOWN || e == sk::KEYUP || e == sk::CHARINPUT ||
		 e == sk::MOUSEMOVE || e == sk::MOUSEBTN || e == sk::MOUSEWHEEL);
}

static void
sendKey(int key, bool down)
{
	send(down ? sk::KEYDOWN : sk::KEYUP, &key);
}

static int gMouseX, gMouseY, gButtons;

static void
sendMouse(void)
{
	sk::MouseState ms = {};
	ms.posx = gMouseX;
	ms.posy = gMouseY;
	ms.buttons = gButtons;
	send(sk::MOUSEMOVE, &ms);
	send(sk::MOUSEBTN, &ms);
}

static int
buttonBit(const std::string &b)
{
	return b == "right" ? 4 : b == "middle" ? 2 : 1;
}

// ---- Script ----

// $NAME and ${NAME} become environment variables (the test runner's paths).
static std::string
expand(const std::string &in)
{
	std::string out;
	for(size_t i = 0; i < in.size(); i++){
		if(in[i] != '$' || i + 1 >= in.size()){
			out += in[i];
			continue;
		}
		bool brace = in[i+1] == '{';
		size_t j = i + 1 + (brace ? 1 : 0), start = j;
		while(j < in.size() && (isalnum((unsigned char)in[j]) || in[j] == '_')) j++;
		std::string name = in.substr(start, j - start);
		const char *v = name.empty() ? nil : getenv(name.c_str());
		if(name.empty()){
			out += in[i];
			continue;
		}
		out += v ? v : "";
		i = j - (brace ? 0 : 1);
	}
	return out;
}

static std::vector<std::string>
tokens(const std::string &line)
{
	std::vector<std::string> out;
	size_t i = 0;
	while(i < line.size()){
		while(i < line.size() && isspace((unsigned char)line[i])) i++;
		if(i >= line.size() || line[i] == '#') break;
		std::string t;
		if(line[i] == '\'' || line[i] == '"'){
			char q = line[i++];
			while(i < line.size() && line[i] != q){
				if(line[i] == '\\' && i + 1 < line.size()) i++;
				t += line[i++];
			}
			i++;
		}else
			while(i < line.size() && !isspace((unsigned char)line[i])) t += line[i++];
		// Variables after the quotes: a Windows path's backslashes stay as they are.
		out.push_back(expand(t));
	}
	return out;
}

static int
coordinate(const std::string &s, bool vertical)
{
	ImGuiIO &io = ImGui::GetIO();
	if(!s.empty() && s.back() == '%')
		return (int)(atof(s.c_str()) / 100.0f * (vertical ? io.DisplaySize.y : io.DisplaySize.x));
	if(s == "pin" || s == "pinbase" || s == "pinend"){
		ImVec2 point;
		if(CoordinatePinScreen(&point, s == "pinbase" ? 1 : s == "pinend" ? 2 : 0)){
			ImVec2 origin = ImGui::GetMainViewport()->Pos;
			return (int)(vertical ? point.y - origin.y : point.x - origin.x);
		}
		return -1000;
	}
	if(s.compare(0,5,"path:")==0){
		int index=0,handle=0;
		sscanf(s.c_str()+5,"%d:%d",&index,&handle);
		ImVec2 point;
		if(LayoutPathPointScreen(&point,index,handle)){
			ImVec2 origin=ImGui::GetMainViewport()->Pos;
			return (int)(vertical?point.y-origin.y:point.x-origin.x);
		}
		return -1000;
	}
	if(s.compare(0,4,"obj:") == 0){
		int model, x, y, z;
		if(sscanf(s.c_str(),"obj:%d:%d:%d:%d",&model,&x,&y,&z) == 4){
			for(CPtrNode *p=instances.first;p;p=p->next){
				ObjectInst *inst=(ObjectInst*)p->item;
				if(inst->m_isDeleted || IsPawnWorldObject(inst) || PawnModelId(inst->m_objectId)!=model) continue;
				ObjectDef *obj=GetObjectDef(inst->m_objectId);
				if(!obj || !obj->m_colModel) continue;
				auto &box=obj->m_colModel->boundingBox;
				rw::V3d choices[3]={box.min,rw::scale(rw::add(box.min,box.max),.5f),box.max};
				rw::V3d point={choices[clamp(x,0,2)].x,choices[clamp(y,0,2)].y,choices[clamp(z,0,2)].z},screen;
				rw::V3d::transformPoints(&point,&point,1,&inst->m_matrix);
				float sw,sh;
				if(Sprite::CalcScreenCoors(point,&screen,&sw,&sh,false)) return (int)(vertical?screen.y:screen.x);
			}
		}
		return -1000;
	}
	// td:320 is that far across (or down) TextDraw mode's canvas; tdruler, on its ruler.
	if(s.compare(0, 3, "td:") == 0)
		return (int)(TextDrawCanvasToScreen((float)atof(s.c_str() + 3), vertical, false) + 0.5f);
	if(s == "tdruler")
		return (int)TextDrawCanvasToScreen(0.0f, vertical, true);
	return atoi(s.c_str());
}

// Chord "Ctrl+Shift+H": the modifiers and the key.
static bool
parseChord(const std::string &text, std::vector<int> &keys)
{
	keys.clear();
	size_t start = 0;
	std::string t = text;
	while(start <= t.size()){
		size_t plus = t.find('+', start);
		// A trailing "+" is the plus key itself ("Ctrl++" or "Num+").
		if(plus == t.size() - 1 && plus > start && (t[plus-1] == '+' || t[plus-1] == 'm'))
			plus = std::string::npos;
		std::string part = t.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
		if(part.empty() && plus != std::string::npos){
			part = "+";
			plus++;
		}
		int key = part == "+" ? sk::KEY_PADADD : keyFromName(part);
		if(key < 0)
			return false;
		keys.push_back(key);
		if(plus == std::string::npos)
			break;
		start = plus + 1;
	}
	return !keys.empty();
}

// A shortcut-table action by id ("duplicate") or name ("Duplicate").
static bool
shortcutChordText(const std::string &name, std::string &chord)
{
	for(int a = 0; a < SC_NUM_ACTIONS; a++){
		const ShortcutInfo *info = GetShortcutInfo(a);
		if(info == nil || (rw::strcmp_ci(info->id, name.c_str()) != 0 && rw::strcmp_ci(info->name, name.c_str()) != 0))
			continue;
		char buf[64];
		ShortcutChordName(GetShortcutChord(a, 0), buf, sizeof(buf));
		chord = buf;
		return !chord.empty();
	}
	return false;
}

static int
countObjects(void)
{
	int n = 0;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst && !inst->m_isDeleted && !IsPawnWorldObject(inst) && IsPawnObjectEditable(inst))
			n++;
	}
	return n;
}

static int
countSelected(void)
{
	int n = 0;
	for(CPtrNode *p = selection.first; p; p = p->next)
		n++;
	return n;
}

static bool
compare(double a, const std::string &op, double b)
{
	if(op == "==") return fabs(a - b) < 0.001;
	if(op == "!=") return fabs(a - b) >= 0.001;
	if(op == ">") return a > b;
	if(op == ">=") return a >= b;
	if(op == "<") return a < b;
	if(op == "<=") return a <= b;
	if(op == "~") return fabs(a - b) < 0.05;
	return false;
}

static std::string
pawnText(bool streamer, bool xml = false)
{
	std::string code;
	if(xml)BeginMapXMLExport();
	BuildPawnCode(GetPawnMapPath(), streamer ? PAWN_CODE_STREAMER : PAWN_CODE_NATIVE, false, code, nil);
	return xml ? EditorXML::Serialize(FinishMapXMLExport()) : code;
}

static bool
windowOpen(const std::string &title)
{
	ImGuiContext &g = *GImGui;
	for(int i = 0; i < g.Windows.Size; i++){
		ImGuiWindow *w = g.Windows[i];
		// A panel in an inactive dock tab is still open.
		bool open = !w->Hidden;
#ifdef IMGUI_HAS_DOCK
		open |= w->DockIsActive;
#endif
		if(w->WasActive && open && nameMatches(w->Name, title, false))
			return true;
	}
	return false;
}

// The map's objects in the map's order.
static std::vector<ObjectInst*>
mapObjectsInOrder(void)
{
	std::vector<ObjectInst*> objs;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isAdded && !inst->m_isDeleted) objs.push_back(inst);
	}
	std::stable_sort(objs.begin(), objs.end(), [](ObjectInst *x, ObjectInst *y){ return x->m_iplIndex < y->m_iplIndex; });
	return objs;
}

// The basis the game gives an object rotated by rx, ry, rz degrees (SA-MP's
// SetObjectRot is opcode 0453 -> CPlaceable::SetOrientation ->
// CMatrix::SetRotate, gta-reversed Core/Matrix.cpp): R = Rz * Rx * Ry.
static void
gameObjectBasis(float rx, float ry, float rz, rw::V3d *right, rw::V3d *up, rw::V3d *at)
{
	float d = 3.14159265358979f/180.0f;
	float sx = sinf(rx*d), cx = cosf(rx*d), sy = sinf(ry*d), cy = cosf(ry*d), sz = sinf(rz*d), cz = cosf(rz*d);
	*right = { cz*cy - sz*sx*sy, cz*sx*sy + sz*cy, -sy*cx };
	*up = { -sz*cx, cz*cx, sx };
	*at = { cz*sy + sz*sx*cy, sz*sy - cz*sx*cy, cy*cx };
}

// keycast_contains / keycast_lacks "text": what the pressed-keys overlay shows now.
static bool
keycastAssert(const std::string &what, const std::string &text, std::string &why, bool *result)
{
	if(what != "keycast_contains" && what != "keycast_lacks")
		return false;
	std::string shown = KeycastShownText();
	why = "keycast shows \"" + shown + "\"";
	bool found = shown.find(text) != std::string::npos;
	*result = what == "keycast_contains" ? found : !found;
	return true;
}

// Exercise the same geometry-copy path used by animated skins and per-instance
// vehicle materials under an unrelated scene dictionary. This catches lookups
// and disk reads that the final material replacement would otherwise hide.
rw::Geometry *CloneInstanceGeometry(rw::Geometry *source);
static rw::Texture *(*gCloneProbeFind)(const char *name);
static rw::Texture *(*gCloneProbeRead)(const char *name, const char *mask);
static int gCloneProbeFinds, gCloneProbeReads;

static rw::Texture*
cloneProbeFind(const char *name)
{
	gCloneProbeFinds++;
	return gCloneProbeFind(name);
}

static rw::Texture*
cloneProbeRead(const char *name, const char *mask)
{
	gCloneProbeReads++;
	return gCloneProbeRead(name, mask);
}

static std::vector<rw::Texture*>
geometryTextures(rw::Geometry *geometry)
{
	std::vector<rw::Texture*> textures;
	auto add = [&](rw::Texture *texture){ if(texture) textures.push_back(texture); };
	for(int i = 0; i < geometry->matList.numMaterials; i++){
		rw::Material *material = geometry->matList.materials[i];
		if(material == nil) continue;
		add(material->texture);
		rw::MatFX *fx = rw::MatFX::get(material);
		if(fx == nil) continue;
		for(int j = 0; j < 2; j++)
			switch(fx->fx[j].type){
			case rw::MatFX::BUMPMAP: add(fx->fx[j].bump.bumpedTex); add(fx->fx[j].bump.tex); break;
			case rw::MatFX::ENVMAP: add(fx->fx[j].env.tex); break;
			case rw::MatFX::DUAL: add(fx->fx[j].dual.tex); break;
			}
	}
	return textures;
}

static bool
checkGeometryCloneTextures(ObjectInst *inst, std::string &why)
{
	ObjectDef *def = inst ? GetObjectDef(inst->m_objectId) : nil;
	if(def == nil || inst->m_rwObject == nil){ why = "no selected rendered model"; return false; }
	std::vector<rw::Geometry*> geometries;
	if(def->m_type == ObjectDef::ATOMIC)
		geometries.push_back(((rw::Atomic*)inst->m_rwObject)->geometry);
	else if(def->m_type == ObjectDef::CLUMP){
		rw::Clump *clump = (rw::Clump*)inst->m_rwObject;
		FORLIST(lnk, clump->atomics)
			geometries.push_back(rw::Atomic::fromClump(lnk)->geometry);
	}
	int checked = 0;
	for(rw::Geometry *source : geometries){
		if(source == nil) continue;
		std::vector<rw::Texture*> textures = geometryTextures(source);
		if(textures.empty()) continue;
		std::vector<int32> refs;
		std::vector<rw::TexDictionary*> dictionaries;
		for(rw::Texture *texture : textures){
			refs.push_back(texture->refCount);
			dictionaries.push_back(texture->dict);
		}
		rw::TexDictionary *previous = rw::TexDictionary::getCurrent();
		rw::TexDictionary *decoy = rw::TexDictionary::create();
		rw::Texture *sentinel = rw::Texture::create(nil);
		strcpy(sentinel->name, "__clone_scene_probe__");
		decoy->add(sentinel);
		rw::TexDictionary::setCurrent(decoy);
		bool ok = true;
		for(int pass = 0; pass < 2 && ok; pass++){
			if(pass == 1){
				// Also protect the source from same-name textures in the scene.
				rw::Texture *collision = rw::Texture::create(nil);
				memcpy(collision->name, textures.front()->name, sizeof(collision->name));
				decoy->add(collision);
			}
			int count = decoy->count(), allocated = rw::Texture::numAllocated;
			uint32 flags = source->flags;
			gCloneProbeFind = rw::Texture::findCB;
			gCloneProbeRead = rw::Texture::readCB;
			gCloneProbeFinds = gCloneProbeReads = 0;
			rw::Texture::findCB = cloneProbeFind;
			rw::Texture::readCB = cloneProbeRead;
			rw::Geometry *copy = CloneInstanceGeometry(source);
			bool restored = rw::Texture::findCB == cloneProbeFind && rw::Texture::readCB == cloneProbeRead;
			rw::Texture::findCB = gCloneProbeFind;
			rw::Texture::readCB = gCloneProbeRead;
			bool sameTextures = copy && geometryTextures(copy) == textures;
			bool sameShape = copy && copy->numVertices == source->numVertices && copy->numTriangles == source->numTriangles;
			if(copy) copy->destroy();
			bool sameOwners = true;
			for(size_t i = 0; i < textures.size(); i++)
				if(textures[i]->refCount != refs[i] || textures[i]->dict != dictionaries[i]) sameOwners = false;
			ok = sameTextures && sameShape && sameOwners && restored && source->flags == flags &&
				rw::TexDictionary::getCurrent() == decoy && decoy->count() == count &&
				rw::Texture::numAllocated == allocated && gCloneProbeFinds == 0 && gCloneProbeReads == 0;
			if(!ok){
				char message[256];
				snprintf(message, sizeof(message),
					"geometry %d pass %d: %d scene lookups, %d disk reads, dictionary %d -> %d, textures preserved %d, owners preserved %d",
					checked, pass, gCloneProbeFinds, gCloneProbeReads, count, decoy->count(), sameTextures, sameOwners);
				why = message;
			}
		}
		rw::TexDictionary::setCurrent(previous);
		decoy->destroy();
		if(!ok) return false;
		checked++;
	}
	why = std::to_string(checked) + " textured geometries cloned without scene lookups, disk reads or texture changes";
	return checked > 0;
}

// assert objects|selected|textdraws|textdraw_zoom|radar_zones <op> N
// assert selected.model|selected.x|selected.y|selected.z <op> N
// assert selected.texture <slot> "name"
// assert pawn_contains "text" [streamer]   (and pawn_lacks)
// assert pawn_count "text" <op> N             (occurrences in the native Pawn)
// assert toast_contains "text"
// assert keycast_contains "text"   (and keycast_lacks: what the pressed-keys overlay shows now)
// assert file_contains <path> "text"   (and file_lacks)
// assert window "Title"     / no_window "Title"
// assert window_docked|window_x|window_y|window_width|window_height "Title" <op> N
// assert textdraw_canvas_right <op> N
// assert item 'Window' 'Label'   / no_item
// assert item_fully_visible 'Window' 'Label'   (bounds stay inside the clip rectangle)
// assert hidden|player_view_hidden|fov|ui_scale|camera_moved <op> N   (camera_moved: metres since mark_camera)
// assert camera_target.x|y|z <op> N   (the point the camera looks at)
// assert camera_position.x|y|z <op> N
// assert train.cars|train.off_track|train.N.x|y|z|model <op> N   (the selected engine's train in game)
// assert drawn_rotation N|selected RX RY RZ   (the map's Nth object, from 0, is drawn as CreateObject rotates it)
static bool
uiAssert(const std::vector<std::string> &a, size_t first, std::string &why)
{
	if(a.size() <= first){
		why = "nothing to assert";
		return false;
	}
	const std::string &what = a[first];
	auto arg = [&](size_t k) -> std::string { return a.size() > first + k ? a[first + k] : std::string(); };
	ObjectInst *sel = selection.first ? (ObjectInst*)selection.first->item : nil;
	char buf[256];
	bool result;
	if(what == "geometry_clone_textures")
		return checkGeometryCloneTextures(sel, why);
	if(keycastAssert(what, arg(1), why, &result))
		return result;
	if(what == "placing"){
		why = gPlaceMode ? "placing" : "not placing";
		return compare(gPlaceMode ? 1 : 0, arg(1), atof(arg(2).c_str()));
	}
	if(what == "fog_enabled"){
		why = gEnableFog ? "fog enabled" : "fog disabled";
		return compare(gEnableFog ? 1 : 0, arg(1), atof(arg(2).c_str()));
	}
	if(what == "new_dot"){
		// new_dot textdraws|code <op> <n>: 1 while that editor's "new" dot shows.
		int shown = WorkspaceNewDotShown(arg(1) == "code" ? 1 : 0) ? 1 : 0;
		why = "new_dot " + arg(1) + " is " + std::to_string(shown);
		return compare(shown, arg(2), atof(arg(3).c_str()));
	}
	if(what == "missing_catalog_glyphs"){
		// Baked at start-up, not just loadable: the renderer can't add glyphs
		// later, so one ImGui loads on demand would still draw blank. A glyph
		// counts when a source of the UI font both covers it in the ranges it
		// was baked with and has it.
		std::vector<unsigned int> codepoints;
		I18nCatalogCodepoints(codepoints);
		ImFontAtlas *atlas = ImGui::GetIO().Fonts;
		ImFont *font = atlas->Fonts[0];
#if IMGUI_VERSION_NUM >= 19200
		ImFontBaked *baked = font->GetFontBaked(font->LegacySize);
		const ImVector<ImFontConfig*> &sources = font->Sources;
#else
		ImFont *baked = font;
		ImVector<ImFontConfig*> sources;
		for(int i = 0; i < font->SourcesCount; i++) sources.push_back((ImFontConfig*)&font->Sources[i]);
#endif
		auto inRanges = [&](unsigned int cp){
			for(ImFontConfig *src : sources){
				const ImWchar *r = src->GlyphRanges ? src->GlyphRanges : atlas->GetGlyphRangesDefault();
				for(; r[0]; r += 2)
					if(cp >= r[0] && cp <= r[1])
						return true;
			}
			return false;
		};
		int count = 0;
		for(unsigned int cp : codepoints){
			if(cp < 0x20)
				continue;
			if(!inRanges(cp) || baked->FindGlyphNoFallback((ImWchar)cp) == nil){
				say("  missing catalog glyph U+%04X", cp);
				count++;
			}
		}
		why = "missing catalog glyphs: " + std::to_string(count);
		return compare(count, arg(1), atof(arg(2).c_str()));
	}
	if(what=="particles.model" || what=="particles.model_live" || what=="particles.model_drawn"){
		int kind=what=="particles.model"?0:what=="particles.model_live"?1:2;
		int count=ParticlePreview::CountForModel(atoi(arg(1).c_str()),kind);
		why=what+" is "+std::to_string(count);
		return compare(count,arg(2),atof(arg(3).c_str()));
	}
	if(what.compare(0,10,"particles.") == 0){
		const auto &stats=ParticlePreview::GetStats();
		int value=what=="particles.systems"?stats.systems:what=="particles.live"?stats.live:
			what=="particles.drawn"?stats.drawn:what=="particles.missing"?stats.missing:stats.definitions;
		why=what+" is "+std::to_string(value);
		return compare(value,arg(1),atof(arg(2).c_str()));
	}
	if(what == "frame_limit" || what == "vsync" || what == "render_fog_start"){
		double value = what == "frame_limit" ? gFrameLimit : what == "vsync" ? (gVSync ? 1 : 0) : TheCamera.m_rwcam->fogPlane;
		why = what + " is " + std::to_string(value);
		return compare(value, arg(1), atof(arg(2).c_str()));
	}
	if(what == "render_far_clip"){
		float distance = TheCamera.m_rwcam->farPlane;
		why = "render far clip is " + std::to_string(distance);
		return compare(distance, arg(1), atof(arg(2).c_str()));
	}
	if(what == "rendered.map_model" || what == "rendered.lod_overlaps"){
		std::set<ObjectInst*> visible;
		ForEachVisibleInst([](ObjectInst *inst, void *data){
			((std::set<ObjectInst*>*)data)->insert(inst);
		}, &visible);
		int count = 0;
		if(what == "rendered.map_model"){
			int model = atoi(arg(1).c_str());
			for(ObjectInst *inst : visible)
				if(IsUserMapObject(inst) && PawnModelId(inst->m_objectId) == model)
					count++;
			why = "visible map instances of model " + arg(1) + ": " + std::to_string(count);
			return compare(count, arg(2), atof(arg(3).c_str()));
		}
		for(ObjectInst *inst : visible){
			ObjectInst *parent = inst->m_lod;
			for(int depth = 0; parent && depth < 32; depth++, parent = parent->m_lod)
				if(visible.count(parent)) count++;
		}
		why = "visible LOD overlaps: " + std::to_string(count);
		return compare(count, arg(1), atof(arg(2).c_str()));
	}
	if(what == "objects" || what == "selected" || what == "selected_world" || what == "textdraws" || what == "textdraw_zoom" || what == "radar_zones" ||
	   what == "selected.model" || what == "selected.x" || what == "selected.y" || what == "selected.z"){
		double v = 0;
		if(what == "objects") v = countObjects();
		else if(what == "selected") v = countSelected();
		else if(what == "selected_world"){for(CPtrNode *p=selection.first;p;p=p->next)if(IsPawnWorldObject((ObjectInst*)p->item))v++;}
		else if(what == "textdraws") v = GetTextDraws().size();
		else if(what == "textdraw_zoom") v = TextDrawCanvasZoom();
		else if(what == "radar_zones") v = CountRadarArtZones();
		else if(sel == nil){ why = "nothing selected"; return false; }
		else if(what == "selected.model") v = PawnModelId(sel->m_objectId);
		else if(what == "selected.x") v = sel->m_translation.x;
		else if(what == "selected.y") v = sel->m_translation.y;
		else v = sel->m_translation.z;
		double want = atof(arg(2).c_str());
		snprintf(buf, sizeof(buf), "%s is %g", what.c_str(), v);
		why = buf;
		return compare(v, arg(1), want);
	}
	if(what == "train.cars" || what.compare(0, 6, "train.") == 0){
		// train.cars (how many the selected engine makes), train.N.x|y|z
		// (car N where the client puts it), train.off_track
		if(sel == nil){ why = "nothing selected"; return false; }
		TrainCar cars[TRAIN_MAX_CARS];
		float off = 0.0f;
		int n = ComputeTrainCars(sel->m_objectId, sel->m_translation, GetTrainInstAngle(sel), cars, &off);
		double v = 0;
		if(what == "train.cars") v = n;
		else if(what == "train.off_track") v = off;
		else{
			int index = atoi(what.c_str() + 6);
			size_t dot = what.find('.', 6);
			std::string field = dot == std::string::npos ? "" : what.substr(dot + 1);
			if(index < 0 || index >= n){ why = "no such train car"; return false; }
			v = field == "x" ? cars[index].position.x : field == "y" ? cars[index].position.y :
				field == "model" ? cars[index].model : cars[index].position.z;
		}
		snprintf(buf, sizeof(buf), "%s is %g", what.c_str(), v);
		why = buf;
		return compare(v, arg(1), atof(arg(2).c_str()));
	}
	if(what.compare(0, 9, "textdraw.") == 0){
		// textdraw.N.x|y|letter_x|letter_y|size_x|size_y|locked
		std::vector<TextDraw> &tds = GetTextDraws();
		size_t dot = what.find('.', 9);
		int index = atoi(what.c_str() + 9);
		std::string field = dot == std::string::npos ? "" : what.substr(dot + 1);
		if(index < 0 || index >= (int)tds.size()){ why = "no such textdraw"; return false; }
		const TextDraw &td = tds[index];
		if(field == "server_txd"){
			why = "server TXD: " + td.serverTxd;
			return td.serverTxd == arg(1);
		}
		if(field == "texture_loaded"){
			bool loaded = TextDrawSpriteTextureAvailable(td);
			why = loaded ? "sprite texture loaded" : "sprite texture missing";
			return compare(loaded ? 1 : 0, arg(1), atof(arg(2).c_str()));
		}
		double v = field == "x" ? td.x : field == "y" ? td.y : field == "letter_x" ? td.letterX :
			field == "letter_y" ? td.letterY : field == "size_x" ? td.sizeX : field == "locked" ? (double)td.locked : td.sizeY;
		snprintf(buf, sizeof(buf), "%s is %g", what.c_str(), v);
		why = buf;
		return compare(v, arg(1), atof(arg(2).c_str()));
	}
	if(what == "file_contains" || what == "file_lacks"){
		std::string text;
		FILE *f = fopen(arg(1).c_str(), "rb");
		if(f){
			char b[4096];
			size_t n;
			while((n = fread(b, 1, sizeof(b), f)) > 0) text.append(b, n);
			fclose(f);
		}
		bool found = text.find(arg(2)) != std::string::npos;
		why = f ? (found ? "found" : "not in the file") : "can't read " + arg(1);
		return f && (what == "file_contains" ? found : !found);
	}
	if(what == "drawn_rotation"){
		ObjectInst *inst = sel;
		if(arg(1) != "selected"){
			std::vector<ObjectInst*> objs = mapObjectsInOrder();
			int index = atoi(arg(1).c_str());
			inst = index >= 0 && index < (int)objs.size() ? objs[index] : nil;
		}
		if(inst == nil){ why = "no such object"; return false; }
		rw::V3d want[3];
		gameObjectBasis(atof(arg(2).c_str()), atof(arg(3).c_str()), atof(arg(4).c_str()), &want[0], &want[1], &want[2]);
		const rw::Matrix &m = inst->m_matrix;
		const rw::V3d got[3] = { m.right, m.up, m.at };
		float err = 0;
		for(int i = 0; i < 3; i++)
			err = std::max(err, std::max(fabsf(got[i].x - want[i].x), std::max(fabsf(got[i].y - want[i].y), fabsf(got[i].z - want[i].z))));
		snprintf(buf, sizeof(buf), "drawn basis is off by %g (right %g %g %g, up %g %g %g, at %g %g %g)", err,
			got[0].x, got[0].y, got[0].z, got[1].x, got[1].y, got[1].z, got[2].x, got[2].y, got[2].z);
		why = buf;
		return err <= 2e-5f;
	}
	if(what == "gizmo_hovered"){
		why = gGizmoHovered ? "the gizmo is hovered" : "the gizmo isn't hovered";
		return gGizmoHovered;
	}
	if(what == "proxy_draws"){
		// Labels, sounds, map icons, gang zones and path points drawn with a
		// model this frame instead of their editor icon (should be 0).
		double v = GetElementProxyModelDraws();
		why = "proxy_draws is " + std::to_string((int)v);
		return compare(v, arg(1), atof(arg(2).c_str()));
	}
	if(what == "gizmo"){
		std::string mode = gGizmoMode == GIZMO_ROTATE ? "rotate" : "move";
		why = "gizmo is " + mode;
		return mode == arg(1);
	}
	if(what == "hidden" || what == "player_view_hidden" || what == "fov" || what == "ui_scale" ||
	   what == "camera_moved" || what == "camera_target.x" || what == "camera_target.y" ||
	   what == "camera_target.z" || what == "camera_position.x" || what == "camera_position.y" ||
	   what == "camera_position.z" || what == "camera_up.z" || what == "camera_pitch"){
		double v = 0;
		if(what == "hidden"){
			v = CountHiddenInstances();
		}else if(what == "player_view_hidden"){
			for(CPtrNode *p = instances.first; p; p = p->next)
				if(PlayerViewHides((ObjectInst*)p->item))
					v++;
		}else if(what == "fov") v = TheCamera.m_fov;
		else if(what == "ui_scale") v = GetUiScale();
		else if(what == "camera_up.z") v = TheCamera.m_localup.z;	// the picture's up: below 0 is upside down
		else if(what == "camera_pitch"){
			rw::V3d d = rw::sub(TheCamera.m_target, TheCamera.m_position);
			v = atan2(d.z, sqrt(d.x * d.x + d.y * d.y)) * 180.0 / 3.14159265358979;
		}
		else if(what == "camera_target.x") v = TheCamera.m_target.x;
		else if(what == "camera_target.y") v = TheCamera.m_target.y;
		else if(what == "camera_target.z") v = TheCamera.m_target.z;
		else if(what == "camera_position.x") v = TheCamera.m_position.x;
		else if(what == "camera_position.y") v = TheCamera.m_position.y;
		else if(what == "camera_position.z") v = TheCamera.m_position.z;
		else v = rw::length(rw::sub(TheCamera.m_position, gMarkedCamera));
		snprintf(buf, sizeof(buf), "%s is %g", what.c_str(), v);
		why = buf;
		return compare(v, arg(1), atof(arg(2).c_str()));
	}
	if(what == "discovery_count"){
		int count = BrowserDiscoveryCount(atoi(arg(1).c_str()));
		why = "instance count is " + std::to_string(count);
		return compare(count, arg(2), atof(arg(3).c_str()));
	}
	if(what == "discovery_highlights"){
		int count = 0;
		for(CPtrNode *p = instances.first; p; p = p->next)
			if(((ObjectInst*)p->item)->m_highlight == HIGHLIGHT_MATCH) count++;
		why = "yellow highlight count is " + std::to_string(count);
		return compare(count, arg(1), atof(arg(2).c_str()));
	}
	if(what == "snap_candidate"){
		std::vector<ObjectInst*> objs;
		std::vector<rw::V3d> positions;
		for(CPtrNode *p=selection.first;p;p=p->next){
			ObjectInst *inst=(ObjectInst*)p->item;
			objs.push_back(inst); positions.push_back(inst->m_translation);
		}
		rw::V3d correction;
		bool found=FindSelectionSnapDelta(objs,positions,{(float)atof(arg(1).c_str()),(float)atof(arg(2).c_str()),(float)atof(arg(3).c_str())},&correction);
		why=found?"snap target found":"no snap target";
		return compare(found,arg(4),atof(arg(5).c_str()));
	}
	if(what=="layout.count"||what=="layout.objects"||what=="layout.point_z"||what=="layout.matches"||what=="layout.upside_down"||what=="duplicates"||what=="view.bounds"||what=="gizmo.pivot"){
		double value=what=="layout.count"?LayoutPathCount():what=="layout.objects"?LayoutPathObjectCount():
			what=="layout.matches"?LayoutPathObjectsMatch():what=="layout.upside_down"?LayoutPathUpsideDownCount():
			what=="layout.point_z"?LayoutPathPointHeight(atoi(arg(1).c_str())):what=="duplicates"?MapCleanupDuplicateCount():what=="view.bounds"?gRenderBounds:gGizmoPivot;
		int offset=what=="layout.point_z"?1:0;
		why=what+" is "+std::to_string(value);
		return compare(value,arg(offset+1),atof(arg(offset+2).c_str()));
	}
	if(what == "line.points" || what == "line.markers" || what == "line.picking" || what == "line.cursor"){
		const auto &feedback = GetMapToolsLineFeedback();
		int value = what == "line.points" ? feedback.points : what == "line.markers" ? feedback.markers :
			what == "line.picking" ? feedback.picking : feedback.cursorVisible;
		why = what + " is " + std::to_string(value);
		return compare(value, arg(1), atof(arg(2).c_str()));
	}
	if(what.compare(0, 4, "pin.") == 0){
		rw::V3d pin, end;
		bool placed = CoordinatePinPosition(&pin), measured = CoordinatePinPosition(&pin, &end);
		rw::V3d d = rw::sub(end, pin);
		auto angles=Alignment::Angles({d.x,d.y,d.z});
		double value = what == "pin.count" ? CoordinatePinCount() :
			what == "pin.snap_model" ? CoordinatePinSnapModel() :
			what == "pin.anchor_snapped" ? CoordinatePinAnchorSnapped() :
			what == "pin.heading" ? angles[1] : what == "pin.pitch" ? angles[2] :
			what == "pin.axis_aligned" ? ((fabsf(d.x)<.001f)+(fabsf(d.y)<.001f)+(fabsf(d.z)<.001f)>=2) :
			what == "pin.angle_stepped" ? (fabsf(remainderf(angles[1],15))<.15f) :
			what == "pin.placed" ? placed : what == "pin.measured" ? measured :
			what == "pin.x" ? pin.x : what == "pin.y" ? pin.y : what == "pin.z" ? pin.z :
			what == "pin.dx" ? d.x : what == "pin.dy" ? d.y : what == "pin.dz" ? d.z : rw::length(d);
		why = what + " is " + std::to_string(value);
		if(what == "pin.angle_stepped") why += "; heading=" + std::to_string(angles[1]) + "; length=" + std::to_string(angles[0]);
		return compare(value, arg(1), atof(arg(2).c_str()));
	}
	if(what == "clipboard_contains"){
		const char *text = ImGui::GetClipboardText();
		why = text ? text : "";
		return why.find(arg(1)) != std::string::npos;
	}
	if(what == "selected.texture"){
		if(sel == nil){ why = "nothing selected"; return false; }
		const ObjectMaterialOverride *m = GetObjectMaterialOverride(sel, atoi(arg(1).c_str()));
		std::string tex = m && m->active ? m->textureName : "";
		why = "slot texture is \"" + tex + "\"";
		return tex == arg(2);
	}
	if(what == "pawn_count"){
		std::string text = pawnText(false), needle = arg(1);
		int count = 0;
		for(size_t at = needle.empty() ? std::string::npos : text.find(needle); at != std::string::npos;
		    at = text.find(needle, at + needle.size()))
			count++;
		why = "\"" + needle + "\" occurs " + std::to_string(count) + " time(s)";
		return compare(count, arg(2), atof(arg(3).c_str()));
	}
 if(what == "ide_contains" || what == "ide_lacks"){
  bool found = PawnEditorText().find(arg(1)) != std::string::npos;
  why = found ? "found" : "not found";
  return what == "ide_contains" ? found : !found;
 }
 if(what == "ide_dirty") return compare(PawnEditorHasUnsavedChanges() ? 1 : 0, arg(1), atof(arg(2).c_str()));

	if(what == "game_problem_contains"){
		CheckGameDir(GetSuggestedGameDir(), &why);
		return why.find(arg(1)) != std::string::npos;
	}
	if(what == "game_folder_contains"){
		why = GetSuggestedGameDir();
		return why.find(arg(1)) != std::string::npos;
	}

	if(what == "pawn_contains" || what == "pawn_lacks" || what == "xml_contains" || what == "xml_lacks"){
		std::string text = pawnText(arg(2) == "streamer", what.compare(0, 3, "xml") == 0);
		bool found = text.find(arg(1)) != std::string::npos;
		why = found ? "found" : "not found";
		return what == "pawn_contains" || what == "xml_contains" ? found : !found;
	}
	if(what == "multiplayer_lobby"){	// signed in to a server with rooms, picking one
		why = "multiplayer state " + std::to_string(GetMultiplayerState());
		return GetMultiplayerState() == MP_LOBBY;
	}
	if(what == "multiplayer_ready"){	// in a session, with someone else in it
		why = "multiplayer state " + std::to_string(GetMultiplayerState()) + ", " +
			std::to_string(GetMultiplayerPeerCount()) + " others";
		return (GetMultiplayerState() == MP_JOINED || GetMultiplayerState() == MP_HOSTING) && GetMultiplayerPeerCount() > 0;
	}
	if(what == "terminal_contains" || what == "terminal_lacks"){
		std::string text = TerminalScreenText();
		bool found = text.find(arg(1)) != std::string::npos;
		why = "the terminal shows:\n" + text;
		return what == "terminal_contains" ? found : !found;
	}
	if(what == "toast_contains"){
		why = "last toast: " + gLastToast;
		return gLastToast.find(arg(1)) != std::string::npos;
	}
	if(what == "textdraw_canvas_right"){
		float right = TextDrawCanvasToScreen(640.0f, false, false);
		why = "textdraw_canvas_right is " + std::to_string(right);
		return compare(right, arg(1), atof(arg(2).c_str()));
	}
	if(what == "window_docked" || what == "window_x" || what == "window_y" || what == "window_width" || what == "window_height"){
		for(ImGuiWindow *window : GImGui->Windows){
			if(!window->WasActive || ((window->Flags & ImGuiWindowFlags_ChildWindow) && !window->DockIsActive) ||
			   (arg(1) != window->Name && !nameMatches(window->Name, arg(1), false))) continue;
			double value = what == "window_docked" ? window->DockIsActive :
				what == "window_x" ? window->Pos.x : what == "window_y" ? window->Pos.y :
				what == "window_width" ? window->Size.x : window->Size.y;
			snprintf(buf, sizeof(buf), "%s '%s' is %.1f", what.c_str(), arg(1).c_str(), value);
			why = buf;
			return compare(value, arg(2), atof(arg(3).c_str()));
		}
		why = "no such active window: " + arg(1);
		return false;
	}
	if(what == "clipped_items"){
		// clipped_items <op> <n>: labelled items on screen whose box runs past the
		// side of their window (a label that grew, in a language with longer words).
		int count = 0;
		for(const UiItem &it : gPrevItems){
			if(it.label.empty() || it.clip.GetWidth() <= 0 || it.bb.GetWidth() <= 0)
				continue;
			bool seenRow = it.bb.Min.y >= it.clip.Min.y && it.bb.Max.y <= it.clip.Max.y;
			if(seenRow && (it.bb.Max.x > it.clip.Max.x + 1.0f || it.bb.Min.x < it.clip.Min.x - 1.0f)){
				say("  clipped '%s' in '%s' (%.0f..%.0f of %.0f..%.0f)", it.label.c_str(), it.window.c_str(),
					it.bb.Min.x, it.bb.Max.x, it.clip.Min.x, it.clip.Max.x);
				count++;
			}
		}
		snprintf(buf, sizeof(buf), "clipped_items is %d", count);
		why = buf;
		return compare(count, arg(1), atof(arg(2).c_str()));
	}
	if(what == "work_area_top"){
		ImGuiViewport *vp = ImGui::GetMainViewport();
		double top = vp->WorkPos.y - vp->Pos.y;
		why = "work area starts at " + std::to_string(top);
		return compare(top, arg(1), atof(arg(2).c_str()));
	}
	if(what == "item_count"){
		int count = 0;
		for(const UiItem &it : gPrevItems)
			if(nameMatches(it.rawLabel, arg(1), false) || (it.label.empty() && idIs(it, arg(1)))) count++;
		why = "item count is " + std::to_string(count);
		return compare(count, arg(2), atof(arg(3).c_str()));
	}

	if(what == "window" || what == "no_window"){
		bool open = windowOpen(arg(1));
		why = open ? "open" : "not open";
		return what == "window" ? open : !open;
	}
	if(what == "item_fully_visible"){
		const UiItem *item = findItem(arg(1), arg(2));
		if(!item){ why = "item not found"; return false; }
		bool fits = item->bb.Min.x >= item->clip.Min.x - 1.0f && item->bb.Min.y >= item->clip.Min.y - 1.0f &&
			item->bb.Max.x <= item->clip.Max.x + 1.0f && item->bb.Max.y <= item->clip.Max.y + 1.0f;
		snprintf(buf, sizeof(buf), "%s: item (%.0f,%.0f..%.0f,%.0f), clip (%.0f,%.0f..%.0f,%.0f)",
			arg(2).c_str(), item->bb.Min.x, item->bb.Min.y, item->bb.Max.x, item->bb.Max.y,
			item->clip.Min.x, item->clip.Min.y, item->clip.Max.x, item->clip.Max.y);
		why = buf;
		return fits;
	}
	if(what == "item" || what == "no_item"){
		bool found = findItem(arg(1), arg(2)) != nil;
		why = found ? "found" : "not found";
		return what == "item" ? found : !found;
	}
	why = "unknown assert " + what;
	return false;
}

// Low-level steps a command expands into, one per frame.
struct Step {
	enum { MOVE, DOWN, UP, KEYDOWN, KEYUP, CHAR, WHEEL, ITEM, NOP } type;
	int x, y, value;
	std::string window, label;
	bool right;
	int nth;
};
static std::vector<Step> gSteps;
static size_t gStep;
static int gWaitFrames;
static std::string gScrollWindow, gScrollLabel;
static int gScrollFrames;
static std::vector<std::string> gWaitUntil;
static int gWaitUntilFrames;
static std::string gGrabPath;
static int gDragItemDx, gDragItemDy;
static bool gDragItem;
static std::string gDragToWindow, gDragToLabel;	// drag_item_to: the item to drop on

static bool
runStep(const Step &s)
{
	switch(s.type){
	case Step::MOVE: gMouseX = s.x; gMouseY = s.y; sendMouse(); break;
	case Step::DOWN: gButtons |= s.value; sendMouse(); break;
	case Step::UP: gButtons &= ~s.value; sendMouse(); break;
	case Step::KEYDOWN: sendKey(s.value, true); break;
	case Step::KEYUP: sendKey(s.value, false); break;
	case Step::CHAR: send(sk::CHARINPUT, (void*)(uintptr)s.value); break;
	case Step::WHEEL: {
		sk::MouseState ms = {};
		ms.posx = gMouseX; ms.posy = gMouseY; ms.buttons = gButtons; ms.wheelDelta = (float)s.value;
		send(sk::MOUSEWHEEL, &ms);
		break;
	}
	case Step::ITEM: {
		const UiItem *item = findItem(s.window, s.label, s.nth ? s.nth : 1);
		if(item == nil){
			say("line %d: FAIL no item '%s' in window '%s'", (int)gLine, s.label.c_str(), s.window.c_str());
			dumpSeen(s.window);
			gFails++;
			return false;
		}
		gMouseX = (int)((item->bb.Min.x + item->bb.Max.x) * 0.5f);
		gMouseY = (int)((item->bb.Min.y + item->bb.Max.y) * 0.5f);
		say("line %d: '%s' in '%s' at %d, %d", (int)gLine, plainLabel(item->label).c_str(),
			plainLabel(item->window).c_str(), gMouseX, gMouseY);
		sendMouse();
		break;
	}
	case Step::NOP: break;
	}
	return true;
}

static void
addStep(int type, int x = 0, int y = 0, int value = 0)
{
	Step s = {};
	s.type = (decltype(s.type))type;
	s.x = x; s.y = y; s.value = value;
	gSteps.push_back(s);
}

static void
addChord(const std::vector<int> &keys)
{
	for(size_t i = 0; i < keys.size(); i++) addStep(Step::KEYDOWN, 0, 0, keys[i]);
	addStep(Step::NOP);
	for(size_t i = keys.size(); i-- > 0;) addStep(Step::KEYUP, 0, 0, keys[i]);
	addStep(Step::NOP);
}

static void finish(void);

// Turns the next script line into steps, or runs it at once.
static void
nextCommand(void)
{
	while(gLine < gLines.size()){
		std::string line = gLines[gLine++];
		std::vector<std::string> a = tokens(line);
		if(a.empty())
			continue;
		const std::string &cmd = a[0];
		say("line %d: %s", (int)gLine, line.c_str());
		gSteps.clear();
		gStep = 0;
		if(cmd == "wait"){
			gWaitFrames = a.size() > 1 ? atoi(a[1].c_str()) : 1;
			return;
		}else if(cmd == "wait_until"){
			gWaitUntil = a;
			gWaitUntilFrames = 600;
			return;
		}else if(cmd == "key" || cmd == "shortcut" || cmd == "hold" || cmd == "release"){
			std::string chord = a.size() > 1 ? a[1] : "";
			if((cmd == "hold" || cmd == "release") &&
			   (chord == "left" || chord == "right" || chord == "middle")){
				addStep(cmd == "hold" ? Step::DOWN : Step::UP, 0, 0, buttonBit(chord));
				addStep(Step::NOP);
				return;
			}
			if(cmd == "shortcut" && !shortcutChordText(chord, chord)){
				say("line %d: FAIL no shortcut '%s'", (int)gLine, a.size() > 1 ? a[1].c_str() : "");
				gFails++;
				continue;
			}
			std::vector<int> keys;
			if(!parseChord(chord, keys)){
				say("line %d: FAIL unknown key '%s'", (int)gLine, chord.c_str());
				gFails++;
				continue;
			}
			if(cmd == "hold") for(int k : keys) addStep(Step::KEYDOWN, 0, 0, k);
			else if(cmd == "release") for(int k : keys) addStep(Step::KEYUP, 0, 0, k);
			else addChord(keys);
			return;
		}else if(cmd == "move" && a.size() >= 3){
			addStep(Step::MOVE, coordinate(a[1], false), coordinate(a[2], true));
			return;
		}else if(cmd == "click"){
			int bit = buttonBit(a.size() == 2 ? a[1] : a.size() >= 4 ? a[3] : "left");
			if(a.size() >= 3)
				addStep(Step::MOVE, coordinate(a[1], false), coordinate(a[2], true));
			addStep(Step::NOP);
			addStep(Step::DOWN, 0, 0, bit);
			addStep(Step::UP, 0, 0, bit);
			addStep(Step::NOP);
			return;
		}else if(cmd == "drag" && a.size() >= 5){
			int x1 = coordinate(a[1], false), y1 = coordinate(a[2], true);
			int x2 = coordinate(a[3], false), y2 = coordinate(a[4], true);
			int frames = a.size() > 5 ? atoi(a[5].c_str()) : 10;
			addStep(Step::MOVE, x1, y1);
			addStep(Step::DOWN, 0, 0, 1);
			for(int f = 1; f <= frames; f++)
				addStep(Step::MOVE, x1 + (x2 - x1) * f / frames, y1 + (y2 - y1) * f / frames);
			addStep(Step::UP, 0, 0, 1);
			addStep(Step::NOP);
			return;
		}else if(cmd == "drag_window" && a.size() >= 4){
			ImGuiWindow *window = nil;
			for(ImGuiWindow *candidate : GImGui->Windows)
				if(candidate->WasActive && (!(candidate->Flags & ImGuiWindowFlags_ChildWindow) || candidate->DockIsActive) &&
				   (a[1] == candidate->Name || nameMatches(candidate->Name, a[1], false))) window = candidate;
			if(!window){ say("line %d: FAIL no window '%s'", (int)gLine, a[1].c_str()); gFails++; continue; }
			ImRect title = window->DockIsActive ? window->DC.DockTabItemRect : window->TitleBarRect();
			int x = (int)title.GetCenter().x, y = (int)title.GetCenter().y;
			int dx = atoi(a[2].c_str()), dy = atoi(a[3].c_str());
			addStep(Step::MOVE, x, y);
			addStep(Step::DOWN, 0, 0, 1);
			for(int f = 1; f <= 20; f++) addStep(Step::MOVE, x + dx * f / 20, y + dy * f / 20);
			for(int f = 0; f < 5; f++) addStep(Step::NOP);
			addStep(Step::UP, 0, 0, 1);
			addStep(Step::NOP);
			return;
		}else if(cmd == "wheel" && a.size() >= 2){
			addStep(Step::WHEEL, 0, 0, atoi(a[1].c_str()));
			addStep(Step::NOP);
			return;
		}else if((cmd == "click_item" || cmd == "hover_item" || cmd == "drag_item") && a.size() >= 3){
			Step s = {};
			s.type = Step::ITEM;
			s.window = a[1];
			s.label = a[2];
			// An optional number picks the Nth item with that label
			// (after dx dy for drag_item).
			for(size_t k = cmd == "drag_item" ? 5 : 3; k < a.size(); k++)
				if(isdigit((unsigned char)a[k][0]) || (a[k][0] == '-' && isdigit((unsigned char)a[k][1])))
					s.nth = atoi(a[k].c_str());
			gSteps.push_back(s);
			addStep(Step::NOP);
			if(cmd == "click_item"){
				int bit = 1;
				for(size_t k = 3; k < a.size(); k++)
					if(a[k] == "right") bit = 4;
				addStep(Step::DOWN, 0, 0, bit);
				addStep(Step::UP, 0, 0, bit);
				addStep(Step::NOP);
			}else if(cmd == "drag_item" && a.size() >= 5){
				gDragItem = true;
				gDragItemDx = atoi(a[3].c_str());
				gDragItemDy = atoi(a[4].c_str());
			}
			return;
		}else if(cmd == "drag_item_to" && a.size() >= 5){
			Step s = {};
			s.type = Step::ITEM;
			s.window = a[1];
			s.label = a[2];
			gSteps.push_back(s);
			addStep(Step::NOP);
			gDragItem = true;
			gDragToWindow = a[3];
			gDragToLabel = a[4];
			return;
		}else if(cmd == "set_item" && a.size() >= 4){
			// set_item 'Window' 'Label' "text" [N] [tab]: double-click the field,
			// select all, type, Enter (Tab instead for a multiline field, where
			// Enter is a new line). Works for text, number, drag and colour fields.
			Step s = {};
			s.type = Step::ITEM;
			s.window = a[1];
			s.label = a[2];
			s.nth = 1;
			bool tab = false, ctrl = false;
			for(size_t k = 4; k < a.size(); k++){
				if(a[k] == "tab") tab = true;
				else if(a[k] == "ctrl") ctrl = true;
				else if(isdigit((unsigned char)a[k][0])) s.nth = atoi(a[k].c_str());
			}
			gSteps.push_back(s);
			addStep(Step::NOP);
			if(ctrl) addStep(Step::KEYDOWN, 0, 0, sk::KEY_LCTRL);
			addStep(Step::DOWN, 0, 0, 1);
			addStep(Step::UP, 0, 0, 1);
			if(ctrl) addStep(Step::KEYUP, 0, 0, sk::KEY_LCTRL);
			else{
				addStep(Step::DOWN, 0, 0, 1);
				addStep(Step::UP, 0, 0, 1);
			}
			addStep(Step::NOP);
			addStep(Step::NOP);
			addChord({ sk::KEY_LCTRL, 'A' });
			for(unsigned char c : a[3])
				addStep(Step::CHAR, 0, 0, c);
			addStep(Step::NOP);
			addChord({ tab ? sk::KEY_TAB : sk::KEY_ENTER });
			return;
		}else if(cmd == "close_popup"){
			// Click the empty end of the menu bar: closes an open combo or menu.
			ImGuiIO &io = ImGui::GetIO();
			addStep(Step::MOVE, (int)io.DisplaySize.x - 4, 6);
			addStep(Step::NOP);
			addStep(Step::DOWN, 0, 0, 1);
			addStep(Step::UP, 0, 0, 1);
			addStep(Step::NOP);
			return;
		}else if(cmd == "reopen"){
			// Open the map file again, as saved.
			std::string path = GetPawnMapPath();
			if(path.empty() || !OpenPawnMapFile(path.c_str())){
				say("line %d: FAIL could not reopen '%s'", (int)gLine, path.c_str());
				gFails++;
			}
			gWaitFrames = 2;
			return;
		}else if(cmd == "click_object" && a.size() >= 2){
			// The Nth object of the map (from 0), clicked where it is on screen.
			// click_object N (newest first) or click_object model=M
			bool byModel = a[1].compare(0, 6, "model=") == 0;
			int want = atoi(a[1].c_str() + (byModel ? 6 : 0)), n = 0;
			ObjectInst *target = nil;
			for(CPtrNode *p = instances.first; p && !target; p = p->next){
				ObjectInst *inst = (ObjectInst*)p->item;
				if(inst == nil || inst->m_isDeleted || IsPawnWorldObject(inst) || !IsPawnObjectEditable(inst))
					continue;
				if(byModel ? PawnModelId(inst->m_objectId) == want : n++ == want)
					target = inst;
			}
			if(target == nil){
				say("line %d: FAIL the map has no object %d", (int)gLine, want);
				gFails++;
				continue;
			}
			if(!gObjectFramed){
				// Look at it from close by first, so nothing stands in front.
				ObjectDef *obj = GetObjectDef(target->m_objectId);
				float r = obj && obj->m_colModel ? std::max(obj->m_colModel->boundingSphere.radius, 1.0f) : 2.0f;
				TheCamera.m_target = target->m_translation;
				TheCamera.m_position = rw::add(target->m_translation, rw::V3d{ -1.6f * r, -1.6f * r, 1.4f * r });
				gObjectFramed = true;
				gLine--;	// run this line again once the view has moved
				gWaitFrames = 3;
				return;
			}
			gObjectFramed = false;
			rw::V3d screen;
			float sw, sh;
			if(!Sprite::CalcScreenCoors(target->m_translation, &screen, &sw, &sh, false)){
				say("line %d: FAIL object %d is not on screen", (int)gLine, want);
				gFails++;
				continue;
			}
			int bit = a.size() > 2 && a[2] == "right" ? 4 : 1;
			say("line %d: object at %.0f, %.0f (window %.0fx%.0f)", (int)gLine, screen.x, screen.y,
				ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
			addStep(Step::MOVE, (int)screen.x, (int)screen.y);
			addStep(Step::NOP);
			addStep(Step::DOWN, 0, 0, bit);
			if(a.size() > 3 && a[3] == "jitter"){
				addStep(Step::MOVE, (int)screen.x + 3, (int)screen.y + 2);
				addStep(Step::NOP);
			}
			addStep(Step::UP, 0, 0, bit);
			addStep(Step::NOP);
			return;
		}else if(cmd == "click_world" && a.size() >= 4){
			// click_world X Y Z [shift|right]: look at the point from close by, then click it,
			// to place an element or pick what is there.
			rw::V3d p = { (float)atof(a[1].c_str()), (float)atof(a[2].c_str()), (float)atof(a[3].c_str()) };
			if(!gObjectFramed){
				TheCamera.m_target = p;
				TheCamera.m_position = rw::add(p, rw::V3d{ -8.0f, -8.0f, 7.0f });
				gObjectFramed = true;
				gLine--;
				gWaitFrames = 3;
				return;
			}
			gObjectFramed = false;
			rw::V3d screen;
			float sw, sh;
			if(!Sprite::CalcScreenCoors(p, &screen, &sw, &sh, false)){
				say("line %d: FAIL %s %s %s is not on screen", (int)gLine, a[1].c_str(), a[2].c_str(), a[3].c_str());
				gFails++;
				continue;
			}
			bool shift = a.size() > 4 && a[4] == "shift";
			int bit = a.size() > 4 && a[4] == "right" ? 4 : 1;
			addStep(Step::MOVE, (int)screen.x, (int)screen.y);
			if(shift)
				addStep(Step::KEYDOWN, 0, 0, sk::KEY_LSHIFT);
			addStep(Step::NOP);
			addStep(Step::DOWN, 0, 0, bit);
			addStep(Step::UP, 0, 0, bit);
			if(shift)
				addStep(Step::KEYUP, 0, 0, sk::KEY_LSHIFT);
			addStep(Step::NOP);
			return;
		}else if(cmd == "scroll_until" && a.size() >= 3){
			gScrollWindow = a[1];
			gScrollLabel = a[2];
			gScrollFrames = 120;
			return;
		}else if(cmd == "dump_items"){
			// What ImGui drew last frame, for writing scripts.
			std::string filter = a.size() > 1 ? a[1] : "";
			for(size_t i = 0; i < gPrevItems.size(); i++){
				const UiItem &it = gPrevItems[i];
				std::string line = "'" + plainLabel(it.window) + "' '" + it.rawLabel + "'";
				if(filter.empty() || line.find(filter) != std::string::npos)
					say("  item %s at %.0f,%.0f", line.c_str(), it.bb.Min.x, it.bb.Min.y);
			}
			continue;
		}else if(cmd == "dump_matrices" && a.size() >= 2){
			// dump_matrices PATH: the matrix each map object is drawn with, in
			// the map's order: model, then right, up, at and pos.
			std::vector<ObjectInst*> objs = mapObjectsInOrder();
			FILE *f = fopen(a[1].c_str(), "w");
			if(f == nil){
				say("line %d: FAIL could not write %s", (int)gLine, a[1].c_str());
				gFails++;
				continue;
			}
			for(ObjectInst *inst : objs){
				const rw::Matrix &m = inst->m_matrix;
				fprintf(f, "%d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", inst->m_objectId,
					m.right.x, m.right.y, m.right.z, m.up.x, m.up.y, m.up.z,
					m.at.x, m.at.y, m.at.z, m.pos.x, m.pos.y, m.pos.z);
			}
			fclose(f);
			say("dumped %d matrices to %s", (int)objs.size(), a[1].c_str());
			continue;
		}else if(cmd == "mark_camera"){
			gMarkedCamera = TheCamera.m_position;
			continue;
		}else if(cmd == "camera" && a.size() >= 7){
			// camera X Y Z TX TY TZ: look at a spot, for click_object.
			TheCamera.m_position = { (float)atof(a[1].c_str()), (float)atof(a[2].c_str()), (float)atof(a[3].c_str()) };
			TheCamera.m_target = { (float)atof(a[4].c_str()), (float)atof(a[5].c_str()), (float)atof(a[6].c_str()) };
			gWaitFrames = 3;
			return;
		}else if(cmd == "clipboard" && a.size() >= 2){
			ImGui::SetClipboardText(a[1].c_str());
			addStep(Step::NOP);
			return;
		}else if(cmd == "type" && a.size() >= 2){
			for(unsigned char c : a[1])
				addStep(Step::CHAR, 0, 0, c);
			addStep(Step::NOP);
			return;
		}else if(cmd == "open" && a.size() >= 2){
			if(!OpenPawnMapFile(a[1].c_str())){
				say("line %d: FAIL could not open %s", (int)gLine, a[1].c_str());
				gFails++;
			}
			gWaitFrames = 2;
			return;
		}else if(cmd == "grab" && a.size() >= 2){
			gGrabPath = a[1];
			gWaitFrames = 2;
			return;
		}else if(cmd == "assert"){
			std::string why;
			if(uiAssert(a, 1, why)){
				gPasses++;
				say("line %d: ok (%s)", (int)gLine, why.c_str());
			}else{
				gFails++;
				say("line %d: FAIL %s", (int)gLine, why.c_str());
			}
			continue;
		}else if(cmd == "log"){
			continue;
		}else if(cmd == "fail"){
			gFails++;
			continue;
		}else if(cmd == "exit"){
			gLine = gLines.size();
			break;
		}else{
			say("line %d: FAIL unknown command '%s'", (int)gLine, cmd.c_str());
			gFails++;
			continue;
		}
	}
	if(gLine >= gLines.size() && gSteps.empty() && gWaitFrames <= 0 && gWaitUntil.empty())
		finish();
}

static void
finish(void)
{
	say("%s: %d assert(s) passed, %d failure(s)", gFails ? "FAILED" : "PASSED", gPasses, gFails);
	if(gLog)
		fclose(gLog);
	fflush(stdout);
	CrashReportsShutdown();	// a finished script is a clean exit, not a crash
	// Skip shutdown: nothing of a test run should be saved.
	_exit(gFails ? 1 : 0);
}

static bool
privateConfig(void)
{
	// Only with a config folder of its own: a script must never change the
	// user's settings, recent maps or shortcuts. HAUMEA_CONFIG_DIR names one
	// directly (Windows and Wine, which don't use XDG_CONFIG_HOME).
	const char *own = getenv("HAUMEA_CONFIG_DIR");
	if(own && own[0])
		return true;
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if(xdg == nil || xdg[0] == '\0')
		return false;
	if(home && (std::string(xdg) == std::string(home) + "/.config" || std::string(xdg) == std::string(home) + "/.config/"))
		return false;
	return true;
}

bool
UiScriptActive(void)
{
	return gActive;
}

void
UiScriptRestateMouse(void)
{
#ifdef UI_FEED_IMGUI
	// Win32's backend polls the real cursor each frame when it gets no mouse
	// messages; the last position given wins.
	if(gActive)
		ImGui::GetIO().AddMousePosEvent((float)gMouseX, (float)gMouseY);
#endif
}

void
UiScriptInit(void)
{
	const char *path = nil;
	for(int i = 1; i + 1 < sk::args.argc; i++)
		if(strcmp(sk::args.argv[i], "--ui-script") == 0)
			path = sk::args.argv[i+1];
	if(path == nil)
		return;
	if(!privateConfig()){
		printf("[ui-script] refusing to run: set XDG_CONFIG_HOME to a private folder\n");
		fflush(stdout);
		_exit(2);
	}
	FILE *f = fopen(path, "r");
	if(f == nil){
		printf("[ui-script] can't read %s\n", path);
		fflush(stdout);
		_exit(2);
	}
	char line[2048];
	while(fgets(line, sizeof(line), f)){
		line[strcspn(line, "\r\n")] = '\0';
		gLines.push_back(line);
	}
	fclose(f);
	// The log goes to the current folder, not next to the script in the source tree.
	const char *base = strrchr(path, '/');
	gLog = fopen((std::string(base ? base + 1 : path) + ".log").c_str(), "w");
	// $UI_CONFIG_DIR: the editor's own config folder, wherever the platform keeps it.
#ifdef _WIN32
	_putenv_s("UI_CONFIG_DIR", GetConfigDir());
#else
	setenv("UI_CONFIG_DIR", GetConfigDir(), 1);
#endif
	gActive = true;
	say("running %s (%d lines)", path, (int)gLines.size());
}

// Once a frame, before the editor reads input.
void
UiScriptTick(void)
{
	static bool inited;
	if(!inited){
		inited = true;
		UiScriptInit();
	}
	if(!gActive)
		return;
	GImGui->TestEngineHookItems = true;
	gPrevItems.swap(gItems);
	gItems.clear();
	gItemIndex.clear();
	gItemLabels.clear();
	gFrame++;
	if(gFrame < 30)
		return;	// let the first frames settle
	static bool seenItems, reported;
	seenItems = seenItems || !gPrevItems.empty();
	if(seenItems && !reported){
		reported = true;
		ImGuiIO &io = ImGui::GetIO();
		say("window %.0fx%.0f, framebuffer scale %.2f, UI scale %.2f",
			io.DisplaySize.x, io.DisplaySize.y, io.DisplayFramebufferScale.x, GetUiScale());
	}
	if(!seenItems && gFrame > 600){
		say("FAIL ImGui reports no items: this editor was built without IMGUI_ENABLE_TEST_ENGINE "
			"; objects built before it was added need rebuilding (touch vendor/imgui-docking/*.cpp)");
		gFails++;
		finish();
	}
	if(!seenItems)
		return;	// still loading: nothing drawn yet
	if(gFrame > 60 * 60 * 10){
		say("FAIL the script ran for too long");
		gFails++;
		finish();
	}
	if(gWaitFrames > 0){
		gWaitFrames--;
		return;
	}
	if(!gScrollLabel.empty()){
		// scroll_until: down a step a frame until the item is drawn whole,
		// not cut off by the window's edge where a click would miss it.
		const UiItem *target = findItem(gScrollWindow, gScrollLabel);
		if(target && target->clip.Contains(target->bb)){
			gScrollLabel.clear();
		}else if(--gScrollFrames <= 0){
			say("line %d: FAIL never found '%s' in '%s'", (int)gLine, gScrollLabel.c_str(), gScrollWindow.c_str());
			dumpSeen(gScrollWindow);
			gFails++;
			gScrollLabel.clear();
		}else{
			ImGuiContext &g = *GImGui;
			// '*' scrolls the open popup (a combo's list).
			if(gScrollWindow == "*" && g.OpenPopupStack.Size > 0 && g.OpenPopupStack.back().Window){
				ImGuiWindow *w = g.OpenPopupStack.back().Window;
				ImGui::SetScrollY(w, std::min(w->Scroll.y + 60.0f, w->ScrollMax.y));
			}
			for(int i = 0; i < g.Windows.Size; i++){
				ImGuiWindow *w = g.Windows[i];
				if(w->WasActive && !plainLabel(visibleTitle(w->Name)).empty() &&
				   nameMatches(w->Name, gScrollWindow, true))
					ImGui::SetScrollY(w, std::min(w->Scroll.y + 60.0f, w->ScrollMax.y));
			}
			return;
		}
	}
	if(!gWaitUntil.empty()){
		std::string why;
		if(uiAssert(gWaitUntil, 1, why)){
			gWaitUntil.clear();
		}else if(--gWaitUntilFrames <= 0){
			say("line %d: FAIL timed out waiting: %s", (int)gLine, why.c_str());
			gFails++;
			gWaitUntil.clear();
		}else
			return;
	}
	if(gStep < gSteps.size()){
		// An item still being laid out (a tab just opened, a list filling)
		// gets up to a second to show up before the lookup fails.
		static int itemWait;
		const Step &next = gSteps[gStep];
		if(next.type == Step::ITEM && itemWait < 60 &&
		   findItem(next.window, next.label, next.nth ? next.nth : 1) == nil){
			// Not drawn yet: after a short wait, scroll the named panel down a
			// step every few frames, in case it is below the panel's fold
			// (inside a child area that is out of view, so not laid out).
			if(itemWait >= 10 && itemWait % 4 == 0 && next.window != "*"){
				ImGuiContext &g = *GImGui;
				for(int i = 0; i < g.Windows.Size; i++){
					ImGuiWindow *w = g.Windows[i];
					// Docked panels count as child windows in ImGui: match by name only.
					if(w->WasActive && nameMatches(w->Name, next.window, true))
						ImGui::SetScrollY(w, std::min(w->Scroll.y + 80.0f, w->ScrollMax.y));
				}
			}
			itemWait++;
			return;
		}
		// An item scrolled out of its panel's view would be clicked through
		// to whatever is there instead: scroll it into view first, as a
		// person would. A few frames at most.
		if(next.type == Step::ITEM && itemWait < 60){
			const UiItem *target = findItem(next.window, next.label, next.nth ? next.nth : 1);
			// Only up and down: a row a few pixels too wide can't be scrolled to.
			if(target && (target->bb.Min.y < target->clip.Min.y - 0.5f || target->bb.Max.y > target->clip.Max.y + 0.5f)){
				ImGuiContext &g = *GImGui;
				for(int i = 0; i < g.Windows.Size; i++){
					ImGuiWindow *w = g.Windows[i];
					if(!w->WasActive || (w->Name != target->windowName && w->Name != target->rootWindowName))
						continue;
					float below = target->bb.Max.y - target->clip.Max.y;
					float above = target->clip.Min.y - target->bb.Min.y;
					float margin = ImGui::GetFrameHeight();
					if(below > 0.0f)
						ImGui::SetScrollY(w, std::min(w->Scroll.y + below + margin, w->ScrollMax.y));
					else if(above > 0.0f)
						ImGui::SetScrollY(w, std::max(w->Scroll.y - above - margin, 0.0f));
				}
				itemWait++;
				return;
			}
		}
		itemWait = 0;
		if(!runStep(gSteps[gStep])){
			gSteps.clear();
			gStep = 0;
		}else if(gSteps[gStep].type == Step::ITEM && gDragItem){
			// drag_item: press on the item, move by the offset, let go.
			gDragItem = false;
			int x = gMouseX, y = gMouseY;
			if(!gDragToLabel.empty()){
				// drag_item_to: the offset to the other item's middle.
				const UiItem *to = findItem(gDragToWindow, gDragToLabel);
				if(to == nil){
					say("line %d: FAIL no item '%s' in window '%s' to drop on", (int)gLine,
						gDragToLabel.c_str(), gDragToWindow.c_str());
					gFails++;
				}else{
					gDragItemDx = (int)((to->bb.Min.x + to->bb.Max.x) * 0.5f) - x;
					gDragItemDy = (int)((to->bb.Min.y + to->bb.Max.y) * 0.5f) - y;
				}
				gDragToLabel.clear();
			}
			std::vector<Step> rest;
			Step d = {}; d.type = Step::DOWN; d.value = 1; rest.push_back(d);
			for(int f = 1; f <= 10; f++){
				Step m = {}; m.type = Step::MOVE;
				m.x = x + gDragItemDx * f / 10; m.y = y + gDragItemDy * f / 10;
				rest.push_back(m);
			}
			// Rest over the drop point a few frames: ImGui drops on what was
			// hovered before the button went up.
			for(int f = 0; f < 3; f++)
				rest.push_back(rest.back());
			Step u = {}; u.type = Step::UP; u.value = 1; rest.push_back(u);
			gSteps.insert(gSteps.begin() + gStep + 1, rest.begin(), rest.end());
			gStep++;
		}else
			gStep++;
		return;
	}
	nextCommand();
}

// After the frame is drawn: frame grabs.
void
UiScriptAfterRender(void)
{
	if(!gActive || gGrabPath.empty())
		return;
#ifdef RW_GL3
	int vp[4];
	glGetIntegerv(GL_VIEWPORT, vp);
	int w = vp[2], h = vp[3];
	rw::Image *image = rw::Image::create(w, h, 24);
	image->allocate();
	std::vector<uint8> px(w * h * 3);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
	for(int y = 0; y < h; y++)
		memcpy(image->pixels + y * image->stride, &px[(h - 1 - y) * w * 3], w * 3);
	rw::writePNG(image, gGrabPath.c_str());
	image->destroy();
	say("grabbed %s (%dx%d)", gGrabPath.c_str(), w, h);
#else
	say("grab is only available in the GL3 build");
#endif
	gGrabPath.clear();
}
