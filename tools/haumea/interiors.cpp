// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "i18n.h"
#include "icons.h"
#include "interiors.h"
#include "workspace.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

// The game's interiors, from the entry/exit markers in its text IPLs
// ("enex"). A marker inside an interior (its area is not 0) has the point
// where the game puts a player who comes in through that door; its name is a
// GXT key for the place's name.
//
// enex: x1, y1, z1, angle, sizex, sizey, sizez, x2, y2, z2, exitangle,
//       area, flags, "name", sky, numpeds, timeon, timeoff

struct GameInterior {
	int id;
	std::string key;	// GXT key from the IPL
	std::string name;	// its American text, or the key
	rw::V3d pos;	// where a player coming in stands
	float angle;
};

static std::vector<GameInterior> gInteriors;
static bool gInteriorsLoaded;

static std::vector<std::string>
listDir(const std::string &dir)
{
	std::vector<std::string> names;
#ifdef _WIN32
	WIN32_FIND_DATAA data;
	HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &data);
	if(h == INVALID_HANDLE_VALUE)
		return names;
	do{
		if(strcmp(data.cFileName, ".") != 0 && strcmp(data.cFileName, "..") != 0)
			names.push_back(data.cFileName);
	}while(FindNextFileA(h, &data));
	FindClose(h);
#else
	DIR *d = opendir(dir.c_str());
	if(d == nil)
		return names;
	while(struct dirent *e = readdir(d))
		if(strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
			names.push_back(e->d_name);
	closedir(d);
#endif
	std::sort(names.begin(), names.end());
	return names;
}

static void
findIpls(const std::string &dir, int depth, std::vector<std::string> &out)
{
	std::vector<std::string> names = listDir(dir);
	for(size_t i = 0; i < names.size(); i++){
		std::string path = dir + "/" + names[i];
		size_t n = names[i].size();
		if(n > 4 && rw::strncmp_ci(names[i].c_str() + n - 4, ".ipl", 4) == 0)
			out.push_back(path);
		else if(depth > 0 && names[i].find('.') == std::string::npos)
			findIpls(path, depth - 1, out);
	}
}

// ---- GXT (SA: version 4, 8-bit text, keys hashed with CRC32) --------------

static uint32
gxtKeyHash(const char *key)
{
	uint32 crc = 0xFFFFFFFF;
	for(const char *p = key; *p; p++){
		crc ^= (uint8)toupper((unsigned char)*p);
		for(int i = 0; i < 8; i++)
			crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
	}
	return crc;
}

static uint32
readU32(const std::vector<uint8> &d, size_t at)
{
	return at + 4 <= d.size() ? d[at] | d[at+1] << 8 | d[at+2] << 16 | (uint32)d[at+3] << 24 : 0;
}

// The MAIN table of an SA .gxt, hash -> text.
static std::unordered_map<uint32, std::string>
loadGxtMain(const std::string &path)
{
	std::unordered_map<uint32, std::string> texts;
	FILE *f = fopen(path.c_str(), "rb");
	if(f == nil)
		return texts;
	std::vector<uint8> d;
	uint8 buf[65536];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), f)) > 0)
		d.insert(d.end(), buf, buf + n);
	fclose(f);
	if(d.size() < 12 || memcmp(&d[4], "TABL", 4) != 0)
		return texts;
	uint32 tableSize = readU32(d, 8);
	size_t main = 0;
	for(size_t at = 12; at + 12 <= 12 + tableSize && at + 12 <= d.size(); at += 12)
		if(memcmp(&d[at], "MAIN\0", 5) == 0)
			main = readU32(d, at + 8);
	if(main == 0 || main + 8 > d.size() || memcmp(&d[main], "TKEY", 4) != 0)
		return texts;
	uint32 keySize = readU32(d, main + 4);
	size_t data = main + 8 + keySize;
	if(data + 8 > d.size() || memcmp(&d[data], "TDAT", 4) != 0)
		return texts;
	size_t base = data + 8;
	for(size_t at = main + 8; at + 8 <= main + 8 + keySize; at += 8){
		size_t text = base + readU32(d, at);
		std::string s;
		while(text < d.size() && d[text])
			s += (char)d[text++];
		texts[readU32(d, at + 4)] = s;
	}
	return texts;
}

// ---- loading --------------------------------------------------------------

static std::vector<std::string>
splitFields(const std::string &line)
{
	std::vector<std::string> fields;
	std::string field;
	for(size_t i = 0; i <= line.size(); i++){
		if(i == line.size() || line[i] == ','){
			size_t b = field.find_first_not_of(" \t\""), e = field.find_last_not_of(" \t\r\"");
			fields.push_back(b == std::string::npos ? "" : field.substr(b, e - b + 1));
			field.clear();
		}else
			field += line[i];
	}
	return fields;
}

