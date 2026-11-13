// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "keycast.h"
#include "shortcuts.h"
#include "icons.h"
#include "i18n.h"
#include "imgui/imgui_internal.h"

#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {

enum { CTRL = ImGuiMod_Ctrl, SHIFT = ImGuiMod_Shift, ALT = ImGuiMod_Alt, MODS = CTRL | SHIFT | ALT };

enum Corner { CORNER_BOTTOM, CORNER_BOTTOM_LEFT, CORNER_BOTTOM_RIGHT, CORNER_TOP_LEFT, CORNER_TOP_RIGHT, NUM_CORNERS };

const float KEY_HOLD = 1.5f;	// seconds a combo stays after its keys are up
const float KEY_FADE = 0.4f;
const float AGENT_SAME = 1.5f;	// a repeat within this replaces the last line
const float AGENT_HOLD = 3.0f;
const float AGENT_FADE = 0.5f;
const int AGENT_LINES = 3;

bool gShowKeys = false;
bool gShowAgent = true;
int gCorner = CORNER_BOTTOM_LEFT;	// clear of the toasts, which stack up the middle

// The combo on show: its parts as key caps, the shortcut it is, how often.
struct Combo {
	std::vector<std::string> caps;
	std::string action;	// translated shortcut name, or ""
	int count = 0;
	std::vector<int> keys;	// what keeps it up while held
	int mods = 0;
	int buttons = 0;
	double last = -100.0;	// last time a part of it was down
	std::string text;	// as shown without the count, for repeats
};
Combo gCombo;

// The last combo with more than modifiers, for counting repeats: Ctrl alone
// shows between two Ctrl+D but doesn't break the count.
std::string gRepeatText;
int gRepeatCount;
double gRepeatTime = -100.0;

std::vector<int> gHeldKeys;	// in the order pressed
int gLastMods;
bool gDragging[3];

struct AgentLine { std::string text, key; double last; };
std::vector<AgentLine> gAgent;

bool
isModifierKey(int key)
{
	return key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl ||
		key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift ||
		key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
		key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper ||
		key == ImGuiKey_CapsLock || (key >= ImGuiKey_ReservedForModCtrl && key <= ImGuiKey_ReservedForModSuper);
}

std::string
keyCap(int key)
{
	char buf[64];
	ShortcutChordName({ key, 0 }, buf, sizeof(buf));
	return buf;
}

const char *buttonNames[3] = { "LMB", "RMB", "MMB" };

// The shortcut a key and modifiers make, or -1.
int
shortcutFor(int key, int mods)
{
	for(int a = 0; a < SC_NUM_ACTIONS; a++)
		for(int s = 0; s < SHORTCUT_CHORDS; s++){
			ShortcutChord c = GetShortcutChord(a, s);
			if(c.key == key && c.mods == mods)
				return a;
		}
	return -1;
}

const char*
times(void)
{
	// "×" when the font has it.
	ImFont *font = ImGui::GetFont();
	return font && font->IsGlyphInFont(0xD7) ? "\xC3\x97" : "x";
}

std::string
comboText(const Combo &c)
{
	std::string s;
	for(size_t i = 0; i < c.caps.size(); i++){
		if(i) s += " + ";
		s += c.caps[i];
	}
	if(!c.action.empty())
		s += ": " + c.action;
	if(c.count > 1)
		s += std::string(" ") + times() + std::to_string(c.count);
	return s;
}

// Something was pressed: show the combo of everything down now.
void
showCombo(int mods, int key, const char *extra, double now)
{
	Combo c;
	c.mods = mods;
	if(mods & CTRL) c.caps.push_back("Ctrl");
	if(mods & SHIFT) c.caps.push_back("Shift");
	if(mods & ALT) c.caps.push_back("Alt");
	for(int b = 0; b < 3; b++)
		if(ImGui::IsMouseDown(b)){
			c.caps.push_back(buttonNames[b]);
			c.buttons |= 1 << b;
			if(gDragging[b])
				c.caps.push_back(_("drag"));
		}
	for(int k : gHeldKeys){
		c.caps.push_back(keyCap(k));
		c.keys.push_back(k);
	}
	if(extra)
		c.caps.push_back(extra);
	if(c.caps.empty())
		return;
	// The action's name only when the editor ran it: not for W while flying.
	int action = key && extra == nil && gHeldKeys.size() == 1 && c.buttons == 0 ? shortcutFor(key, mods) : -1;
	// Esc's full name lists every mode it leaves: too long for the view.
	if(action >= 0 && ShortcutPressed(action))
		c.action = action == SC_CANCEL ? _("Cancel") : _(GetShortcutInfo(action)->name);
	c.count = 1;
	c.last = now;
	c.text = comboText(c);
	bool modsOnly = c.keys.empty() && c.buttons == 0 && extra == nil;
	if(!modsOnly){
		if(c.text == gRepeatText && now - gRepeatTime < KEY_HOLD + KEY_FADE)
			c.count = gRepeatCount + 1;
		gRepeatText = c.text;
		gRepeatCount = c.count;
		gRepeatTime = now;
	}
	gCombo = c;
}

