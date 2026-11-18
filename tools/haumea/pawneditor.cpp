// SPDX-License-Identifier: GPL-2.0-or-later
#include "pawneditor.h"
#include "TextEditor.h"
#include "encoding.h"
#include "haumea.h"
#include "i18n.h"
#include "pawnsyntax.h"
#include "workspace.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#ifdef _WIN32
#include <windows.h>
#endif

// Hi'iaka: the native Pawn document, independent of the map serializer.
namespace
{
TextEditor editor;
TextEditor::Language language;
bool ready, loaded, followMap = true, recovered;
std::string text, saved, file, diskText, error, sourceMap;
PawnTextEncoding encoding = PAWN_TEXT_UTF8, savedEncoding = PAWN_TEXT_UTF8;
uint32 sourceSeq = (uint32)-1;
int format = PAWN_CODE_NATIVE;
int pending = -1, afterSave = -1;
int fileDialog = -1;
char dialogPath[2048];
bool overwriteConfirmed;
double draftDue;
bool draftPending;

bool readFile(const std::string &path, std::string &out)
{
	std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
	if (!in)
		return false;
	out.assign(std::istreambuf_iterator<char>(in), {});
	return !in.bad();
}

bool writeFile(const std::string &path, const std::string &data)
{
	if (!EnsureParentDirectoriesForPath(path.c_str()))
		return false;
	const std::string temp = path + ".hi-iaka.tmp";
	{
		std::ofstream out(std::filesystem::u8path(temp), std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		out.write(data.data(), data.size());
		out.close();
		if (!out) {
			std::remove(temp.c_str());
			return false;
		}
	}
#ifdef _WIN32
	bool ok = MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	bool ok = std::rename(temp.c_str(), path.c_str()) == 0;
#endif
	if (!ok)
		std::remove(temp.c_str());
	return ok;
}

std::string draftPath()
{
	return std::string(GetConfigDir()) + "/pawn-ide/draft.pwn";
}
bool dirty()
{
	return loaded && (recovered || text != saved || encoding != savedEncoding);
}
void scheduleDraft()
{
	draftDue = ImGui::GetTime() + 0.75;
	draftPending = true;
}

void setDocument(const std::string &value, bool markSaved)
{
	text = value;
	editor.SetText(text);
	if (markSaved) {
		saved = text;
		savedEncoding = encoding;
	}
	loaded = true;
	scheduleDraft();
}

bool wordStart(ImWchar c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '@';
}
bool wordPart(ImWchar c)
{
	return wordStart(c) || (c >= '0' && c <= '9');
}

void setup()
{
	if (ready)
		return;
	ready = true;
	language = *TextEditor::Language::C();
	language.name = "Pawn";
	language.preprocess = 0;
	language.keywords.clear();
	language.declarations.clear();
	language.identifiers.clear();
	language.customTokenizer = [](TextEditor::Iterator start, TextEditor::Iterator end, TextEditor::Color &color) {
		auto it = start;
		if (it == end)
			return start;
		if (*it == '#') {
			++it;
			while (it != end && (*it == ' ' || *it == '\t'))
				++it;
			while (it != end && wordPart(*it))
				++it;
			color = TextEditor::Color::preprocessor;
			return it;
		}
		if (!wordStart(*it))
			return start;
		std::string word;
		while (it != end && wordPart(*it)) {
			word += (char)*it;
			++it;
		}
		auto next = it;
		while (next != end && (*next == ' ' || *next == '\t'))
			++next;
		switch (ClassifyPawnWord(word, next == end ? 0 : (char)*next)) {
		case PAWN_WORD_KEYWORD:
			color = TextEditor::Color::keyword;
			break;
		case PAWN_WORD_STORAGE:
		case PAWN_WORD_TAG:
		case PAWN_WORD_CONSTANT:
			color = TextEditor::Color::declaration;
			break;
		case PAWN_WORD_FUNCTION:
			color = TextEditor::Color::knownIdentifier;
			break;
		default:
			color = TextEditor::Color::identifier;
			break;
		}
		return it;
	};
	editor.SetLanguage(&language);
	auto palette = TextEditor::GetDarkPalette();
	auto col = [&](TextEditor::Color c, int r, int g, int b) { palette[(size_t)c] = IM_COL32(r, g, b, 255); };
	col(TextEditor::Color::text, 212, 212, 212);
	col(TextEditor::Color::keyword, 197, 134, 192);
	col(TextEditor::Color::declaration, 86, 156, 214);
	col(TextEditor::Color::preprocessor, 197, 134, 192);
	col(TextEditor::Color::number, 181, 206, 168);
	col(TextEditor::Color::string, 206, 145, 120);
	col(TextEditor::Color::identifier, 156, 220, 254);
	col(TextEditor::Color::knownIdentifier, 220, 220, 170);
	col(TextEditor::Color::comment, 106, 153, 85);
	col(TextEditor::Color::background, 30, 30, 30);
	col(TextEditor::Color::lineNumber, 133, 133, 133);
	editor.SetPalette(palette);
	editor.SetTabSize(4);
	editor.SetShowLineNumbersEnabled(true);
	editor.SetShowWhitespacesEnabled(false);
	editor.SetAutoIndentEnabled(true);
	editor.SetLineFoldingEnabled(true);
	editor.SetCompletePairedGlyphs(true);
	editor.SetFindReplaceEnabled(true);
	std::string draft;
	if (readFile(draftPath(), draft) && IsValidUtf8(draft)) {
		setDocument(draft, false);
		followMap = false;
		recovered = true;
	}
}

bool mapChanged()
{
	return sourceSeq != GetLatestChangeSeq() || sourceMap != GetPawnMapPath();
}

void generate()
{
	std::string code;
	if (BuildPawnCode(GetPawnMapPath(), (PawnCodeFormat)format, false, code) < 0) {
		error = _("Could not generate Pawn code.");
		return;
	}
	// BuildPawnCode returns UTF-8; encoding is applied only when saving.
	encoding = GetPawnTextEncoding();
	setDocument(code, true);
	sourceSeq = GetLatestChangeSeq();
	sourceMap = GetPawnMapPath();
	file.clear();
	diskText.clear();
	encoding = GetPawnTextEncoding();
	followMap = true;
	recovered = false;
	error.clear();
}

void chooseFile(bool saving)
{
	fileDialog = saving ? PAWN_EDITOR_SAVE_AS : PAWN_EDITOR_OPEN;
	std::string suggested = file;
	if (suggested.empty() && saving) {
		std::filesystem::path p = std::filesystem::u8path(GetPawnMapPath());
		suggested =
		    p.empty() ? "map-script.pwn" : (p.parent_path() / (p.stem().u8string() + ".script.pwn")).u8string();
	}
	snprintf(dialogPath, sizeof(dialogPath), "%s", suggested.c_str());
	overwriteConfirmed = false;
}

bool saveTo(const std::string &target)
{
	std::error_code ec1, ec2;
	const auto map = std::filesystem::weakly_canonical(std::filesystem::u8path(GetPawnMapPath()), ec1);
	const auto dest = std::filesystem::weakly_canonical(std::filesystem::u8path(target), ec2);
	if (IsInGameDir(target.c_str()) || (!ec1 && !ec2 && GetPawnMapPath()[0] &&
					    LogicalPathEquals(map.u8string().c_str(), dest.u8string().c_str()))) {
		error = _("Choose a separate script file. Saving the map regenerates its Pawn file.");
		return false;
	}
	std::string current;
	if (target == file && (!readFile(target, current) || current != diskText)) {
		error = _("This file changed on disk. Use Save As to keep your edits in another file.");
		return false;
	}
	bool lossy = false;
	std::string raw = EncodePawnFileText(text, encoding, &lossy);
	if (lossy) {
		error = _("Some characters cannot be saved as Windows-1252. Choose UTF-8.");
		return false;
	}
	if (!writeFile(target, raw)) {
		error = _("Could not save the script.");
		return false;
	}
	file = target;
	diskText = raw;
	saved = text;
	savedEncoding = encoding;
	followMap = false;
	recovered = false;
	error.clear();
	scheduleDraft();
	return true;
}

bool save(bool saveAs)
{
	if (saveAs || file.empty()) {
		chooseFile(true);
		return false;
	}
	return saveTo(file);
}

bool openFile(const char *path)
{
	std::string raw;
	if (!readFile(path, raw)) {
		error = _("Could not open the script.");
		return false;
	}
	setDocument(DecodePawnFileText(raw, &encoding), true);
	file = path;
	diskText = raw;
	followMap = false;
	recovered = false;
	error.clear();
	return true;
}

void perform(int action)
{
	if (action == PAWN_EDITOR_REFRESH)
		generate();
	else if (action == PAWN_EDITOR_NEW) {
		setDocument("", true);
		file.clear();
		diskText.clear();
		followMap = false;
		recovered = false;
		error.clear();
	} else if (action == PAWN_EDITOR_OPEN) {
		chooseFile(false);
	}
}
// The width a button with this label takes.
float buttonWidth(const char *label)
{
	return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// A dialog's main action: the theme's accent, white text.
bool accentButton(const char *label)
{
	ImVec4 fill = UiAccentFill();
	ImGui::PushStyleColor(ImGuiCol_Button, fill);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(fill.x * 1.12f, fill.y * 1.12f, fill.z * 1.12f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(fill.x * 0.9f, fill.y * 0.9f, fill.z * 0.9f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
	bool pressed = ImGui::Button(label);
	ImGui::PopStyleColor(4);
	return pressed;
}

// Moves to where these buttons end flush with the dialog's right edge: on the
// same line as what came before (sameLine), else on a line of their own.
void dialogRightEnd(const char *const *labels, int n, bool sameLine = true)
{
	float spacing = ImGui::GetStyle().ItemSpacing.x, width = spacing * (n - 1);
	for (int i = 0; i < n; i++)
		width += buttonWidth(labels[i]);
	float right = ImGui::GetWindowContentRegionMax().x;
	if (sameLine)
		ImGui::SameLine();
	if (ImGui::GetCursorPosX() < right - width)
		ImGui::SetCursorPosX(right - width);
}
} // namespace

// For Namaka (agentbridge.cpp): the same document the editor shows, with
// the editor's own checks. Replacing or reloading it discards unsaved text,
// so the bridge asks first.
const std::string &PawnEditorAgentText(void)
{
	setup();
	if (!loaded)
		generate();
	return text;
}
void PawnEditorAgentSetText(const std::string &value)
{
	setup();
	setDocument(value, false);
	followMap = false;
}
bool PawnEditorAgentRefresh(std::string *message)
{
	setup();
	generate();
	*message = error;
	return error.empty();
}
bool PawnEditorAgentOpen(const char *path, std::string *message)
{
	setup();
	bool ok = openFile(path);
	*message = error;
	return ok;
}
bool PawnEditorAgentSave(const char *path, std::string *message)
{
	setup();
	if (!loaded)
		generate();
	std::string target = path && path[0] ? path : file;
	if (target.empty()) {
		*message = "the script has no file yet: give a path";
		return false;
	}
	bool ok = saveTo(target);
	*message = error;
	return ok;
}
std::string PawnEditorAgentFile(void)
{
	return file;
}
bool PawnEditorAgentFollowsMap(void)
{
	return followMap;
}

bool PawnEditorHasUnsavedChanges(void)
{
	return dirty();
}
const std::string &PawnEditorText(void)
{
	return text;
}

void PawnEditorFlushDraft(void)
{
	if (!ready || !loaded)
		return;
	if (dirty()) {
		if (!writeFile(draftPath(), text)) {
			error = _("Could not preserve the code draft. Save the script before closing.");
			return;
		}
	} else
		std::remove(draftPath().c_str());
	draftPending = false;
}

void PawnEditorTick(void)
{
	if (draftPending && ImGui::GetTime() >= draftDue)
		PawnEditorFlushDraft();
}

void PawnEditorRequest(PawnEditorAction action)
{
	setup();
	if (!loaded)
		generate();
	if (action >= PAWN_EDITOR_UNDO) {
		switch (action) {
		case PAWN_EDITOR_UNDO:
			if (editor.CanUndo())
				editor.Undo();
			break;
		case PAWN_EDITOR_REDO:
			if (editor.CanRedo())
				editor.Redo();
			break;
		case PAWN_EDITOR_CUT:
			editor.Cut();
			break;
		case PAWN_EDITOR_COPY:
			editor.Copy();
			break;
		case PAWN_EDITOR_PASTE:
			editor.Paste();
			break;
		case PAWN_EDITOR_SELECT_ALL:
			editor.SelectAll();
			break;
		default:
			break;
		}
		const std::string value = editor.GetText();
		if (value != text) {
			text = value;
			followMap = false;
			scheduleDraft();
		}
		editor.SetFocus();
	} else if (action == PAWN_EDITOR_SAVE || action == PAWN_EDITOR_SAVE_AS)
		save(action == PAWN_EDITOR_SAVE_AS);
	else if (dirty())
		pending = action;
	else
		perform(action);
}

void PawnEditorFindReplace(void)
{
	editor.OpenFindReplaceWindow();
}

int PawnEditorFormat(void)
{
	return format;
}

void PawnEditorSetFormat(int newFormat)
{
	if (newFormat == format)
		return;
	format = newFormat;
	PawnEditorRequest(PAWN_EDITOR_REFRESH);
}

int PawnEditorEncoding(void)
{
	return (int)encoding;
}

void PawnEditorSetEncoding(int enc)
{
	if (enc == (int)encoding)
		return;
	encoding = (PawnTextEncoding)enc;
	// Keep Haumea's optional encoding header consistent with the written bytes.
	const std::string prefix = "// Text encoding: ";
	size_t header = text.find(prefix);
	if (header != std::string::npos && (header == 0 || text[header - 1] == '\n')) {
		size_t end = text.find('\n', header);
		std::string value = text;
		value.replace(header + prefix.size(),
			      (end == std::string::npos ? text.size() : end) - header - prefix.size(),
			      PawnTextEncodingName(encoding));
		setDocument(value, false);
	}
	followMap = false;
	scheduleDraft();
}

std::string PawnEditorDocumentName(void)
{
	std::string name = file.empty() ? (recovered ? _("Recovered draft") : _("Map code")) : file;
	if (dirty())
		name += " *";
	return name;
}

void PawnEditorDraw(void)
{
	setup();
	if (!loaded || (followMap && mapChanged()))
		generate();
	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	ImGui::Begin("###PawnIDE", nullptr,
		     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
			 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
	// Its commands are on the ribbon (PawnEditorSetFormat and the rest below).
	// Over the code: why it no longer follows the map, or what went wrong.
	if (!followMap && mapChanged())
		ImGui::TextDisabled("%s", _("Map changed. Refresh from map to generate its latest code."));
	if (!error.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(UI_COLOR_ERROR));
		ImGui::TextWrapped("%s", error.c_str());
		ImGui::PopStyleColor();
	}
	editor.SetFindButtonLabel(_("Find"));
	editor.SetFindAllButtonLabel(_("Find all"));
	editor.SetReplaceButtonLabel(_("Replace"));
	editor.SetReplaceAllButtonLabel(_("Replace all"));
	const float statusHeight = ImGui::GetTextLineHeightWithSpacing();
	ImGui::PushFont(gCodeFont);
	bool changed = editor.Render("##PawnDocument", ImVec2(0, -statusHeight));
	ImGui::PopFont();
	if (changed) {
		std::string value = editor.GetText();
		if (value != text) {
			text = value;
			followMap = false;
			scheduleDraft();
		}
	}
	// The status line: the cursor's place and whether the code is saved.
	const auto pos = editor.GetMainCursorPosition();
	ImGui::TextDisabled(_("Pawn | Line %d, column %d | %s"), (int)pos.line + 1, (int)pos.index + 1,
			    dirty() ? _("Unsaved changes") : _("Saved"));
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
	    (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) && ImGui::IsKeyPressed(ImGuiKey_S))
		PawnEditorRequest(ImGui::GetIO().KeyShift ? PAWN_EDITOR_SAVE_AS : PAWN_EDITOR_SAVE);

	// Replacing an edited document: the question, Discard at the left, Cancel
	// and Save (the accent) at the right.
	const float fs = ImGui::GetFontSize();
	if (pending >= 0)
		ImGui::OpenPopup(_("Unsaved code"));
	ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 24.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
	if (ImGui::BeginPopupModal(_("Unsaved code"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted(_("Save your code before replacing this document?"));
		ImGui::Spacing();
		if (ImGui::Button(_("Discard"))) {
			int action = pending;
			pending = -1;
			perform(action);
			ImGui::CloseCurrentPopup();
		}
		const char *right[] = {_("Cancel"), _("Save")};
		dialogRightEnd(right, 2);
		if (ImGui::Button(_("Cancel"))) {
			pending = -1;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (accentButton(_("Save"))) {
			int action = pending;
			pending = -1;
			ImGui::CloseCurrentPopup();
			if (save(false))
				perform(action);
			else if (fileDialog >= 0)
				afterSave = action;
			else
				pending = action;
		}
		ImGui::EndPopup();
	}

	// Saving or opening a script: the path (Browse at its end), replacing a
	// file when it asks, then Cancel and Save/Open at the right.
	if (fileDialog >= 0)
		ImGui::OpenPopup(_("Script file"));
	ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 34.0f, 0.0f), ImVec2(fs * 34.0f, FLT_MAX));
	if (ImGui::BeginPopupModal(_("Script file"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		bool saving = fileDialog == PAWN_EDITOR_SAVE_AS;
		ImGui::TextDisabled("%s", saving ? _("Save script as") : _("Open script"));
		ImGui::Spacing();
		{
			char pathId[64];
			UiFieldLabel(_("Path"), pathId, sizeof(pathId));
			const char *browse = _("Browse...");
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth(browse) -
						ImGui::GetStyle().ItemSpacing.x);
			if (ImGui::InputText(pathId, dialogPath, sizeof(dialogPath)))
				overwriteConfirmed = false;
			ImGui::SameLine();
			if (ImGui::Button(browse)) {
				char picked[2048] = {};
				if (PickPawnFile(picked, sizeof(picked), saving, dialogPath)) {
					snprintf(dialogPath, sizeof(dialogPath), "%s", picked);
					overwriteConfirmed = saving; // Native save dialogs already confirm replacement.
				}
			}
		}
		std::error_code ec;
		bool exists = std::filesystem::exists(std::filesystem::u8path(dialogPath), ec);
		bool replacing = saving && exists && file != dialogPath && !overwriteConfirmed;
		if (replacing)
			UiCheckbox(_("Replace existing file"), &overwriteConfirmed);
		if (!error.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(UI_COLOR_ERROR));
			ImGui::TextWrapped("%s", error.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::Spacing();
		const char *primary = saving ? _("Save") : _("Open");
		const char *right[] = {_("Cancel"), primary};
		dialogRightEnd(right, 2, false);
		if (ImGui::Button(_("Cancel"))) {
			fileDialog = -1;
			afterSave = -1;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!dialogPath[0] || (replacing && !overwriteConfirmed));
		if (accentButton(primary)) {
			if (saving ? saveTo(dialogPath) : openFile(dialogPath)) {
				fileDialog = -1;
				ImGui::CloseCurrentPopup();
				if (afterSave >= 0) {
					int action = afterSave;
					afterSave = -1;
					perform(action);
				}
			}
		}
		ImGui::EndDisabled();
		ImGui::EndPopup();
	}

	ImGui::End();
}
