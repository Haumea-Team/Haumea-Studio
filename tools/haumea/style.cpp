// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "icons.h"
#include "lucide_icons.h"
#include "imgui/imgui_internal.h"
#include "font_data.h"
#include "lucide_data.h"
#include "manrope_data.h"
#include "i18n.h"
#include <algorithm>
#include <string>
#include <cstdio>

ImFont *gCodeFont;
ImFont *gIconFont;	// Lucide: the ribbon's outline icons
ImFont *gTerminalFont;
ImFont *gTitleFont;	// large Inter for the splash screen title
ImFont *gBoldFont;	// Manrope SemiBold at the UI size: tabs, section titles, the selected row

// Accent colors — used for highlights, active elements and selection.
static int gUiTheme = UI_THEME_DARK;
static ImVec4 gAccentFill;	// a filled button or tool that is on, under white text
// Graphite and blue, like a creative suite: the colour stays on what is
// active or selected, everything else is neutral grey.
static const ImVec4 kDarkAccent       = ImVec4(0.424f, 0.498f, 0.859f, 1.00f);	// #6C7FDB
static const ImVec4 kDarkAccentHover  = ImVec4(0.451f, 0.522f, 0.867f, 1.00f);	// #7385DD
static const ImVec4 kDarkAccentActive = ImVec4(0.369f, 0.439f, 0.788f, 1.00f);	// #5E70C9
static const ImVec4 kLightAccent       = ImVec4(0.078f, 0.451f, 0.902f, 1.00f);	// #1473E6
static const ImVec4 kLightAccentHover  = ImVec4(0.051f, 0.400f, 0.816f, 1.00f);
static const ImVec4 kLightAccentActive = ImVec4(0.035f, 0.353f, 0.729f, 1.00f);

// More themes, each from seven colours (0xRRGGBB): the window, panels,
// controls, borders, text, secondary text and the accent. Dark and Light
// use Haumea's own palettes.
struct UiPalette
{
	const char *name;
	uint32 window, panel, control, border, text, secondaryText, accent;
};
static const UiPalette kPalettes[] = {
	{ "Basalt",           0x353D3F, 0x2A3235, 0x2E383B, 0x50595C, 0xE5E8EA, 0xA8ABAE, 0x8DBF5F },
	{ "Carbon",           0x222222, 0x111111, 0x323232, 0x595959, 0xF9F9F9, 0xB3B3B3, 0xF56200 },
	{ "Catppuccin Latte", 0xEFF1F5, 0xE6E9EF, 0xF7F8FB, 0xBCC0CC, 0x4C4F69, 0x6C6F85, 0x8839EF },
	{ "Catppuccin Mocha", 0x181825, 0x1E1E2E, 0x313244, 0x45475A, 0xCDD6F4, 0xA6ADC8, 0xCBA6F7 },
	{ "Cocoa",            0x170B07, 0x1B0F0B, 0x271610, 0x45372B, 0xC8C39C, 0x989174, 0xF6EB97 },
	{ "Dracula",          0x21222C, 0x282A36, 0x343746, 0x44475A, 0xF8F8F2, 0x8F9BC9, 0xBD93F9 },
	{ "Evergreen",        0x212924, 0x252D28, 0x3B3B3B, 0x5C675F, 0xF3F3F3, 0xDEDEDE, 0x8BD69B },
	{ "Graphite",         0x151515, 0x222222, 0x101010, 0x3A3A3A, 0xC8C8C8, 0x818181, 0x86B8F9 },
	{ "Gruvbox Dark",     0x1D2021, 0x282828, 0x3C3836, 0x504945, 0xEBDBB2, 0xA89984, 0xFABD2F },
	{ "Gruvbox Light",    0xFBF1C7, 0xEBDBB2, 0xF2E5BC, 0xD5C4A1, 0x3C3836, 0x665C54, 0x076678 },
	{ "Ironwood",         0x303638, 0x212A27, 0x2A3531, 0x59635F, 0xF5F5F5, 0xC5C9C9, 0xBE3A00 },
	{ "Kanagawa",         0x16161D, 0x1F1F28, 0x2A2A37, 0x54546D, 0xDCD7BA, 0xA09C85, 0x7E9CD8 },
	{ "Midnight",         0x060916, 0x0D1123, 0x131931, 0x3E4355, 0xECECEC, 0xAEAFB3, 0xF2D62B },
	{ "Monokai",          0x1E1F1C, 0x272822, 0x3E3D32, 0x49483E, 0xF8F8F2, 0xA6A28C, 0xA6E22E },
	{ "Moonstone",        0x222529, 0x26292D, 0x363636, 0x565B62, 0xEFEFEF, 0xD0D0D0, 0x86B2E1 },
	{ "Night Owl",        0x010E1A, 0x011627, 0x0B2942, 0x1D3B53, 0xD6DEEB, 0x7791A7, 0x7FDBCA },
	{ "Nord",             0x242933, 0x2E3440, 0x3B4252, 0x4C566A, 0xECEFF4, 0x9AA5B8, 0x88C0D0 },
	{ "One Dark",         0x21252B, 0x282C34, 0x2C313A, 0x3E4451, 0xABB2BF, 0x7F848E, 0x61AFEF },
	{ "Orchid",           0x383838, 0x2D2D2D, 0x444444, 0x5C5C5C, 0xD1D1D1, 0x969696, 0x9D5BB1 },
	{ "Rose",             0x261F20, 0x2A2324, 0x352D2E, 0x58494B, 0xF1F1F1, 0xD0D0D0, 0xFAA1A8 },
	{ "Rose Pine",        0x191724, 0x1F1D2E, 0x26233A, 0x403D52, 0xE0DEF4, 0x908CAA, 0xEBBCBA },
	{ "Sand",             0x222425, 0x151515, 0x2B2B2B, 0x4E4E4E, 0xEDEDED, 0xC1C1C1, 0xF1E693 },
	{ "Soft Gray",        0xA8A8A8, 0xBFBFBF, 0xCBCBCB, 0x979797, 0x272727, 0x4B4B4B, 0x126FDA },
	{ "Solarized Dark",   0x00212B, 0x002B36, 0x073642, 0x124A58, 0xEEE8D5, 0x93A1A1, 0x268BD2 },
	{ "Solarized Light",  0xFDF6E3, 0xEEE8D5, 0xF7F1DE, 0xD6CFB8, 0x073642, 0x657B83, 0x268BD2 },
	{ "Synthwave",        0x241B2F, 0x262335, 0x34294F, 0x495495, 0xFFFFFF, 0xB6B1D1, 0xFF7EDB },
	{ "Tokyo Night",      0x16161E, 0x1A1B26, 0x24283B, 0x3B4261, 0xC0CAF5, 0x9AA5CE, 0x7AA2F7 },
	{ "Violet",           0x272727, 0x202020, 0x3A3A3A, 0x5A5A5A, 0xF5F5F5, 0xDEDEDE, 0x6C5CDD },
	{ "Walnut",           0x41240B, 0x2D1001, 0x462E14, 0x5C4323, 0xD4B682, 0xA89570, 0x4CD933 },
};
static const int kNumPalettes = (int)(sizeof(kPalettes)/sizeof(kPalettes[0]));