bool
comboHeld(const Combo &c)
{
	ImGuiIO &io = ImGui::GetIO();
	if(c.mods && (io.KeyMods & c.mods) == c.mods)
		return true;
	for(int b = 0; b < 3; b++)
		if(c.buttons & 1 << b && ImGui::IsMouseDown(b))
			return true;
	for(int k : c.keys)
		if(ImGui::IsKeyDown((ImGuiKey)k))
			return true;
	return false;
}

void
readInput(double now)
{
	ImGuiIO &io = ImGui::GetIO();
	int mods = io.KeyMods & MODS;
	// A text field has the keyboard: only shortcut combos, never typed text.
	bool typing = io.WantTextInput;

	gHeldKeys.erase(std::remove_if(gHeldKeys.begin(), gHeldKeys.end(),
		[](int k){ return !ImGui::IsKeyDown((ImGuiKey)k); }), gHeldKeys.end());
	for(int b = 0; b < 3; b++)
		if(!ImGui::IsMouseDown(b))
			gDragging[b] = false;

	bool any = false;
	for(int key = ImGuiKey_Keyboard_BEGIN; key < ImGuiKey_Keyboard_END; key++){
		if(isModifierKey(key) || !ImGui::IsKeyPressed((ImGuiKey)key, false))
			continue;
		if(typing){
			if(mods & (CTRL | ALT) && shortcutFor(key, mods) >= 0)
				showCombo(mods, key, keyCap(key).c_str(), now);
			any = true;
			continue;
		}
		if(std::find(gHeldKeys.begin(), gHeldKeys.end(), key) == gHeldKeys.end())
			gHeldKeys.push_back(key);
		showCombo(mods, key, nil, now);
		any = true;
	}
	if(!typing){
		for(int b = 0; b < 3; b++){
			if(ImGui::IsMouseClicked(b)){
				showCombo(mods, 0, nil, now);
				any = true;
			}
			if(ImGui::IsMouseDown(b) && !gDragging[b] && ImGui::IsMouseDragging(b)){
				gDragging[b] = true;
				showCombo(mods, 0, nil, now);
				any = true;
			}
		}
		if(io.MouseWheel != 0.0f){
			showCombo(mods, 0, io.MouseWheel > 0.0f ? _("Wheel up") : _("Wheel down"), now);
			any = true;
		}
		// Ctrl, Shift or Alt on its own.
		if(!any && (mods & ~gLastMods) != 0)
			showCombo(mods, 0, nil, now);
	}
	gLastMods = mods;
	if(gCombo.count > 0 && comboHeld(gCombo))
		gCombo.last = now;
}

float
fade(double since, float hold, float fadeTime)
{
	if(since < hold) return 1.0f;
	return (std::max)(0.0f, 1.0f - (float)(since - hold) / fadeTime);	// (): windows.h defines max
}

// The part of the main window the 3D view shows, between the docked panels.
void
viewRect(ImVec2 *pos, ImVec2 *size)
{
	ImGuiViewport *vp = ImGui::GetMainViewport();
	*pos = vp->WorkPos;
	*size = vp->WorkSize;
#ifdef IMGUI_HAS_DOCK
	ImGuiDockNode *central = ImGui::DockBuilderGetCentralNode(ImHashStr("HaumeaDockSpace"));
	if(central && central->Size.x > 32.0f && central->Size.y > 32.0f){
		*pos = central->Pos;
		*size = central->Size;
	}
#endif
}

