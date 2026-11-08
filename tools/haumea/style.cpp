// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "icons.h"
#include "font_data.h"
#include "i18n.h"
#include <algorithm>
#include <string>
#include <cstdio>

ImFont *gCodeFont;
ImFont *gTitleFont;	// large Inter for the splash screen title

// Accent colors — used for highlights, active elements and selection.
static int gUiTheme = UI_THEME_DARK;
static const ImVec4 kDarkAccent       = ImVec4(0.02f, 0.78f, 0.78f, 1.00f);
static const ImVec4 kDarkAccentHover  = ImVec4(0.39f, 0.86f, 0.76f, 1.00f);
static const ImVec4 kDarkAccentActive = ImVec4(0.20f, 0.64f, 0.56f, 1.00f);
static const ImVec4 kLightAccent       = ImVec4(0.00f, 0.43f, 0.48f, 1.00f);
static const ImVec4 kLightAccentHover  = ImVec4(0.04f, 0.34f, 0.31f, 1.00f);
static const ImVec4 kLightAccentActive = ImVec4(0.06f, 0.31f, 0.29f, 1.00f);

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
	return gUiTheme == UI_THEME_LIGHT ? rw::RGBA{ 235, 239, 244, 255 } : rw::RGBA{ 31, 41, 48, 255 };
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

	// ---- Geometry ----
	style.WindowRounding    = 8.0f;
	style.ChildRounding     = 6.0f;
	style.FrameRounding     = 6.0f;
	style.PopupRounding     = 6.0f;
	style.ScrollbarRounding = 7.0f;
	style.GrabRounding      = 4.0f;
	style.TabRounding       = 4.0f;

	style.WindowPadding     = ImVec2(16, 14);
	style.FramePadding      = ImVec2(8, 5);
	style.ItemSpacing       = ImVec2(8, 6);
	style.ItemInnerSpacing  = ImVec2(6, 4);
	style.ScrollbarSize     = 12.0f;
	style.GrabMinSize       = 10.0f;
	style.WindowBorderSize  = 0.0f;
	style.ChildBorderSize   = 0.0f;
	style.PopupBorderSize   = 1.0f;
	style.FrameBorderSize   = 0.0f;
	style.TabBorderSize     = 0.0f;
	style.TabBarBorderSize  = 0.0f;
	style.TabBarOverlineSize = 0.0f;

	style.WindowTitleAlign  = ImVec2(0.0f, 0.5f);