static const UiPalette*
currentPalette(void)
{
	int i = gUiTheme - UI_THEME_FIRST_PALETTE;
	return i >= 0 && i < kNumPalettes ? &kPalettes[i] : nil;
}

static ImVec4
paletteColour(uint32 rgb, float alpha = 1.0f)
{
	return ImVec4(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, alpha);
}

static ImVec4
mixColour(const ImVec4 &a, const ImVec4 &b, float t, float alpha = 1.0f)
{
	return ImVec4(a.x + (b.x - a.x)*t, a.y + (b.y - a.y)*t, a.z + (b.z - a.z)*t, alpha);
}

static float
luminance(const ImVec4 &c)
{
	return 0.2126f*c.x + 0.7152f*c.y + 0.0722f*c.z;
}

int
GetUiTheme(void)
{
	return gUiTheme;
}

void
SetUiTheme(int theme)
{
	gUiTheme = theme >= 0 && theme < NumUiThemes() ? theme : UI_THEME_DARK;
	if(ImGui::GetCurrentContext() != nil)
		SetupStyle();
}

int
NumUiThemes(void)
{
	return UI_THEME_FIRST_PALETTE + kNumPalettes;
}

const char*
GetUiThemeName(int theme)
{
	if(theme == UI_THEME_DARK) return _("Dark");
	if(theme == UI_THEME_LIGHT) return _("Light");
	int i = theme - UI_THEME_FIRST_PALETTE;
	return i >= 0 && i < kNumPalettes ? kPalettes[i].name : "";
}

bool
UiThemeIsLight(void)
{
	const UiPalette *p = currentPalette();
	return p ? luminance(paletteColour(p->window)) > 0.5f : gUiTheme == UI_THEME_LIGHT;
}

rw::RGBA
UiThemePreviewBackground(void)
{
	const UiPalette *p = currentPalette();
	if(p)
		return rw::RGBA{ (uint8)(p->control >> 16), (uint8)(p->control >> 8), (uint8)p->control, 255 };
	return gUiTheme == UI_THEME_LIGHT ? rw::RGBA{ 240, 240, 240, 255 } : rw::RGBA{ 35, 35, 35, 255 };
}

// A palette's colours over the whole style, after SetupStyle's own.
static void
applyPalette(ImVec4 *c, const UiPalette &p)
{
	ImVec4 window = paletteColour(p.window), panel = paletteColour(p.panel), control = paletteColour(p.control);
	ImVec4 border = paletteColour(p.border), text = paletteColour(p.text), secondary = paletteColour(p.secondaryText);
	ImVec4 accent = paletteColour(p.accent);
	ImVec4 accentHover = mixColour(accent, text, 0.25f);
	ImVec4 accentActive = mixColour(accent, window, 0.30f);
	gAccentFill = luminance(accent) > 0.45f ? mixColour(accent, ImVec4(0, 0, 0, 1), 0.35f) : accent;
	bool light = luminance(window) > 0.5f;
	ImVec4 shade = light ? ImVec4(0, 0, 0, 1) : ImVec4(1, 1, 1, 1);

	c[ImGuiCol_WindowBg] = window;
	c[ImGuiCol_ChildBg] = ImVec4(panel.x, panel.y, panel.z, 0.0f);
	c[ImGuiCol_PopupBg] = ImVec4(panel.x, panel.y, panel.z, 0.98f);
	c[ImGuiCol_Border] = ImVec4(border.x, border.y, border.z, 0.70f);
	c[ImGuiCol_Text] = text;
	c[ImGuiCol_TextDisabled] = secondary;
	c[ImGuiCol_FrameBg] = control;
	c[ImGuiCol_FrameBgHovered] = mixColour(control, shade, 0.07f);
	c[ImGuiCol_FrameBgActive] = mixColour(control, shade, 0.12f);
	c[ImGuiCol_TitleBg] = mixColour(window, ImVec4(0, 0, 0, 1), 0.15f);
	c[ImGuiCol_TitleBgActive] = panel;
	c[ImGuiCol_TitleBgCollapsed] = mixColour(window, ImVec4(0, 0, 0, 1), 0.15f, 0.70f);
	c[ImGuiCol_MenuBarBg] = panel;
	c[ImGuiCol_ScrollbarBg] = ImVec4(window.x, window.y, window.z, 0.80f);
	c[ImGuiCol_ScrollbarGrab] = border;
	c[ImGuiCol_ScrollbarGrabHovered] = mixColour(border, text, 0.20f);
	c[ImGuiCol_ScrollbarGrabActive] = mixColour(border, accent, 0.45f);
	c[ImGuiCol_CheckMark] = accent;
	c[ImGuiCol_SliderGrab] = mixColour(border, text, 0.25f);
	c[ImGuiCol_SliderGrabActive] = accent;
	c[ImGuiCol_Button] = mixColour(panel, shade, 0.08f);
	c[ImGuiCol_ButtonHovered] = mixColour(panel, shade, 0.15f);
	c[ImGuiCol_ButtonActive] = accentActive;
	c[ImGuiCol_Header] = mixColour(panel, shade, 0.07f);
	c[ImGuiCol_HeaderHovered] = mixColour(panel, accent, 0.22f);
	c[ImGuiCol_HeaderActive] = accentActive;
	c[ImGuiCol_Separator] = border;
	c[ImGuiCol_SeparatorHovered] = accentHover;
	c[ImGuiCol_SeparatorActive] = accent;
	c[ImGuiCol_ResizeGrip] = ImVec4(accent.x, accent.y, accent.z, 0.25f);
	c[ImGuiCol_ResizeGripHovered] = accentHover;
	c[ImGuiCol_ResizeGripActive] = accent;
	c[ImGuiCol_Tab] = panel;
	c[ImGuiCol_TabHovered] = mixColour(panel, accent, 0.25f);
	c[ImGuiCol_TabSelected] = mixColour(panel, accent, 0.18f);
	c[ImGuiCol_TabDimmed] = c[ImGuiCol_Tab];
	c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabSelected];
	c[ImGuiCol_TabSelectedOverline] = accent;
	c[ImGuiCol_TabDimmedSelectedOverline] = accent;
	c[ImGuiCol_TableHeaderBg] = panel;
	c[ImGuiCol_TableBorderStrong] = border;
	c[ImGuiCol_TableBorderLight] = mixColour(border, panel, 0.5f);
	c[ImGuiCol_TableRowBgAlt] = ImVec4(shade.x, shade.y, shade.z, light ? 0.025f : 0.03f);
	c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.25f);
	c[ImGuiCol_DragDropTarget] = accent;
	c[ImGuiCol_NavHighlight] = accent;
	c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, light ? 0.32f : 0.55f);
}