static void
loadInteriors(void)
{
	gInteriorsLoaded = true;
	gInteriors.clear();
	std::unordered_map<uint32, std::string> gxt = loadGxtMain(GamePath("text/american.gxt"));
	std::vector<std::string> ipls;
	findIpls(GamePath("data/maps"), 3, ipls);
	for(size_t i = 0; i < ipls.size(); i++){
		FILE *f = fopen(ipls[i].c_str(), "r");
		if(f == nil)
			continue;
		char line[1024];
		bool inEnex = false;
		while(fgets(line, sizeof(line), f)){
			if(rw::strncmp_ci(line, "enex", 4) == 0){
				inEnex = true;
				continue;
			}
			if(rw::strncmp_ci(line, "end", 3) == 0){
				inEnex = false;
				continue;
			}
			if(!inEnex || line[0] == '#')
				continue;
			std::vector<std::string> v = splitFields(line);
			if(v.size() < 14)
				continue;
			GameInterior in;
			in.id = atoi(v[11].c_str());
			if(in.id <= 0)
				continue;	// a door in the outside world
			in.pos.x = (float)atof(v[7].c_str());
			in.pos.y = (float)atof(v[8].c_str());
			in.pos.z = (float)atof(v[9].c_str());
			in.angle = fmodf((float)atof(v[10].c_str()), 360.0f);
			if(in.angle < 0.0f)
				in.angle += 360.0f;
			in.key = v[13];
			std::unordered_map<uint32, std::string>::iterator it = gxt.find(gxtKeyHash(in.key.c_str()));
			in.name = it != gxt.end() && !it->second.empty() ? it->second : in.key;
			// Several doors into one place: keep the first.
			bool duplicate = false;
			for(size_t j = 0; j < gInteriors.size() && !duplicate; j++){
				const GameInterior &o = gInteriors[j];
				duplicate = o.id == in.id && o.key == in.key &&
					fabsf(o.pos.x - in.pos.x) < 30.0f && fabsf(o.pos.y - in.pos.y) < 30.0f &&
					fabsf(o.pos.z - in.pos.z) < 10.0f;
			}
			if(!duplicate)
				gInteriors.push_back(in);
		}
		fclose(f);
	}
	std::stable_sort(gInteriors.begin(), gInteriors.end(), [](const GameInterior &a, const GameInterior &b){
		return a.id != b.id ? a.id < b.id : a.name < b.name;
	});
	log("Interiors: %d from %d IPL file(s)\n", (int)gInteriors.size(), (int)ipls.size());
}

static bool
matches(const GameInterior &in, const char *filter)
{
	if(filter[0] == '\0')
		return true;
	char id[16];
	snprintf(id, sizeof(id), "%d", in.id);
	std::string hay = std::string(id) + " " + in.name + " " + in.key;
	std::string needle = filter;
	std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
	std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
	return hay.find(needle) != std::string::npos;
}

static void
goToInterior(const GameInterior &in)
{
	// Behind and above the spawn point, looking at it.
	float a = in.angle * 3.14159265f / 180.0f;
	rw::V3d forward = { -sinf(a), cosf(a), 0.0f };
	TheCamera.m_target = in.pos;
	TheCamera.m_position.x = in.pos.x - forward.x * 4.0f;
	TheCamera.m_position.y = in.pos.y - forward.y * 4.0f;
	TheCamera.m_position.z = in.pos.z + 2.0f;
	if(params.numAreas)
		currentArea = in.id;
}

static std::string
interiorPawn(const GameInterior &in)
{
	char code[256];
	snprintf(code, sizeof(code),
		"SetPlayerInterior(playerid, %d); // %s\n"
		"SetPlayerPos(playerid, %.4f, %.4f, %.4f);\n"
		"SetPlayerFacingAngle(playerid, %.1f);\n",
		in.id, in.name.c_str(), in.pos.x, in.pos.y, in.pos.z, in.angle);
	return code;
}

void
InteriorsWindow(bool *open)
{
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, ImGui::GetFontSize() * 30.0f), ImGuiCond_FirstUseEver);
	if(!ImGui::Begin(LI(ICON_FA_MAP_PIN, "Interiors"), open)){
		ImGui::End();
		return;
	}
	UiFitItemsToPanel();
	if(!gInteriorsLoaded)
		loadInteriors();
	static char filter[64];
	ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4.0f);
	ImGui::InputTextWithHint("##interiorsearch", TI(ICON_FA_MAGNIFYING_GLASS, "Name, key or ID"), filter, sizeof(filter));
	ImGui::SetItemTooltip(_("Click an interior to go there and show it. Right-click to copy it as Pawn."));
	ImGui::SameLine();
	if(ImGui::Button(ICON_FA_ROTATE "##reloadinteriors"))
		loadInteriors();
	ImGui::SetItemTooltip(_("Read the interiors from the game files again."));
	if(gInteriors.empty())
		ImGui::TextDisabled(_("No interiors found in the game's data/maps IPL files."));

	if(ImGui::BeginTable("##interiors", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
	                     ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)){
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(_("ID"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.0f);
		ImGui::TableSetupColumn(_("Name"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn(_("Position"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableHeadersRow();
		for(size_t i = 0; i < gInteriors.size(); i++){
			const GameInterior &in = gInteriors[i];
			if(!matches(in, filter))
				continue;
			ImGui::PushID((int)i);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			char id[16];
			snprintf(id, sizeof(id), "%d", in.id);
			bool here = params.numAreas && currentArea == in.id &&
				fabsf(TheCamera.m_target.x - in.pos.x) < 0.01f && fabsf(TheCamera.m_target.y - in.pos.y) < 0.01f;
			if(ImGui::Selectable(id, here, ImGuiSelectableFlags_SpanAllColumns))
				goToInterior(in);
			if(ImGui::BeginPopupContextItem()){
				if(ImGui::MenuItem(TI(ICON_FA_COPY, "Copy as Pawn")))
					ImGui::SetClipboardText(interiorPawn(in).c_str());
				if(ImGui::MenuItem(TI(ICON_FA_LOCATION_DOT, "Go there")))
					goToInterior(in);
				ImGui::EndPopup();
			}
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(in.name.c_str());
			if(in.name != in.key){
				// Many places share a name ("House"); the key tells them apart.
				ImGui::SameLine();
				ImGui::TextDisabled("%s", in.key.c_str());
			}
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%.1f, %.1f, %.1f", in.pos.x, in.pos.y, in.pos.z);
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::End();
}