const ImU32 CAP_BG = IM_COL32(45, 45, 50, 235), CAP_BORDER = IM_COL32(120, 120, 130, 255);
const ImU32 TEXT = IM_COL32(240, 240, 240, 255), DIM = IM_COL32(170, 170, 175, 255);
const ImU32 AGENT_BG = IM_COL32(40, 30, 70, 225), AGENT_ACCENT = IM_COL32(190, 150, 255, 255);

// One piece of a line: a key cap, or plain text.
struct Piece { std::string text; bool cap; ImU32 colour; };

float
piecesWidth(const std::vector<Piece> &pieces, float pad)
{
	float w = 0.0f;
	for(const Piece &p : pieces)
		w += ImGui::CalcTextSize(p.text.c_str()).x + (p.cap ? pad * 2.0f : 0.0f) + pad * 0.5f;
	return w;
}

void
drawLine(ImDrawList *dl, ImVec2 at, const std::vector<Piece> &pieces, ImU32 bg, float alpha)
{
	auto fadeCol = [alpha](ImU32 c){ return (c & ~IM_COL32_A_MASK) | ((ImU32)(((c >> IM_COL32_A_SHIFT) & 0xFF) * alpha) << IM_COL32_A_SHIFT); };
	float pad = ImGui::GetFontSize() * 0.35f, h = ImGui::GetFontSize() + pad * 2.0f;
	float w = piecesWidth(pieces, pad) + pad * 1.5f;
	dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h + pad), fadeCol(bg), pad * 1.5f);
	float x = at.x + pad, y = at.y + pad * 0.5f;
	for(const Piece &p : pieces){
		ImVec2 ts = ImGui::CalcTextSize(p.text.c_str());
		if(p.cap){
			ImVec2 a(x, y), b(x + ts.x + pad * 2.0f, y + h);
			dl->AddRectFilled(a, b, fadeCol(CAP_BG), pad);
			dl->AddRect(a, b, fadeCol(CAP_BORDER), pad);
			dl->AddText(ImVec2(x + pad, y + pad), fadeCol(p.colour), p.text.c_str());
			x = b.x + pad * 0.5f;
		}else{
			dl->AddText(ImVec2(x, y + pad), fadeCol(p.colour), p.text.c_str());
			x += ts.x + pad * 0.5f;
		}
	}
}

std::vector<Piece>
comboPieces(void)
{
	std::vector<Piece> out;
	for(size_t i = 0; i < gCombo.caps.size(); i++){
		if(i) out.push_back({ "+", false, DIM });
		out.push_back({ gCombo.caps[i], true, TEXT });
	}
	if(!gCombo.action.empty())
		out.push_back({ gCombo.action, false, TEXT });
	if(gCombo.count > 1)
		out.push_back({ times() + std::to_string(gCombo.count), false, DIM });
	return out;
}

std::vector<Piece>
agentPieces(const AgentLine &l)
{
	return { { ICON_FA_BOLT " Namaka:", false, AGENT_ACCENT }, { l.text, false, TEXT } };
}

}	// namespace

void
KeycastAgentAction(const char *text, const char *key)
{
	if(!gShowAgent || text == nil || text[0] == '\0' || ImGui::GetCurrentContext() == nil)
		return;
	double now = ImGui::GetTime();
	if(!gAgent.empty()){
		AgentLine &last = gAgent.back();
		bool same = key ? last.key == key : last.key.empty() && last.text == text;
		if(same && now - last.last < AGENT_SAME){
			last.text = text;
			last.last = now;
			return;
		}
	}
	gAgent.push_back({ text, key ? key : "", now });
	if((int)gAgent.size() > AGENT_LINES)
		gAgent.erase(gAgent.begin());
}