ImVec4
UiAccentFill(void)
{
	return gAccentFill;
}

ImVec4
UiSemanticColor(int color)
{
	bool light = UiThemeIsLight();
	switch(color){
	case UI_COLOR_SUCCESS:
		return light ? ImVec4(0.10f, 0.42f, 0.25f, 1.0f) : ImVec4(0.45f, 0.85f, 0.55f, 1.0f);
	case UI_COLOR_WARNING:
		return light ? ImVec4(0.53f, 0.34f, 0.00f, 1.0f) : ImVec4(1.00f, 0.78f, 0.25f, 1.0f);
	case UI_COLOR_ERROR:
		return light ? ImVec4(0.68f, 0.14f, 0.12f, 1.0f) : ImVec4(1.00f, 0.42f, 0.38f, 1.0f);
	case UI_COLOR_INFO:
		return light ? ImVec4(0.10f, 0.36f, 0.55f, 1.0f) : ImVec4(0.45f, 0.80f, 1.0f, 1.0f);
	}
	return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

// A section of a panel, folding: its title in small capitals in the label
// grey, flat, with a faint wash on hover; no bar of colour. Takes what
// ImGui::CollapsingHeader takes. The ID stays the label's own.
bool
UiSection(const char *label, ImGuiTreeNodeFlags flags)
{
	const char *hash = strstr(label, "###");
	const char *end = ImGui::FindRenderedTextEnd(label);
	std::string shown(label, end);
	for(char &ch : shown)
		if((unsigned char)ch < 128)
			ch = (char)toupper((unsigned char)ch);
	std::string full = shown + (hash ? std::string(hash) : std::string("###") + label);
	bool light = UiThemeIsLight();
	ImVec4 wash = light ? ImVec4(0, 0, 0, 1) : ImVec4(1, 1, 1, 1);
	ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(wash.x, wash.y, wash.z, 0.05f));
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(wash.x, wash.y, wash.z, 0.10f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 5.0f));
	bool open = ImGui::TreeNodeEx(full.c_str(), flags | ImGuiTreeNodeFlags_CollapsingHeader);
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(4);
	return open;
}

// A group's title that doesn't fold: small capitals in the label grey, with
// room above it (in place of ImGui::SeparatorText).
void
UiHeading(const char *label)
{
	const char *end = ImGui::FindRenderedTextEnd(label);
	std::string caps(label, end);
	for(char &ch : caps)
		if((unsigned char)ch < 128)
			ch = (char)toupper((unsigned char)ch);
	if(ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + 1.0f)
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.35f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextUnformatted(caps.c_str());
	ImGui::PopStyleColor();
}

// A field's name in a properties panel's left column, the field filling the
// rest of the row; above the field when the panel is too narrow for both or
// the name too long for its column; just before it when the field follows
// something on the same line. A width set for the field is kept. Writes
// the ID the field takes: "###" and its English name, so it stays the
// same ID (and UI scripts find it by name) whatever the language.
void
UiFieldLabel(const char *label, char *id, size_t size)
{
	const char *hash = strstr(label, "###");
	const char *end = ImGui::FindRenderedTextEnd(label);
	if(hash) snprintf(id, size, "%s", hash);
	else if(end == label) snprintf(id, size, "%s", label);	// "##id": no name to show
	else snprintf(id, size, "###%s", label);
	if(end == label)
		return;
	ImGuiContext &g = *ImGui::GetCurrentContext();
	ImGuiWindow *window = g.CurrentWindow;
	bool widthSet = (g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth) != 0;
	float width = g.NextItemData.Width;
	bool inRow = window->DC.IsSameLine;
	float fs = ImGui::GetFontSize();
	float startX = ImGui::GetCursorPosX();
	float avail = ImGui::GetContentRegionAvail().x;
	float column = std::min(fs * 9.4f, avail * 0.42f);
	float textWidth = ImGui::CalcTextSize(label, end).x;
	ImGui::AlignTextToFramePadding();
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextUnformatted(label, end);
	ImGui::PopStyleColor();
	if(inRow)
		ImGui::SameLine();
	else if(avail >= fs * 14.0f && textWidth <= column - fs * 0.5f){
		ImGui::SameLine();
		ImGui::SetCursorPosX(startX + column);
	}
	ImGui::SetNextItemWidth(widthSet ? width : -FLT_MIN);
}

