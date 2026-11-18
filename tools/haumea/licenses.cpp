// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "i18n.h"
#include "imgui/imgui_internal.h"

#include <stdio.h>
#include <string.h>
#include <string>

// Help > About > Licenses: LICENSE and THIRD_PARTY_NOTICES.md, which every
// release ships next to the editor. A build run from the source tree finds
// them a few folders up (bin/<platform>/Release/haumea).

static const char *licenseFiles[] = {
	"LICENSE", "THIRD_PARTY_NOTICES.md", "fonts/material/LICENSE-Liberation.txt", "fonts/LICENSE-NotoSansSC.txt"
};
static const int NUM_LICENSE_FILES = sizeof(licenseFiles)/sizeof(licenseFiles[0]);

static bool
readLicenseFile(const char *name, std::string &text)
{
	char dir[1024];
	if(!GetEditorRootDirectory(dir, sizeof(dir)))
		return false;
	std::string base = dir;
	for(int up = 0; up < 5; up++){
		char path[1200];
		snprintf(path, sizeof(path), "%s/%s", base.c_str(), name);
		FILE *f = fopen(path, "rb");
		if(f){
			char buf[8192];
			size_t n;
			text.clear();
			while((n = fread(buf, 1, sizeof(buf), f)) > 0)
				text.append(buf, n);
			fclose(f);
			return true;
		}
		size_t slash = base.find_last_of("/\\");
		if(slash == std::string::npos)
			break;
		base.resize(slash);
	}
	return false;
}

// Read the licenses Haumea ships under: pick a file, search it, read it.
void
DrawLicensesWindow(bool *open)
{
	static std::string texts[NUM_LICENSE_FILES];
	static bool loaded, found[NUM_LICENSE_FILES];
	static int current;
	static char search[128];
	if(!loaded){
		for(int i = 0; i < NUM_LICENSE_FILES; i++)
			found[i] = readLicenseFile(licenseFiles[i], texts[i]);
		loaded = true;
	}
	const char *title = _("Licenses###licenses");
	ImGuiWindowFlags placed = UiPlaceToolWindow(title, 42.0f, false);
	if(!ImGui::Begin(title, open, placed)){
		ImGui::End();
		return;
	}
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", _("Haumea Studio is free software under the GNU General Public License, version 2 "
		"or any later version. It includes components under their own licenses."));
	ImGui::PopStyleColor();
	ImGui::Spacing();

	// The toolbar: the search filling the row, the file at its right end.
	float spacing = ImGui::GetStyle().ItemSpacing.x;
	float fileWidth = floorf(ImGui::GetFontSize() * 13.0f);
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - fileWidth - spacing);
	ImGui::InputTextWithHint("##license_search", _("Search"), search, sizeof(search));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(fileWidth);
	if(ImGui::BeginCombo("##license_files", licenseFiles[current])){
		for(int i = 0; i < NUM_LICENSE_FILES; i++){
			if(ImGui::Selectable(licenseFiles[i], current == i))
				current = i;
			if(current == i)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	// The text filling the rest; while searching, only the lines that match.
	ImGui::BeginChild("##license_text", ImVec2(0, 0), ImGuiChildFlags_Borders);
	const std::string &text = texts[current];
	if(!found[current])
		ImGui::TextWrapped(_("%s isn't next to the editor. It ships with every release, and is "
			"in the source at https://github.com/itsneufox/Haumea-Studio."), licenseFiles[current]);
	else if(search[0] == '\0')
		ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
	else{
		const char *at = text.c_str(), *end = at + text.size();
		const char *needleEnd = search + strlen(search);
		bool any = false;
		while(at < end){
			const char *eol = (const char*)memchr(at, '\n', end - at);
			if(eol == nil)
				eol = end;
			if(ImStristr(at, eol, search, needleEnd)){
				ImGui::TextUnformatted(at, eol);
				any = true;
			}
			at = eol + 1;
		}
		if(!any)
			ImGui::TextDisabled("%s", _("Nothing matches."));
	}
	ImGui::EndChild();
	ImGui::End();
}