void
KeycastFrame(void)
{
	double now = ImGui::GetTime();
	if(gShowKeys)
		readInput(now);
	gAgent.erase(std::remove_if(gAgent.begin(), gAgent.end(),
		[now](const AgentLine &l){ return now - l.last > AGENT_HOLD + AGENT_FADE; }), gAgent.end());

	float keyAlpha = gShowKeys && gCombo.count > 0 ? fade(now - gCombo.last, KEY_HOLD, KEY_FADE) : 0.0f;
	bool agent = gShowAgent && !gAgent.empty();
	if(keyAlpha <= 0.0f && !agent)
		return;

	// Lines bottom up for the bottom corners: the keys last, Namaka above.
	struct Shown { std::vector<Piece> pieces; ImU32 bg; float alpha; };
	std::vector<Shown> lines;
	if(agent)
		for(const AgentLine &l : gAgent)
			lines.push_back({ agentPieces(l), AGENT_BG, fade(now - l.last, AGENT_HOLD, AGENT_FADE) });
	if(keyAlpha > 0.0f)
		lines.push_back({ comboPieces(), IM_COL32(20, 20, 22, 200), keyAlpha });

	ImVec2 pos, size;
	viewRect(&pos, &size);
	float pad = ImGui::GetFontSize() * 0.35f, lineH = ImGui::GetFontSize() + pad * 3.0f, gap = pad;
	float margin = ImGui::GetFontSize();
	bool top = gCorner == CORNER_TOP_LEFT || gCorner == CORNER_TOP_RIGHT;
	// Bottom centre sits above the toasts.
	float bottom = pos.y + size.y - margin - (gCorner == CORNER_BOTTOM ? ImGui::GetFontSize() * 4.0f : 0.0f);
	float total = lines.size() * (lineH + gap);

	ImDrawList *dl = EditorForegroundDrawList();
	for(size_t i = 0; i < lines.size(); i++){
		float w = piecesWidth(lines[i].pieces, pad) + pad * 1.5f;
		float x;
		if(gCorner == CORNER_BOTTOM_LEFT || gCorner == CORNER_TOP_LEFT) x = pos.x + margin;
		else if(gCorner == CORNER_BOTTOM_RIGHT || gCorner == CORNER_TOP_RIGHT) x = pos.x + size.x - margin - w;
		else x = pos.x + (size.x - w) * 0.5f;
		float y = top ? pos.y + margin + i * (lineH + gap) : bottom - total + i * (lineH + gap);
		drawLine(dl, ImVec2(x, y), lines[i].pieces, lines[i].bg, lines[i].alpha);
	}
}

bool
KeycastEnabled(void)
{
	return gShowKeys;
}

void
KeycastSetEnabled(bool on)
{
	gShowKeys = on;
	if(!on)
		gCombo = Combo();
}

std::string
KeycastShownText(void)
{
	if(ImGui::GetCurrentContext() == nil)
		return "";
	double now = ImGui::GetTime();
	std::string out;
	if(gShowAgent)
		for(const AgentLine &l : gAgent)
			if(now - l.last <= AGENT_HOLD + AGENT_FADE)
				out += "Namaka: " + l.text + "\n";
	if(gShowKeys && gCombo.count > 0 && now - gCombo.last < KEY_HOLD + KEY_FADE)
		out += comboText(gCombo) + "\n";
	return out;
}

bool
KeycastLoadSetting(const char *key, const char *value)
{
	if(strcmp(key, "keycast") == 0)
		gShowKeys = atoi(value) != 0;
	else if(strcmp(key, "keycast_agent") == 0)
		gShowAgent = atoi(value) != 0;
	else if(strcmp(key, "keycast_corner") == 0)
		gCorner = (std::max)(0, (std::min)(atoi(value), NUM_CORNERS - 1));
	else
		return false;
	return true;
}

void
KeycastSaveSettings(FILE *f)
{
	fprintf(f, "keycast %d\n", gShowKeys ? 1 : 0);
	fprintf(f, "keycast_agent %d\n", gShowAgent ? 1 : 0);
	fprintf(f, "keycast_corner %d\n", gCorner);
}

bool
KeycastPreferences(void)
{
	bool changed = false;
	bool keys = gShowKeys;
	if(ImGui::Checkbox(L("Show pressed keys (for tutorials and recordings)"), &keys)){
		KeycastSetEnabled(keys);
		changed = true;
	}
	ImGui::SetItemTooltip("%s", _("Shows the keys and mouse buttons you press over the 3D view.\n"
		"While you type in a field only shortcuts show, never the text."));
	changed |= ImGui::Checkbox(L("Show what Namaka is doing"), &gShowAgent);
	ImGui::SetItemTooltip("%s", _("While Namaka works in the editor, a line over the 3D view says what it is doing."));
	if(!gShowAgent)
		gAgent.clear();
	if(gShowKeys || gShowAgent){
		const char *corners[NUM_CORNERS] = { _("Bottom"), _("Bottom left"), _("Bottom right"), _("Top left"), _("Top right") };
		changed |= ImGui::Combo(L("Where"), &gCorner, corners, NUM_CORNERS);
		ImGui::SetItemTooltip("%s", _("Where in the 3D view the pressed keys and Namaka's actions show."));
	}
	return changed;
}