// The on/off switch: a pill, the accent and a knob at the right when on, the
// field grey with a ringed knob at the left when off.
static void
drawTogglePill(ImDrawList *dl, ImVec2 at, float width, float height, bool on)
{
	float r = height * 0.5f, knob = height - 4.0f;
	ImVec2 end(at.x + width, at.y + height);
	if(on){
		dl->AddRectFilled(at, end, ImGui::GetColorU32(ImGuiCol_CheckMark), r);
		dl->AddCircleFilled(ImVec2(end.x - 2.0f - knob*0.5f, at.y + r), knob*0.5f, ImGui::GetColorU32(ImVec4(1, 1, 1, 1)));
	}else{
		dl->AddRectFilled(at, end, ImGui::GetColorU32(ImGuiCol_FrameBg), r);
		dl->AddRect(at, end, ImGui::GetColorU32(ImVec4(0.54f, 0.54f, 0.54f, 1.0f)), r, 0, 1.0f);
		dl->AddCircleFilled(ImVec2(at.x + 2.0f + knob*0.5f, at.y + r), knob*0.5f, ImGui::GetColorU32(ImVec4(0.54f, 0.54f, 0.54f, 1.0f)));
	}
}

// An option that is on or off, as a properties panel shows one: on its own
// line, a row with its name on the left and a switch at the right end, the
// whole row clickable; after something on the same line, the switch and its
// name; with no name, the switch alone. In place of ImGui::Checkbox.
bool
UiCheckbox(const char *label, bool *value)
{
	ImGuiContext &g = *ImGui::GetCurrentContext();
	bool inRow = g.CurrentWindow->DC.IsSameLine;
	const char *end = ImGui::FindRenderedTextEnd(label);
	bool named = end != label;
	float fs = ImGui::GetFontSize();
	float h = ImGui::GetFrameHeight();
	float pillW = floorf(fs * 2.0f), pillH = floorf(fs * 1.1f);
	ImVec2 at = ImGui::GetCursorScreenPos();
	ImVec2 size;
	if(!named) size = ImVec2(pillW, h);
	else if(inRow) size = ImVec2(pillW + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label, end).x, h);
	else{
		// The row's width, or SetNextItemWidth's, so a button can end the row.
		float w = ImGui::GetContentRegionAvail().x;
		if(g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth)
			w = g.NextItemData.Width > 0.0f ? g.NextItemData.Width : w + g.NextItemData.Width;
		size = ImVec2(std::max(w, pillW), h);
	}
	bool pressed = ImGui::InvisibleButton(label, size);
	if(pressed){
		*value = !*value;
		ImGui::MarkItemEdited(g.LastItemData.ID);
	}
	bool hovered = ImGui::IsItemHovered();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	float pillY = at.y + floorf((h - pillH) * 0.5f);
	if(named && !inRow){
		if(hovered)
			dl->AddRectFilled(at, ImVec2(at.x + size.x, at.y + h), ImGui::GetColorU32(ImGuiCol_HeaderHovered), ImGui::GetStyle().FrameRounding);
		dl->AddText(ImVec2(at.x, at.y + (h - fs) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text, 0.9f), label, end);
		drawTogglePill(dl, ImVec2(at.x + size.x - pillW - 2.0f, pillY), pillW, pillH, *value);
	}else{
		drawTogglePill(dl, ImVec2(at.x, pillY), pillW, pillH, *value);
		if(named)
			dl->AddText(ImVec2(at.x + pillW + ImGui::GetStyle().ItemInnerSpacing.x, at.y + (h - fs) * 0.5f),
				ImGui::GetColorU32(ImGuiCol_Text), label, end);
	}
	return pressed;
}

// A segmented switch: the choices in one well of the field grey, the current
// one raised on the panel's grey with the full text, the others dim. fill:
// share the row's width; otherwise each as wide as its name.
bool
UiSegmented(const char *id, const char *const *labels, int count, int *current, bool fill)
{
	ImGui::PushID(id);
	float pad = 3.0f, h = ImGui::GetFrameHeight();
	float avail = ImGui::GetContentRegionAvail().x;
	float natural = 0.0f;
	for(int i = 0; i < count; i++)
		natural += ImGui::CalcTextSize(labels[i], nil, true).x + ImGui::GetFontSize() * 1.6f;
	float wellW = fill ? avail : std::min(avail, natural + pad*2.0f);
	ImVec2 at = ImGui::GetCursorScreenPos();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	float r = ImGui::GetStyle().FrameRounding;
	dl->AddRectFilled(at, ImVec2(at.x + wellW, at.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), r);
	float x = at.x + pad, inner = wellW - pad*2.0f;
	bool changed = false;
	for(int i = 0; i < count; i++){
		float natW = ImGui::CalcTextSize(labels[i], nil, true).x + ImGui::GetFontSize() * 1.6f;
		float w = fill ? floorf(inner / count) : floorf(natW * std::min(1.0f, inner / natural));
		if(fill && i == count - 1)
			w = at.x + wellW - pad - x;
		ImGui::SetCursorScreenPos(ImVec2(x, at.y + pad));
		if(ImGui::InvisibleButton(labels[i], ImVec2(w, h - pad*2.0f)) && *current != i){
			*current = i;
			changed = true;
		}
		bool hovered = ImGui::IsItemHovered(), on = *current == i;
		ImVec2 p0(x, at.y + pad), p1(x + w, at.y + h - pad);
		if(on)
			dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_WindowBg), r - 1.0f);
		else if(hovered)
			dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_HeaderHovered), r - 1.0f);
		const char *end = ImGui::FindRenderedTextEnd(labels[i]);
		ImVec2 ts = ImGui::CalcTextSize(labels[i], end);
		dl->AddText(ImVec2(floorf(x + (w - ts.x)*0.5f), floorf(at.y + (h - ts.y)*0.5f)),
			ImGui::GetColorU32(on || hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled), labels[i], end);
		x += w;
	}
	ImGui::SetCursorScreenPos(at);
	ImGui::Dummy(ImVec2(wellW, h));
	ImGui::PopID();
	return changed;
}