#ifdef IMGUI_HAS_DOCK
	// Panels dock by dragging their title bar onto the edges of the
	// screen or onto another panel. Needs librw patched with
	// tools/release/patch_librw_haumea.py.
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#endif
	style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
	style.SeparatorTextPadding = ImVec2(0.0f, 4.0f);
	style.SeparatorTextBorderSize = 0.0f;

	// ---- Colors ----
	ImVec4 *c = style.Colors;

	// Backgrounds
	c[ImGuiCol_WindowBg]             = light ? ImVec4(0.96f, 0.97f, 0.98f, 1.00f) : ImVec4(0.075f, 0.095f, 0.115f, 1.00f);
	c[ImGuiCol_ChildBg]              = light ? ImVec4(0.93f, 0.95f, 0.96f, 0.00f) : ImVec4(0.090f, 0.105f, 0.125f, 0.00f);
	c[ImGuiCol_PopupBg]              = light ? ImVec4(0.99f, 0.995f, 1.00f, 1.00f) : ImVec4(0.105f, 0.122f, 0.145f, 0.98f);

	// Borders
	c[ImGuiCol_Border]               = light ? ImVec4(0.70f, 0.76f, 0.77f, 0.85f) : ImVec4(0.24f, 0.29f, 0.33f, 0.45f);
	c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

	// Text
	c[ImGuiCol_Text]                 = light ? ImVec4(0.13f, 0.16f, 0.17f, 1.00f) : ImVec4(0.93f, 0.95f, 0.96f, 1.00f);
	c[ImGuiCol_TextDisabled]         = light ? ImVec4(0.40f, 0.46f, 0.47f, 1.00f) : ImVec4(0.59f, 0.65f, 0.69f, 1.00f);

	// Frames (input boxes, sliders, checkboxes)
	c[ImGuiCol_FrameBg]              = light ? ImVec4(0.88f, 0.91f, 0.92f, 1.00f) : ImVec4(0.13f, 0.15f, 0.18f, 1.00f);
	c[ImGuiCol_FrameBgHovered]       = light ? ImVec4(0.83f, 0.88f, 0.89f, 1.00f) : ImVec4(0.18f, 0.21f, 0.24f, 1.00f);
	c[ImGuiCol_FrameBgActive]        = light ? ImVec4(0.78f, 0.84f, 0.84f, 1.00f) : ImVec4(0.22f, 0.26f, 0.29f, 1.00f);

	// Title bar
	c[ImGuiCol_TitleBg]              = light ? ImVec4(0.90f, 0.93f, 0.94f, 1.00f) : ImVec4(0.065f, 0.075f, 0.09f, 1.00f);
	c[ImGuiCol_TitleBgActive]        = light ? ImVec4(0.86f, 0.91f, 0.91f, 1.00f) : ImVec4(0.10f, 0.13f, 0.15f, 1.00f);
	c[ImGuiCol_TitleBgCollapsed]     = light ? ImVec4(0.90f, 0.93f, 0.94f, 0.70f) : ImVec4(0.065f, 0.075f, 0.09f, 0.70f);

	// Menu bar
	c[ImGuiCol_MenuBarBg]            = light ? ImVec4(0.91f, 0.94f, 0.95f, 1.00f) : ImVec4(0.09f, 0.105f, 0.125f, 1.00f);

	// Scrollbar
	c[ImGuiCol_ScrollbarBg]          = light ? ImVec4(0.90f, 0.93f, 0.94f, 0.80f) : ImVec4(0.08f, 0.095f, 0.11f, 0.80f);
	c[ImGuiCol_ScrollbarGrab]        = light ? ImVec4(0.68f, 0.73f, 0.74f, 1.00f) : ImVec4(0.24f, 0.29f, 0.32f, 1.00f);
	c[ImGuiCol_ScrollbarGrabHovered] = light ? ImVec4(0.58f, 0.66f, 0.67f, 1.00f) : ImVec4(0.34f, 0.41f, 0.43f, 1.00f);
	c[ImGuiCol_ScrollbarGrabActive]  = light ? ImVec4(0.45f, 0.60f, 0.59f, 1.00f) : ImVec4(0.42f, 0.52f, 0.51f, 1.00f);

	// Checkbox / Radio mark
	c[ImGuiCol_CheckMark]            = accent;

	// Slider
	c[ImGuiCol_SliderGrab]           = light ? ImVec4(0.50f, 0.58f, 0.59f, 1.00f) : ImVec4(0.37f, 0.46f, 0.48f, 1.00f);
	c[ImGuiCol_SliderGrabActive]     = accent;

	// Buttons
	c[ImGuiCol_Button]               = light ? ImVec4(0.88f, 0.91f, 0.92f, 1.00f) : ImVec4(0.14f, 0.17f, 0.19f, 1.00f);
	c[ImGuiCol_ButtonHovered]        = light ? ImVec4(0.81f, 0.87f, 0.88f, 1.00f) : ImVec4(0.20f, 0.25f, 0.27f, 1.00f);
	c[ImGuiCol_ButtonActive]         = accentActive;

	// Headers (collapsing headers, selectable, menu items)
	c[ImGuiCol_Header]               = light ? ImVec4(0.85f, 0.90f, 0.90f, 1.00f) : ImVec4(0.14f, 0.18f, 0.19f, 1.00f);
	c[ImGuiCol_HeaderHovered]        = light ? ImVec4(0.78f, 0.86f, 0.86f, 1.00f) : ImVec4(0.21f, 0.28f, 0.28f, 1.00f);
	c[ImGuiCol_HeaderActive]         = accentActive;

	// Separators
	c[ImGuiCol_Separator]            = light ? ImVec4(0.77f, 0.81f, 0.82f, 1.00f) : ImVec4(0.22f, 0.27f, 0.29f, 1.00f);
	c[ImGuiCol_SeparatorHovered]     = accentHover;
	c[ImGuiCol_SeparatorActive]      = accent;

	// Resize grip
	c[ImGuiCol_ResizeGrip]           = light ? ImVec4(0.45f, 0.65f, 0.61f, 0.40f) : ImVec4(0.22f, 0.32f, 0.31f, 0.40f);
	c[ImGuiCol_ResizeGripHovered]    = accentHover;
	c[ImGuiCol_ResizeGripActive]     = accent;

	// Tabs
	c[ImGuiCol_Tab]                  = light ? ImVec4(0.90f, 0.93f, 0.94f, 1.00f) : ImVec4(0.105f, 0.13f, 0.15f, 1.00f);
	c[ImGuiCol_TabHovered]           = light ? ImVec4(0.82f, 0.89f, 0.89f, 1.00f) : ImVec4(0.18f, 0.24f, 0.25f, 1.00f);
	c[ImGuiCol_TabSelected]          = light ? ImVec4(0.82f, 0.89f, 0.89f, 1.00f) : ImVec4(0.15f, 0.22f, 0.22f, 1.00f);

	c[ImGuiCol_TabDimmed] = c[ImGuiCol_Tab];
	c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabSelected];
	c[ImGuiCol_TabSelectedOverline] = accent;
	c[ImGuiCol_TabDimmedSelectedOverline] = accent;

	// Table
	c[ImGuiCol_TableHeaderBg]        = light ? ImVec4(0.90f, 0.93f, 0.94f, 1.00f) : ImVec4(0.11f, 0.14f, 0.16f, 1.00f);
	c[ImGuiCol_TableBorderStrong]    = light ? ImVec4(0.75f, 0.80f, 0.81f, 1.00f) : ImVec4(0.22f, 0.27f, 0.29f, 1.00f);
	c[ImGuiCol_TableBorderLight]     = light ? ImVec4(0.81f, 0.85f, 0.86f, 1.00f) : ImVec4(0.17f, 0.21f, 0.23f, 1.00f);
	c[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	c[ImGuiCol_TableRowBgAlt]        = light ? ImVec4(0.0f, 0.0f, 0.0f, 0.025f) : ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

	// Selection / Drag-drop
	c[ImGuiCol_TextSelectedBg]       = ImVec4(accent.x, accent.y, accent.z, 0.25f);
	c[ImGuiCol_DragDropTarget]       = accent;

	// Nav highlight
	c[ImGuiCol_NavHighlight]         = accent;
	c[ImGuiCol_NavWindowingHighlight]= light ? ImVec4(0.10f, 0.15f, 0.16f, 0.55f) : ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
	c[ImGuiCol_NavWindowingDimBg]    = light ? ImVec4(0.20f, 0.24f, 0.25f, 0.14f) : ImVec4(0.80f, 0.80f, 0.80f, 0.20f);

	// Modal dimming
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
	float fontSize = 15.0f;

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
		0xf043, 0xf043, // droplet
		0xf047, 0xf047, // arrows-up-down-left-right
		0xf04b, 0xf04b, // play
		0xf04d, 0xf04d, // stop
		0xf059, 0xf05a, // circle-question, circle-info
		0xf06e, 0xf06e, // eye
		0xf070, 0xf071, // eye-slash, triangle-exclamation
		0xf076, 0xf076, // magnet
		0xf07c, 0xf07c, // folder-open
		0xf0ac, 0xf0ad, // globe, wrench
		0xf0c1, 0xf0c1, // link
		0xf0c5, 0xf0c7, // copy, floppy-disk
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

	// --- Main font: Inter (embedded) ---
	ImFontConfig fontCfg;
	fontCfg.FontDataOwnedByAtlas = false;
	io.Fonts->AddFontFromMemoryTTF(
		(void *)inter_font_data, inter_font_size,
		fontSize, &fontCfg);

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
		// Latin Extended-A need baking: the defaults cover the rest, which is
		// why a catalog that only uses them changes nothing here.
		for(unsigned int cp : codepoints)
			if(cp >= 0x2E80)
				eastAsian = true;
		std::vector<unsigned int> baked;
		for(unsigned int cp : codepoints)
			if(cp >= 0x180)
				baked.push_back(cp);
		if(eastAsian){
			for(const ImWchar *r = io.Fonts->GetGlyphRangesChineseSimplifiedCommon(); r[0]; r += 2)
				for(unsigned int c = r[0]; c <= r[1]; c++)
					if(c >= 0x180)
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
 gCodeFont = mono.empty() ? io.Fonts->AddFontDefault() : io.Fonts->AddFontFromFileTTF(mono.c_str(), fontSize);

	// --- Title font: the splash wordmark. Baked large because the backend
	// cannot render fonts at new sizes, so it is only ever scaled down; only
	// the wordmark's letters, to keep the atlas small ---
	static const ImWchar wordmarkRanges[] = {
		' ', ' ', 'H', 'H', 'S', 'S', 'a', 'a', 'd', 'e', 'i', 'i', 'm', 'm', 'o', 'o',
		't', 'u', 0,
	};
	ImFontConfig titleCfg;
	titleCfg.FontDataOwnedByAtlas = false;
	gTitleFont = io.Fonts->AddFontFromMemoryTTF(
		(void *)inter_font_data, inter_font_size,
		128.0f, &titleCfg, wordmarkRanges);
}