// A window's main action, along its foot: full width, the accent fill and
// white text. Pushed to the bottom of the window when there is room.
bool
UiFooterButton(const char *label, bool enabled)
{
	float y = ImGui::GetWindowContentRegionMax().y - ImGui::GetFrameHeight();
	if(ImGui::GetCursorPosY() < y && !(ImGui::GetCurrentWindow()->Flags & ImGuiWindowFlags_AlwaysAutoResize))
		ImGui::SetCursorPosY(y);
	else
		ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
	ImVec4 fill = UiAccentFill();
	ImGui::PushStyleColor(ImGuiCol_Button, fill);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(fill.x*1.12f, fill.y*1.12f, fill.z*1.12f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(fill.x*0.9f, fill.y*0.9f, fill.z*0.9f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
	ImGui::BeginDisabled(!enabled);
	bool pressed = ImGui::Button(label, ImVec2(-FLT_MIN, 0));
	ImGui::EndDisabled();
	ImGui::PopStyleColor(4);
	return pressed && enabled;
}

// A read-only detail as a properties-panel row: its name in grey on the
// left, its value as plain text, and a copy button that shows on the row
// under the mouse. True when the value was copied.
bool
UiInfoRow(const char *label, const char *value, const char *tooltip)
{
	ImGui::PushID(label);
	float fs = ImGui::GetFontSize(), avail = ImGui::GetContentRegionAvail().x;
	float column = std::min(fs * 9.4f, avail * 0.42f);
	float h = ImGui::GetFrameHeight();
	ImVec2 at = ImGui::GetCursorScreenPos();
	float startX = ImGui::GetCursorPosX();
	bool hovered = ImGui::IsMouseHoveringRect(at, ImVec2(at.x + avail, at.y + h)) && ImGui::IsWindowHovered();
	if(hovered)
		ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + avail, at.y + h),
			ImGui::GetColorU32(ImGuiCol_HeaderHovered), ImGui::GetStyle().FrameRounding);
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", label);
	ImGui::SameLine();
	ImGui::SetCursorPosX(startX + column);
	float valueWidth = avail - column - h - ImGui::GetStyle().ItemSpacing.x;
	ImVec2 v = ImGui::GetCursorScreenPos();
	ImGui::PushClipRect(v, ImVec2(v.x + valueWidth, v.y + h), true);
	ImGui::TextUnformatted(value && value[0] ? value : "-");
	ImGui::PopClipRect();
	if(tooltip && tooltip[0])
		ImGui::SetItemTooltip("%s", tooltip);
	bool copied = false;
	ImGui::SameLine();
	ImGui::SetCursorPosX(startX + avail - h);
	if(hovered && value && value[0]){
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		if(ImGui::Button(ICON_FA_COPY "##copy", ImVec2(h, h))){
			ImGui::SetClipboardText(value);
			copied = true;
		}
		ImGui::PopStyleColor();
		ImGui::SetItemTooltip("%s", _("Copy"));
	}else
		ImGui::Dummy(ImVec2(h, h));
	ImGui::PopID();
	return copied;
}

void
SetupStyle(void)
{
	ImGuiStyle &style = ImGui::GetStyle();
	bool light = UiThemeIsLight();
	if(light)
		ImGui::StyleColorsLight(&style);
	else
		ImGui::StyleColorsDark(&style);
	ImVec4 accent = light ? kLightAccent : kDarkAccent;
	ImVec4 accentHover = light ? kLightAccentHover : kDarkAccentHover;
	ImVec4 accentActive = light ? kLightAccentActive : kDarkAccentActive;
	gAccentFill = light ? accentActive : accent;	// the main buttons

	// ---- Geometry: soft, roomy and flat. Surfaces are told apart by tone,
	// not by outlines; controls are rounded and a little taller. ----
	// ---- Geometry: soft, roomy and flat. Surfaces are told apart by tone,
	// not by outlines; controls are rounded and a little taller. ----
	style.WindowRounding    = 8.0f;
	style.ChildRounding     = 8.0f;
	style.FrameRounding     = 5.0f;
	style.PopupRounding     = 8.0f;
	style.ScrollbarRounding = 8.0f;
	style.GrabRounding      = 4.0f;
	style.TabRounding       = 5.0f;

	style.WindowPadding     = ImVec2(12, 10);
	style.FramePadding      = ImVec2(9, 5);
	style.ItemSpacing       = ImVec2(8, 6);
	style.ItemInnerSpacing  = ImVec2(6, 5);
	style.IndentSpacing     = 16.0f;
	style.ScrollbarSize     = 11.0f;
	style.GrabMinSize       = 11.0f;
	style.WindowBorderSize  = 1.0f;
	style.ChildBorderSize   = 0.0f;
	style.PopupBorderSize   = 1.0f;
	style.FrameBorderSize   = 0.0f;
	style.TabBorderSize     = 0.0f;
	style.TabBarBorderSize  = 0.0f;
	style.TabBarOverlineSize = 2.0f;
#ifdef IMGUI_HAS_DOCK
	style.DockingSeparatorSize = 2.0f;
#endif

	style.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
	// No collapse arrow at the start of title bars and tab strips.
	style.WindowMenuButtonPosition = ImGuiDir_None;

#ifdef IMGUI_HAS_DOCK
	// Panels dock by dragging their title bar onto the edges of the
	// screen or onto another panel. Needs the docking ImGui that the
	// librw submodule (Haumea-Team/librw) ships.
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#endif
	style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
	style.SeparatorTextPadding = ImVec2(0.0f, 4.0f);
	style.SeparatorTextBorderSize = 1.0f;

	// ---- Colors ----
	ImVec4 *c = style.Colors;
	// Every grey below is one of these: the app's frame (menus, bars),
	// panels, input wells, raised controls, lines and text.
	auto rgb = [](int hex) { return ImVec4(((hex >> 16) & 255)/255.0f, ((hex >> 8) & 255)/255.0f, (hex & 255)/255.0f, 1.0f); };
	// Dark: neutral near-black, a properties-panel editor's: the ground and
	// the gaps between panels black, panels and bars one step up, fields
	// and buttons another. Neutral on purpose: a cool cast would tint every
	// colour judged against it. Light: soft cool greys.
	ImVec4 chrome  = light ? rgb(0xE6E8EE) : rgb(0x0A0A0B);	// menus, the ground
	ImVec4 bar     = light ? rgb(0xEDEFF4) : rgb(0x121213);	// tab strips, side bars, the ribbon
	ImVec4 panel   = light ? rgb(0xF3F4F7) : rgb(0x121213);	// panels
	ImVec4 well    = light ? rgb(0xFFFFFF) : rgb(0x191A1C);	// fields
	ImVec4 raised  = light ? rgb(0xE2E5EC) : rgb(0x1E1F21);	// buttons
	ImVec4 line    = light ? rgb(0xD2D6E0) : ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
	ImVec4 divider = light ? rgb(0xC4C9D6) : rgb(0x0A0A0B);	// between panels
	ImVec4 text    = light ? rgb(0x1B1E27) : rgb(0xE2E2E6);
	ImVec4 muted   = light ? rgb(0x5E6475) : rgb(0x8F8F97);
	auto lift = [&](ImVec4 v, float t) { return mixColour(v, light ? ImVec4(0,0,0,1) : ImVec4(1,1,1,1), t); };

	c[ImGuiCol_WindowBg]             = panel;
	c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_PopupBg]              = light ? rgb(0xFBFCFE) : rgb(0x191A1C);
	c[ImGuiCol_Border]               = line;
	c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_Text]                 = text;
	c[ImGuiCol_TextDisabled]         = muted;

	c[ImGuiCol_FrameBg]              = well;
	c[ImGuiCol_FrameBgHovered]       = lift(well, 0.05f);
	c[ImGuiCol_FrameBgActive]        = lift(well, 0.09f);

	// Floating panels' title bars and the docked panels' tab strips.
	c[ImGuiCol_TitleBg]              = bar;
	c[ImGuiCol_TitleBgActive]        = bar;
	c[ImGuiCol_TitleBgCollapsed]     = ImVec4(bar.x, bar.y, bar.z, 0.85f);
	c[ImGuiCol_MenuBarBg]            = chrome;

	c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_ScrollbarGrab]        = light ? lift(panel, 0.18f) : ImVec4(1, 1, 1, 0.10f);
	c[ImGuiCol_ScrollbarGrabHovered] = light ? lift(panel, 0.28f) : ImVec4(1, 1, 1, 0.16f);
	c[ImGuiCol_ScrollbarGrabActive]  = light ? lift(panel, 0.38f) : ImVec4(1, 1, 1, 0.22f);

	c[ImGuiCol_CheckMark]            = accent;
	c[ImGuiCol_SliderGrab]           = mixColour(accent, panel, 0.35f);
	c[ImGuiCol_SliderGrabActive]     = accent;

	c[ImGuiCol_Button]               = raised;
	c[ImGuiCol_ButtonHovered]        = light ? lift(raised, 0.08f) : rgb(0x26272A);
	c[ImGuiCol_ButtonActive]         = light ? ImVec4(accent.x, accent.y, accent.z, 0.55f) : rgb(0x2E2F33);

	// Rows: hover a faint white wash, pressed a stronger one; the selected
	// row is the accent's, as a tree selects.
	c[ImGuiCol_Header]               = light ? lift(panel, 0.05f) : ImVec4(accent.x, accent.y, accent.z, 0.72f);
	c[ImGuiCol_HeaderHovered]        = light ? ImVec4(accent.x, accent.y, accent.z, 0.22f) : ImVec4(1, 1, 1, 0.05f);
	c[ImGuiCol_HeaderActive]         = light ? ImVec4(accent.x, accent.y, accent.z, 0.42f) : ImVec4(1, 1, 1, 0.10f);

	// Menus' and lists' dividers: a faint line on dark; the gaps between
	// panels stay the ground's black (DockingEmptyBg).
	c[ImGuiCol_Separator]            = light ? divider : ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
	c[ImGuiCol_SeparatorHovered]     = accentHover;
	c[ImGuiCol_SeparatorActive]      = accent;

	c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_ResizeGripHovered]    = accentHover;
	c[ImGuiCol_ResizeGripActive]     = accent;

	// Tabs: the open one is the panel's own colour, the rest sink into the strip.
	// Tabs: the open one a pill of the field grey on the dock's ground, the
	// others bare; no line of colour.
	c[ImGuiCol_Tab]                  = bar;
	c[ImGuiCol_TabHovered]           = light ? lift(bar, 0.08f) : rgb(0x1A1A1C);
	c[ImGuiCol_TabSelected]          = light ? panel : well;
	c[ImGuiCol_TabDimmed]            = bar;
	c[ImGuiCol_TabDimmedSelected]    = light ? panel : well;
	c[ImGuiCol_TabSelectedOverline]  = light ? accent : ImVec4(0, 0, 0, 0);
	c[ImGuiCol_TabDimmedSelectedOverline] = light ? ImVec4(accent.x, accent.y, accent.z, 0.5f) : ImVec4(0, 0, 0, 0);
#ifdef IMGUI_HAS_DOCK
	c[ImGuiCol_DockingPreview]       = ImVec4(accent.x, accent.y, accent.z, 0.40f);
	c[ImGuiCol_DockingEmptyBg]       = chrome;
#endif

	c[ImGuiCol_TableHeaderBg]        = lift(panel, 0.05f);
	c[ImGuiCol_TableBorderStrong]    = line;
	c[ImGuiCol_TableBorderLight]     = lift(panel, 0.07f);
	c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_TableRowBgAlt]        = light ? ImVec4(0, 0, 0, 0.025f) : ImVec4(1, 1, 1, 0.02f);

	c[ImGuiCol_TextSelectedBg]       = ImVec4(accent.x, accent.y, accent.z, light ? 0.35f : 0.45f);
	c[ImGuiCol_DragDropTarget]       = accent;
	c[ImGuiCol_NavHighlight]         = accent;
	c[ImGuiCol_NavWindowingHighlight]= light ? ImVec4(0.10f, 0.10f, 0.10f, 0.55f) : ImVec4(1, 1, 1, 0.70f);
	c[ImGuiCol_NavWindowingDimBg]    = light ? ImVec4(0.20f, 0.20f, 0.20f, 0.14f) : ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
	c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.00f, 0.00f, 0.00f, light ? 0.32f : 0.55f);

	if(const UiPalette *palette = currentPalette())
		applyPalette(c, *palette);

	// Keep ImGui geometry in GLFW logical coordinates. The camera raster and
	// viewport map those positions to framebuffer pixels; scissor clips need
	// their own framebuffer scaling in the draw-list adapter.
}

void
SetupFonts(void)
{
	ImGuiIO &io = ImGui::GetIO();
	float fontSize = 16.0f;

	// Only bake the exact icons used by Haumea. Packing the entire private-use
	// area creates a very large atlas, which can fail on some D3D9 GPUs during
	// startup even though the rest of the app is fine.
	static const ImWchar iconRanges[] = {
		0xe13a, 0xe13a, // code-compare
		0xf002, 0xf005, // magnifying-glass, star
		0xf007, 0xf007, // user
		0xf031, 0xf031, // font
		0xf0c8, 0xf0c8, // square
		0xf276, 0xf276, // map-pin
		0xf3c5, 0xf3c5, // location-dot
		0xf00c, 0xf00d, // check, xmark
		0xf013, 0xf013, // gear
		0xf017, 0xf017, // clock
		0xf023, 0xf023, // lock
		0xf028, 0xf028, // volume-high
		0xf03a, 0xf03a, // list
		0xf03d, 0xf03e, // video, image
		0xf05b, 0xf05b, // crosshairs
		0xf063, 0xf063, // arrow-down
		0xf0ae, 0xf0b0, // list-check, filter
		0xf0c0, 0xf0c0, // users
		0xf0d7, 0xf0d7, // caret-down
		0xf0da, 0xf0da, // caret-right
		0xf1de, 0xf1de, // sliders
		0xf120, 0xf120, // terminal
		0xf043, 0xf043, // droplet
		0xf047, 0xf047, // arrows-up-down-left-right
		0xf04b, 0xf04d, // play, pause, stop
		0xf059, 0xf05a, // circle-question, circle-info
		0xf06e, 0xf06e, // eye
		0xf070, 0xf071, // eye-slash, triangle-exclamation
		0xf076, 0xf076, // magnet
		0xf07c, 0xf07c, // folder-open
		0xf0ac, 0xf0ad, // globe, wrench
		0xf0c1, 0xf0c1, // link
		0xf0c4, 0xf0c7, // scissors, copy, floppy-disk
		0xf24d, 0xf24d, // clone
		0xf121, 0xf121, // code
		0xf0db, 0xf0db, // table-columns
		0xf0e7, 0xf0ea, // bolt, paste
		0xf0f3, 0xf0f3, // bell
		0xf11b, 0xf11b, // gamepad
		0xf15b, 0xf15b, // file
		0xf185, 0xf185, // sun
		0xf1b2, 0xf1b2, // cube
		0xf1da, 0xf1da, // clock-rotate-left
		0xf1f8, 0xf1fc, // trash, paintbrush
		0xf245, 0xf245, // arrow-pointer
		0xf2d0, 0xf2d0, // window-maximize
		0xf2ea, 0xf2f1, // rotate-left, rotate
		0xf2f5, 0xf2f9, // right-from-bracket, rotate-right
		0xf304, 0xf304, // pen
		0xf56e, 0xf56f, // file-export, file-import
		0xf5cb, 0xf5cb, // vector-square
		0xf5fd, 0xf5fd, // layer-group
		0xf601, 0xf601, // location-crosshairs
		0xf6fc, 0xf6fc, // mountain
		0xf743, 0xf743, // cloud-sun
		0xf773, 0xf773, // water
		0,
	};

	// --- Main font: Manrope (embedded), a properties-panel editor's face;
	// Inter merged under it for the letters Manrope lacks. ---
	ImFontConfig fontCfg;
	fontCfg.FontDataOwnedByAtlas = false;
	io.Fonts->AddFontFromMemoryTTF(
		(void *)manrope_regular_font_data, manrope_regular_font_size,
		fontSize, &fontCfg);
	ImFontConfig fallbackCfg;
	fallbackCfg.MergeMode = true;
	fallbackCfg.FontDataOwnedByAtlas = false;
	io.Fonts->AddFontFromMemoryTTF(
		(void *)inter_font_data, inter_font_size,
		fontSize, &fallbackCfg);

	// --- Icon font: FontAwesome 6 (embedded, merged) ---
	ImFontConfig iconCfg;
	iconCfg.MergeMode = true;
	iconCfg.PixelSnapH = true;
	iconCfg.GlyphMinAdvanceX = fontSize;
	iconCfg.FontDataOwnedByAtlas = false;
	io.Fonts->AddFontFromMemoryTTF(
		(void *)fa_solid_font_data, fa_solid_font_size,
		fontSize - 1.0f, &iconCfg, iconRanges);

	// --- Other scripts: Inter covers Latin, Greek and Cyrillic, and the atlas
	// holds only the ranges it is given, so a catalog in any other script drew
	// every character as "?". Bake the characters the catalogs translate into,
	// so a language pays only for the glyphs it uses, and merge them in from a
	// face that has them. Static, like iconRanges: the atlas is built at the
	// first frame, after this function returns. ---
	static const ImWchar *scriptRanges = nil;
	static std::vector<ImWchar> scriptRangeList;
	static bool eastAsian = false;
	if(scriptRanges == nil){
		std::vector<unsigned int> codepoints;
		I18nCatalogCodepoints(codepoints);
		// Text the user types (group names, notes) is in no catalog, so an East
		// Asian catalog also gets the common Chinese set. Only characters past
		// Latin-1 need baking: the default range stops at U+00FF, so Polish,
		// Romanian and Turkish letters (Latin Extended-A, U+0100..U+017F) need
		// it too.
		for(unsigned int cp : codepoints)
			if(cp >= 0x2E80)
				eastAsian = true;
		std::vector<unsigned int> baked;
		for(unsigned int cp : codepoints)
			if(cp >= 0x100)
				baked.push_back(cp);
		if(eastAsian){
			for(const ImWchar *r = io.Fonts->GetGlyphRangesChineseSimplifiedCommon(); r[0]; r += 2)
				for(unsigned int c = r[0]; c <= r[1]; c++)
					if(c >= 0x100)
						baked.push_back(c);
		}
		std::sort(baked.begin(), baked.end());
		baked.erase(std::unique(baked.begin(), baked.end()), baked.end());
		// Neighbouring characters share one range; a range per character would
		// make this array needlessly large.
		for(size_t i = 0; i < baked.size(); ){
			size_t last = i;
			while(last + 1 < baked.size() && baked[last + 1] == baked[last] + 1)
				last++;
			scriptRangeList.push_back((ImWchar)baked[i]);
			scriptRangeList.push_back((ImWchar)baked[last]);
			i = last + 1;
		}
		if(!scriptRangeList.empty()){
			scriptRangeList.push_back(0);
			scriptRanges = scriptRangeList.data();
		}
	}
	if(scriptRanges != nil && !eastAsian){
		// Inter carries Greek, Cyrillic and the Latin extensions itself: bake
		// those from the embedded copy instead of loading another face.
		ImFontConfig latinCfg;
		latinCfg.MergeMode = true;
		latinCfg.PixelSnapH = true;
		latinCfg.FontDataOwnedByAtlas = false;
		io.Fonts->AddFontFromMemoryTTF(
			(void *)inter_font_data, inter_font_size,
			fontSize, &latinCfg, scriptRanges);
	}
	if(scriptRanges != nil && eastAsian){
		// Ship the same CJK face on every platform. System paths are only a
		// fallback for installations made without the bundled fonts folder.
		std::string scriptFace;
		ImU32 scriptFaceNo = 0;
		char fontRoot[2048];
		if(GetEditorRootDirectory(fontRoot, sizeof(fontRoot))){
			const char *paths[] = {
				"/fonts/NotoSansSC-Regular.otf", "/../../../fonts/NotoSansSC-Regular.otf",
				"/fonts/NotoSansSC-Regular.ttf", "/../../../fonts/NotoSansSC-Regular.ttf"
			};
			for(const char *relative : paths){
				std::string candidate = std::string(fontRoot) + relative;
				FILE *f = fopen(candidate.c_str(), "rb");
				if(f){ fclose(f); scriptFace = candidate; break; }
			}
		}
		if(scriptFace.empty()){
			// A .ttc holds several faces; FontNo picks one from it.
			struct { const char *path; ImU32 face; } faces[] = {
				{ "C:/Windows/Fonts/msyh.ttc", 0 },	// Microsoft YaHei
				{ "C:/Windows/Fonts/simhei.ttf", 0 },	// SimHei
				{ "C:/Windows/Fonts/simsun.ttc", 0 },	// SimSun
				{ "/System/Library/Fonts/PingFang.ttc", 0 },	// macOS
				{ "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 0 },	// Debian, Ubuntu
				{ "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 0 },	// Arch Linux
				{ "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", 0 },
				{ "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc", 0 },
			};
			for(const auto &candidate : faces){
				FILE *f = fopen(candidate.path, "rb");
				if(f){ fclose(f); scriptFace = candidate.path; scriptFaceNo = candidate.face; break; }
			}
		}
		if(!scriptFace.empty()){
			ImFontConfig scriptCfg;
			scriptCfg.MergeMode = true;
			scriptCfg.PixelSnapH = true;
			scriptCfg.FontNo = scriptFaceNo;
			io.Fonts->AddFontFromFileTTF(scriptFace.c_str(), fontSize, &scriptCfg, scriptRanges);
		}
	}

 // A real monospace face, bundled for material text in every release.
 char editorRoot[2048];
 std::string mono;
 if(GetEditorRootDirectory(editorRoot, sizeof(editorRoot))){
  const char *paths[] = { "/fonts/material/LiberationMono-Regular.ttf", "/../../../fonts/material/LiberationMono-Regular.ttf" };
  for(const char *relative : paths){
   std::string candidate = std::string(editorRoot) + relative;
   FILE *f = fopen(candidate.c_str(), "rb");
   if(f){ fclose(f); mono = candidate; break; }
  }
 }
	// --- Ribbon icons: Lucide (embedded), a face of its own: its codepoints
	// share Font Awesome's private-use range. ---
	ImFontConfig lucideCfg;
	lucideCfg.FontDataOwnedByAtlas = false;
	lucideCfg.PixelSnapH = true;
	static const ImWchar lucideRanges[] = { 0xe000, 0xe5ff, 0 };
	gIconFont = io.Fonts->AddFontFromMemoryTTF((void *)lucide_font_data, lucide_font_size, fontSize, &lucideCfg, lucideRanges);

	// Code: JetBrains Mono (embedded).
	ImFontConfig codeCfg;
	codeCfg.FontDataOwnedByAtlas = false;
	gCodeFont = io.Fonts->AddFontFromMemoryTTF((void *)jetbrains_mono_font_data, jetbrains_mono_font_size, fontSize, &codeCfg);
 // Latin, Greek, Cyrillic, punctuation, arrows, maths and shapes. Line drawing
 // isn't here: the terminal draws it itself, so it joins up (terminal.cpp).
 static const ImWchar terminalRanges[] = {
  0x0020, 0x02FF, 0x0370, 0x04FF, 0x2000, 0x20CF, 0x2100, 0x22FF, 0x25A0, 0x25FF, 0,
 };
 gTerminalFont = mono.empty() ? gCodeFont : io.Fonts->AddFontFromFileTTF(mono.c_str(), fontSize, nullptr, terminalRanges);

	// --- Title font: the splash wordmark. Baked large because the backend
	// cannot render fonts at new sizes, so it is only ever scaled down; only
	// the wordmark's letters, to keep the atlas small ---
	static const ImWchar wordmarkRanges[] = {
		' ', ' ', 'H', 'H', 'S', 'S', 'a', 'a', 'd', 'e', 'i', 'i', 'm', 'm', 'o', 'o',
		't', 'u', 0,
	};
	ImFontConfig boldCfg;
	boldCfg.FontDataOwnedByAtlas = false;
	gBoldFont = io.Fonts->AddFontFromMemoryTTF(
		(void *)manrope_semibold_font_data, manrope_semibold_font_size,
		fontSize, &boldCfg);
	{
		// Its icons and other scripts, as the main face has them.
		ImFontConfig fallback;
		fallback.MergeMode = true;
		fallback.FontDataOwnedByAtlas = false;
		io.Fonts->AddFontFromMemoryTTF((void *)inter_font_data, inter_font_size, fontSize, &fallback);
		ImFontConfig icons = iconCfg;
		icons.MergeMode = true;
		io.Fonts->AddFontFromMemoryTTF((void *)fa_solid_font_data, fa_solid_font_size, fontSize - 1.0f, &icons, iconRanges);
	}
	ImFontConfig titleCfg;
	titleCfg.FontDataOwnedByAtlas = false;
	gTitleFont = io.Fonts->AddFontFromMemoryTTF(
		(void *)manrope_semibold_font_data, manrope_semibold_font_size,
		128.0f, &titleCfg, wordmarkRanges);
}
