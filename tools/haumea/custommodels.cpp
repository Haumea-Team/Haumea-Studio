// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "autocol.h"
#include "workspace.h"
#include "categories.h"
#include "i18n.h"
#include "serverartwork.h"

#include <algorithm>
#include <regex>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <sys/stat.h>
#include <ctime>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#else
#include <dirent.h>
#endif

// Custom models (open.mp, SA-MP 0.3.DL): DFF and TXD files in the server's
// models folder, registered by AddSimpleModel/AddCharModel calls. A map
// registers the ones it uses in its own code, as open.mp scripts do; Haumea
// keeps the workspace's models in models/haumea_models.txt (the same calls,
// which servers don't read) so they show before any map uses them. A
// server's artconfig.txt, SA-MP's way, is read but never written. Nothing is
// copied into the game.

static const char *const CUSTOM_MODELS_LIST = "haumea_models.txt";
// In a session's model list: the models before it are the host's artconfig.txt.
static const char *const SESSION_OWN_MODELS = "// models the maps register\n";

// The same patterns open.mp's CustomModels component reads artconfig.txt with.
static const std::regex rAddCharModel(R"(AddCharModel\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*\"(.+?)\"\s*,\s*\"(.+?)\"\s*\))");
static const std::regex rAddSimpleModel(R"(AddSimpleModel\s*\(\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(-\d+)\s*,\s*\"(.+?)\"\s*,\s*\"(.+?)\"\s*\))");
static const std::regex rAddSimpleModelTimed(R"(AddSimpleModelTimed\s*\(\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(-\d+)\s*,\s*\"(.+?)\"\s*,\s*\"(.+?)\"\s*,\s*(\d+)\s*,\s*(\d+)\s*\))");

struct CustomModel {
	int editorId;
	std::string dffPath;
	// As the server lists it, for a multiplayer session's copy.
	int pawnId, baseId;
	std::string txdPath;
	bool timed, character;
	int timeOn, timeOff;
	// Listed in the server's artconfig.txt (SA-MP's way): registered there,
	// so a map leaves it out of its own AddSimpleModel calls.
	bool inArtconfig;
};

static std::string gServerDir;	// the server folder holding models/
static std::string gModelsDir;	// artwork.models_path / artpath, or models/
static std::vector<CustomModel> gModels;
static std::map<int, std::string> gTxdPaths;	// txd slot -> file
static std::map<std::string, int> gTxdSlots;	// file -> txd slot
static std::string gLastMapPath;
static std::map<int, int> gLegacyRemap;	// old modloader/Haumea ID -> editor ID
static std::string gLookupDir;	// where model files are looked up instead of gModelsDir
// Custom models whose DFF carries no collision: Haumea makes one from the
// triangles so they can be clicked and stood on here, but in the game
// nothing collides with them.
static std::set<int> gGeneratedCollision;
// Size and time of each model's DFF and TXD when read, to notice a rebuild.
struct FileStamp { long long size, time; };
static std::map<std::string, FileStamp> gFileStamps;

int
PawnModelId(int editorId)
{
	return editorId > CUSTOM_MODEL_BASE ? CUSTOM_MODEL_BASE - editorId : editorId;
}

int
EditorModelId(int pawnId)
{
	return pawnId <= -1000 && pawnId >= -30000 ? CUSTOM_MODEL_BASE - pawnId : pawnId;
}

bool
IsCustomModel(int editorId)
{
	for(size_t i = 0; i < gModels.size(); i++)
		if(gModels[i].editorId == editorId)
			return true;
	return false;
}

void
ListCustomSimpleModels(std::vector<int> &pawnIds)
{
	pawnIds.clear();
	for(size_t i = 0; i < gModels.size(); i++)
		if(gModels[i].editorId > CUSTOM_MODEL_BASE)
			pawnIds.push_back(PawnModelId(gModels[i].editorId));
	std::sort(pawnIds.begin(), pawnIds.end(), [](int a, int b){ return a > b; });
}

static bool
isDir(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}

static bool
isFile(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFREG);
}

// The one place that decides the server folder: the workspace, or else the
// open map's folder or one above it that has a models/ or gamemodes/ folder.
static std::string
customModelsServerDir(const char *mapPath)
{
	if(GetWorkspaceDir()[0])
		return GetWorkspaceDir();
	if(mapPath == nil || mapPath[0] == '\0')
		return "";
	std::string dir = mapPath;
	for(size_t i = 0; i < dir.size(); i++)
		if(dir[i] == '\\')
			dir[i] = '/';
	size_t slash = dir.find_last_of('/');
	dir = slash == std::string::npos ? "." : dir.substr(0, slash);
	for(int depth = 0; depth < 8 && !dir.empty(); depth++){
		if(isFile(dir + "/models/artconfig.txt") || isFile(dir + "/models/" + CUSTOM_MODELS_LIST) ||
		   isDir(dir + "/gamemodes"))
			return dir;
		slash = dir.find_last_of('/');
		if(slash == std::string::npos || slash == 0)
			break;
		dir.resize(slash);
	}
	return "";
}

const char*
GetCustomModelsDir(void)
{
	return gModelsDir.empty() ? nil : gModelsDir.c_str();
}

// open.mp runs on Linux too, where a server's file names must match; the
// client doesn't care. Look the name up without case if it isn't there as is.
static std::string
findModelFile(const std::string &name)
{
	const std::string &base = gLookupDir.empty() ? gModelsDir : gLookupDir;
	std::string path = base + "/" + name;
	if(isFile(path))
		return path;
#ifndef _WIN32
	std::string dir = base;
	std::string file = name;
	size_t slash = name.find_last_of("/\\");
	if(slash != std::string::npos){
		dir += "/" + name.substr(0, slash);
		file = name.substr(slash + 1);
	}
	DIR *d = opendir(dir.c_str());
	if(d){
		struct dirent *e;
		while((e = readdir(d)) != nil)
			if(rw::strcmp_ci(e->d_name, file.c_str()) == 0){
				std::string found = dir + "/" + e->d_name;
				closedir(d);
				return found;
			}
		closedir(d);
	}
#endif
	return "";
}

const char*
GetCustomModelDffPath(int editorId)
{
	for(size_t i = 0; i < gModels.size(); i++)
		if(gModels[i].editorId == editorId)
			return gModels[i].dffPath.c_str();
	return nil;
}

const char*
GetCustomModelTxdPath(int txdSlot)
{
	std::map<int, std::string>::iterator it = gTxdPaths.find(txdSlot);
	return it == gTxdPaths.end() ? nil : it->second.c_str();
}

const char*
GetCustomModelTxdName(int txdSlot)
{
	static std::map<int, std::string> names;
	std::map<int, std::string>::iterator it = gTxdPaths.find(txdSlot);
	if(it == gTxdPaths.end())
		return nil;
	std::string &name = names[txdSlot];
	if(name.empty()){
		name = it->second;
		size_t slash = name.find_last_of("/\\");
		if(slash != std::string::npos)
			name = name.substr(slash + 1);
		size_t dot = name.find_last_of('.');
		if(dot != std::string::npos)
			name.resize(dot);
	}
	return name.c_str();
}

int
FindCustomModelTxdSlot(const char *name)
{
	if(name == nil || name[0] == '\0')
		return -1;
	for(std::map<int, std::string>::iterator it = gTxdPaths.begin(); it != gTxdPaths.end(); ++it)
		if(rw::strncmp_ci(GetCustomModelTxdName(it->first), name, 1024) == 0)
			return it->first;
	return -1;
}

static int
txdSlotForFile(const std::string &path)
{
	std::map<std::string, int>::iterator it = gTxdSlots.find(path);
	if(it != gTxdSlots.end())
		return it->second;
	// Custom TXDs get slots of their own so they never share a stock name.
	char name[MODELNAMELEN];
	snprintf(name, sizeof(name), "~custom%d", (int)gTxdSlots.size());
	int slot = AddTxdSlot(name);
	gTxdSlots[path] = slot;
	gTxdPaths[slot] = path;
	return slot;
}

static bool
serverTexturePath(const std::string &relative, std::string &path)
{
	std::string name = relative;
	std::replace(name.begin(), name.end(), '\\', '/');
	if(gModelsDir.empty() || name.empty() || name[0] == '/' || name.find(':') != std::string::npos ||
	   name.size() < 5 || rw::strcmp_ci(name.c_str() + name.size() - 4, ".txd") != 0)
		return false;
	for(size_t at = 0; at < name.size(); ){
		size_t end = name.find('/', at);
		if(name.substr(at, end == std::string::npos ? end : end-at) == "..") return false;
		if(end == std::string::npos) break;
		at = end + 1;
	}
	path = findModelFile(name);
	return !path.empty();
}

int
LoadServerTextureTxd(const std::string &relativePath)
{
	std::string path;
	if(!serverTexturePath(relativePath, path)) return -1;
	int slot = txdSlotForFile(path);
	if(!IsTxdLoaded(slot)) LoadTxd(slot);
	return IsTxdLoaded(slot) ? slot : -1;
}

std::string
ServerTexturePathForModel(int pawnId)
{
	ObjectDef *obj = GetObjectDef(EditorModelId(pawnId));
	if(pawnId > -1000 || pawnId < -30000 || !IsCustomModel(EditorModelId(pawnId)) || obj == nil)
		return "";
	const char *path = GetCustomModelTxdPath(obj->m_txdSlot);
	std::string prefix = gModelsDir + "/";
	return path && std::string(path).compare(0, prefix.size(), prefix) == 0 ?
		std::string(path + prefix.size()) : "";
}

int
ServerTextureModelId(const std::string &relativePath)
{
	std::string path;
	if(!serverTexturePath(relativePath, path)) return 0;
	for(const CustomModel &model : gModels){
		int id = PawnModelId(model.editorId);
		if(id > -1000 || id < -30000) continue;
		ObjectDef *obj = GetObjectDef(model.editorId);
		const char *txd = obj ? GetCustomModelTxdPath(obj->m_txdSlot) : nil;
		if(txd && path == txd) return id;
	}
	return 0;
}

static void
listServerTextures(const std::string &relative, std::vector<std::string> &files, int depth)
{
	if(depth > 16) return;
	std::string dir = gModelsDir + "/" + relative;
#ifdef _WIN32
	WIN32_FIND_DATAA entry;
	HANDLE handle = FindFirstFileA((dir + "*").c_str(), &entry);
	if(handle == INVALID_HANDLE_VALUE) return;
	do{
		std::string name = entry.cFileName;
		if(name == "." || name == ".." || (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
		if(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			listServerTextures(relative + name + "/", files, depth + 1);
		else if(name.size() > 4 && rw::strcmp_ci(name.c_str() + name.size()-4, ".txd") == 0)
			files.push_back(relative + name);
	}while(FindNextFileA(handle, &entry));
	FindClose(handle);
#else
	DIR *handle = opendir(dir.c_str());
	if(handle == nil) return;
	while(struct dirent *entry = readdir(handle)){
		std::string name = entry->d_name;
		if(name == "." || name == "..") continue;
		struct stat st;
		if(lstat((dir + name).c_str(), &st) != 0) continue;
		if(S_ISDIR(st.st_mode)) listServerTextures(relative + name + "/", files, depth + 1);
		else if(S_ISREG(st.st_mode) && name.size() > 4 && rw::strcmp_ci(name.c_str() + name.size()-4, ".txd") == 0)
			files.push_back(relative + name);
	}
	closedir(handle);
#endif
}

void
ListServerTextureFiles(std::vector<std::string> &files)
{
	files.clear();
	if(!gModelsDir.empty()) listServerTextures("", files, 0);
	std::sort(files.begin(), files.end());
}

// fmt with its one %s or %d filled in.
static std::string
message(const char *fmt, const std::string &arg)
{
	char buf[1200];
	snprintf(buf, sizeof(buf), fmt, arg.c_str());
	return buf;
}

static std::string
message(const char *fmt, int arg)
{
	char buf[256];
	snprintf(buf, sizeof(buf), fmt, arg);
	return buf;
}

static void
modelName(const std::string &dff, char *dst, size_t size)
{
	std::string base = dff;
	size_t slash = base.find_last_of("/\\");
	if(slash != std::string::npos)
		base = base.substr(slash + 1);
	size_t dot = base.find_last_of('.');
	if(dot != std::string::npos)
		base.resize(dot);
	snprintf(dst, size, "%s", base.c_str());
}

// problems are logged, so English unless translated (for the dialog).
static void
addModel(int pawnId, int baseId, const std::string &dff, const std::string &txd,
	bool timed, int timeOn, int timeOff, bool character, std::vector<std::string> &problems,
	bool translated = false, bool inArtconfig = false)
{
	int editorId = EditorModelId(pawnId);
	if(character ? (pawnId < 20001 || pawnId > 30000) : (pawnId > -1000 || pawnId < -30000)){
		problems.push_back(message(translated ? _("ID %d is out of range") : "ID %d is out of range", pawnId));
		return;
	}
	if(GetObjectDef(editorId)){
		if(!IsCustomModel(editorId))
			problems.push_back(message(translated ? _("ID %d is already a game model") : "ID %d is already a game model", pawnId));
		return;	// listed twice: the first one counts, as on the server
	}
	std::string dffPath = findModelFile(dff);
	std::string txdPath = findModelFile(txd);
	if(dffPath.empty() || txdPath.empty()){
		problems.push_back(message(translated ? _("missing %s") : "missing %s", dffPath.empty() ? dff : txd));
		return;
	}
	ObjectDef *obj = AddObjectDef(editorId);
	if(obj == nil)
		return;
	modelName(dff, obj->m_name, sizeof(obj->m_name));
	obj->m_txdSlot = txdSlotForFile(txdPath);
	obj->m_numAtomics = 1;
	ObjectDef *base = GetObjectDef(baseId);
	if(character){
		obj->m_type = ObjectDef::CLUMP;
		obj->m_isPed = true;
		obj->m_drawDist[0] = 150.0f;
	}else{
		obj->m_type = ObjectDef::ATOMIC;
		obj->m_drawDist[0] = base && base->m_drawDist[0] > 0.0f ? base->m_drawDist[0] : 300.0f;
		obj->m_isTimed = timed;
		obj->m_timeOn = timeOn;
		obj->m_timeOff = timeOff;
	}
	SetObjectCategory(editorId, CUSTOM_MODELS_CATEGORY);
	CustomModel model = { editorId, dffPath, pawnId, baseId, txdPath, timed, character, timeOn, timeOff, inArtconfig };
	gModels.push_back(model);
	for(const std::string *path : { &dffPath, &txdPath }){
		struct stat st;
		if(stat(path->c_str(), &st) == 0)
			gFileStamps[*path] = { (long long)st.st_size, (long long)st.st_mtime };
	}
}

static void
readModelCalls(const std::string &text, std::vector<std::string> &problems, bool inArtconfig = false)
{
	std::smatch m;
	std::string::const_iterator from = text.begin();
	while(std::regex_search(from, text.end(), m, rAddSimpleModelTimed)){
		addModel(atoi(m[3].str().c_str()), atoi(m[2].str().c_str()), m[4], m[5], true,
			atoi(m[6].str().c_str()), atoi(m[7].str().c_str()), false, problems, false, inArtconfig);
		from = m.suffix().first;
	}
	from = text.begin();
	while(std::regex_search(from, text.end(), m, rAddSimpleModel)){
		addModel(atoi(m[3].str().c_str()), atoi(m[2].str().c_str()), m[4], m[5], false, 0, 0, false, problems, false,
			inArtconfig);
		from = m.suffix().first;
	}
	from = text.begin();
	while(std::regex_search(from, text.end(), m, rAddCharModel)){
		addModel(atoi(m[2].str().c_str()), atoi(m[1].str().c_str()), m[3], m[4], false, 0, 0, true, problems, false,
			inArtconfig);
		from = m.suffix().first;
	}
}

static std::string
readText(const std::string &path)
{
	std::string text;
	FILE *f = fopen(path.c_str(), "rb");
	if(f == nil)
		return text;
	char buf[8192];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), f)) > 0)
		text.append(buf, n);
	fclose(f);
	return text;
}

static void
listGamemodes(const std::string &dir, std::vector<std::string> &out)
{
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "/*.pwn").c_str(), &fd);
	if(h == INVALID_HANDLE_VALUE)
		return;
	do out.push_back(dir + "/" + fd.cFileName); while(FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR *d = opendir(dir.c_str());
	if(d == nil)
		return;
	struct dirent *e;
	while((e = readdir(d)) != nil){
		size_t len = strlen(e->d_name);
		if(len > 4 && rw::strcmp_ci(e->d_name + len - 4, ".pwn") == 0)
			out.push_back(dir + "/" + e->d_name);
	}
	closedir(d);
#endif
}

static void
unloadModels(void)
{
	for(size_t i = 0; i < gModels.size(); i++){
		RemoveObjectDef(gModels[i].editorId);
		SetObjectCategory(gModels[i].editorId, -1);
	}
	gModels.clear();
	// TXD slots stay registered; a TXD that comes back keeps its slot.
}

void
LoadCustomModelsForMap(const char *mapPath, const std::string &source)
{
	gLastMapPath = mapPath ? mapPath : "";
	std::string serverDir = customModelsServerDir(mapPath);
	std::string modelsDir = serverDir.empty() ? "" : ServerArtworkDirectory(serverDir);
	if(serverDir != gServerDir || modelsDir != gModelsDir){
		unloadModels();
		gServerDir = serverDir;
		gModelsDir = modelsDir;
	}
	if(gServerDir.empty())
		return;
	size_t before = gModels.size();
	std::vector<std::string> problems;
	// The server's artconfig.txt (SA-MP's way, still read), then Haumea's own
	// list of the workspace's models, then the scripts that register them.
	readModelCalls(readText(gModelsDir + "/artconfig.txt"), problems, true);
	readModelCalls(readText(gModelsDir + "/" + CUSTOM_MODELS_LIST), problems);
	std::vector<std::string> gamemodes;
	listGamemodes(gServerDir + "/gamemodes", gamemodes);
	for(size_t i = 0; i < gamemodes.size(); i++)
		readModelCalls(readText(gamemodes[i]), problems);
	readModelCalls(source, problems);
	// The maps beside it: a map generated without its registrations (a 0.3.7
	// copy, say) often sits next to the 0.3.DL one that has them.
	if(mapPath && mapPath[0]){
		std::string dir = mapPath;
		size_t slash = dir.find_last_of("/\\");
		dir = slash == std::string::npos ? "." : dir.substr(0, slash);
		std::vector<std::string> siblings, theirProblems;
		listGamemodes(dir, siblings);
		std::sort(siblings.begin(), siblings.end());
		for(const std::string &sibling : siblings)
			if(sibling != mapPath)
				readModelCalls(readText(sibling), theirProblems);
		// Theirs aren't this map's problems: logged, not shown.
		for(const std::string &problem : theirProblems)
			log("CustomModels: (a map beside it) %s\n", problem.c_str());
	}
	if(gModels.size() != before)
		log("CustomModels: %d custom model(s) from %s\n", (int)(gModels.size() - before), gModelsDir.c_str());
	for(size_t i = 0; i < problems.size(); i++)
		log("CustomModels: %s\n", problems[i].c_str());
	if(!problems.empty())
		Toast(TOAST_SPAWN, _("Custom models that couldn't be loaded: %d (see the log)"), (int)problems.size());
}

// Reads the custom models' DFF and TXD files again (all of them, or only
// the ones that changed on disk) and redraws what uses them; with lists,
// also reads the model lists again for models added since. Returns how
// many models were read again.
static int
reloadCustomModelFiles(bool onlyChanged, bool lists)
{
	std::vector<int> ids, slots;
	auto changed = [&](const std::string &path){
		struct stat st;
		if(stat(path.c_str(), &st) != 0)
			return false;	// gone: keep what was loaded
		FileStamp now = { (long long)st.st_size, (long long)st.st_mtime };
		auto it = gFileStamps.find(path);
		bool differs = it == gFileStamps.end() || it->second.size != now.size || it->second.time != now.time;
		gFileStamps[path] = now;
		return !onlyChanged || differs;
	};
	std::map<std::string, bool> seen;	// a TXD shared by many models is checked once
	for(const CustomModel &m : gModels){
		if(seen.find(m.dffPath) == seen.end())
			seen[m.dffPath] = changed(m.dffPath);
		if(seen.find(m.txdPath) == seen.end())
			seen[m.txdPath] = changed(m.txdPath);
		if(seen[m.dffPath] || seen[m.txdPath])
			ids.push_back(m.editorId);
		if(seen[m.txdPath]){
			ObjectDef *obj = GetObjectDef(m.editorId);
			if(obj && obj->m_txdSlot >= 0 && std::find(slots.begin(), slots.end(), obj->m_txdSlot) == slots.end())
				slots.push_back(obj->m_txdSlot);
		}
	}
	if(!ids.empty()){
		int rebuilt = ReloadModelsInPlace(ids, slots);
		log("CustomModels: read %d model(s) again, %d object(s) redrawn\n", (int)ids.size(), rebuilt);
	}
	if(lists && !gLastMapPath.empty())
		LoadCustomModelsForMap(gLastMapPath.c_str(), readText(gLastMapPath));
	else if(lists)
		LoadCustomModelsForMap(nil, std::string());
	return (int)ids.size();
}

int
ReloadCustomModels(void)
{
	return reloadCustomModelFiles(false, true);
}

// Each frame: a model file rebuilt under the same name and ID shows without
// a restart. In a session the host's new file reaches the others' copies,
// which this notices the same way.
void
CustomModelsTick(void)
{
	static time_t last;
	time_t now = time(nil);
	if(gModels.empty() || now - last < 2)
		return;
	last = now;
	if(reloadCustomModelFiles(true, false) > 0)
		Toast(TOAST_SPAWN, _("Custom models changed on disk were reloaded"));
}

// ---- A multiplayer session's custom models ----

static std::string
baseName(const std::string &path)
{
	size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string
CustomModelsSessionList(std::map<std::string, std::string> &files)
{
	std::vector<std::pair<int, std::string>> lines;
	for(const CustomModel &m : gModels){
		std::string dff = baseName(m.dffPath), txd = baseName(m.txdPath);
		files.emplace(dff, m.dffPath);
		files.emplace(txd, m.txdPath);
		char line[512];
		if(m.character)
			snprintf(line, sizeof(line), "AddCharModel(%d, %d, \"%s\", \"%s\");\n", m.baseId, m.pawnId,
				dff.c_str(), txd.c_str());
		else if(m.timed)
			snprintf(line, sizeof(line), "AddSimpleModelTimed(-1, %d, %d, \"%s\", \"%s\", %d, %d);\n", m.baseId,
				m.pawnId, dff.c_str(), txd.c_str(), m.timeOn, m.timeOff);
		else
			snprintf(line, sizeof(line), "AddSimpleModel(-1, %d, %d, \"%s\", \"%s\");\n", m.baseId, m.pawnId,
				dff.c_str(), txd.c_str());
		// The host's artconfig.txt models first: a guest's map leaves those
		// out of its own registrations too, so both save the same code.
		lines.emplace_back(m.inArtconfig ? m.pawnId - 100000 : m.pawnId, line);
	}
	std::sort(lines.begin(), lines.end());
	std::string out;
	bool marked = false;
	for(const auto &line : lines){
		if(line.first >= -30000 && !marked){
			out += SESSION_OWN_MODELS;
			marked = true;
		}
		out += line.second;
	}
	if(!marked)
		out += SESSION_OWN_MODELS;
	return out;
}

int
AddSessionCustomModels(const std::string &dir, const std::string &artconfig)
{
	size_t before = gModels.size();
	std::vector<std::string> problems;
	gLookupDir = dir;
	// Before the marker: the host's artconfig.txt; after it, models maps register.
	size_t mark = artconfig.find(SESSION_OWN_MODELS);
	readModelCalls(artconfig.substr(0, mark), problems, mark != std::string::npos);
	if(mark != std::string::npos)
		readModelCalls(artconfig.substr(mark), problems);
	gLookupDir.clear();
	for(size_t i = 0; i < problems.size(); i++)
		log("CustomModels: session: %s\n", problems[i].c_str());
	return (int)(gModels.size() - before);
}

// The AddSimpleModel/AddCharModel calls for these custom models (editor
// IDs), one a line with indent before each, leaving out the ones the
// server's artconfig.txt registers.
std::string
CustomModelRegistrations(const std::vector<int> &editorIds, const char *indent)
{
	std::vector<std::pair<int, std::string>> lines;
	for(const CustomModel &m : gModels){
		if(m.inArtconfig || std::find(editorIds.begin(), editorIds.end(), m.editorId) == editorIds.end())
			continue;
		// As the server finds them: relative to its models folder.
		auto relative = [](const std::string &path){
			std::string dir = gModelsDir + "/";
			return path.compare(0, dir.size(), dir) == 0 ? path.substr(dir.size()) : baseName(path);
		};
		std::string dff = relative(m.dffPath), txd = relative(m.txdPath);
		char line[512];
		if(m.character)
			snprintf(line, sizeof(line), "%sAddCharModel(%d, %d, \"%s\", \"%s\");\n", indent, m.baseId, m.pawnId,
				dff.c_str(), txd.c_str());
		else if(m.timed)
			snprintf(line, sizeof(line), "%sAddSimpleModelTimed(-1, %d, %d, \"%s\", \"%s\", %d, %d);\n", indent,
				m.baseId, m.pawnId, dff.c_str(), txd.c_str(), m.timeOn, m.timeOff);
		else
			snprintf(line, sizeof(line), "%sAddSimpleModel(-1, %d, %d, \"%s\", \"%s\");\n", indent, m.baseId,
				m.pawnId, dff.c_str(), txd.c_str());
		lines.emplace_back(m.pawnId, line);
	}
	std::sort(lines.begin(), lines.end(), [](const std::pair<int, std::string> &a, const std::pair<int, std::string> &b){
		return a.first > b.first;
	});
	std::string out;
	for(const auto &line : lines)
		out += line.second;
	return out;
}

bool
MapUsesCustomModels(void)
{
	return !gModels.empty();
}

// Collision of a custom model: the COL stored in its DFF, as 0.3.DL wants
// it, or one made from the model's own triangles.
bool
CustomModelHasNoGameCollision(int editorId)
{
	return gGeneratedCollision.count(editorId) != 0;
}

void
SetupCustomModelCollision(ObjectDef *obj, rw::Clump *clump)
{
	if(obj == nil || obj->m_colModel || clump == nil)
		return;
	gGeneratedCollision.erase(obj->m_id);	// read again: the file may have changed
	// The layout of librwgta's collision plugin data (gtaplg.cpp).
	struct SizedData { uint32 size; uint8 *data; };
	SizedData *data = PLUGINOFFSET(SizedData, clump, gta::collisionOffset);
	std::vector<char> generated;
	uint8 *bytes = data->size >= sizeof(ColFileHeader) ? data->data : nil;
	int size = bytes ? data->size : 0;
	if(bytes == nil){
		rw::Atomic *atomic = nil;
		FORLIST(lnk, clump->atomics){
			atomic = rw::Atomic::fromClump(lnk);
			break;
		}
		char err[128];
		gGeneratedCollision.insert(obj->m_id);
		if(atomic == nil || !GenerateCol3FromAtomic(atomic, obj->m_name, generated, nil, err, sizeof(err)))
			return;
		bytes = (uint8*)generated.data();
		size = (int)generated.size();
	}
	ColFileHeader *header = (ColFileHeader*)bytes;
	int bodySize = min((int)header->modelsize - 24, size - (int)sizeof(ColFileHeader));
	if(bodySize <= 0)
		return;
	CColModel *col = new CColModel;
	col->file = nil;
	uint8 *body = bytes + sizeof(ColFileHeader);
	switch(header->fourcc){
	case 0x4C4C4F43: ReadColModel(col, body, bodySize); break;
	case 0x324C4F43: ReadColModelVer2(col, body, bodySize); break;
	case 0x334C4F43: ReadColModelVer3(col, body, bodySize); break;
	case 0x344C4F43: ReadColModelVer4(col, body, bodySize); break;
	default:
		delete col;
		return;
	}
	obj->m_colModel = col;
}

// ---- Adding models to the server ----


static std::string
serverDirForWriting(void)
{
	return gServerDir.empty() ? std::string(GetWorkspaceDir()) : gServerDir;
}

static bool
readFile(const std::string &path, std::vector<uint8> &data)
{
	data.clear();
	FILE *f = fopen(path.c_str(), "rb");
	if(f == nil)
		return false;
	uint8 buf[65536];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), f)) > 0)
		data.insert(data.end(), buf, buf + n);
	fclose(f);
	return true;
}

// Copies src into the models folder under its own name, or name_2 and so on
// when a different file already has it. Returns the name used.
static bool
copyIntoModels(const std::string &modelsDir, const std::string &src, std::string &usedName, std::string &err)
{
	std::vector<uint8> data, existing;
	if(!readFile(src, data) || data.empty()){
		err = message(_("can't read %s"), src);
		return false;
	}
	std::string name = src.substr(src.find_last_of("/\\") + 1);
	size_t dot = name.find_last_of('.');
	std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
	std::string ext = dot == std::string::npos ? "" : name.substr(dot);
	for(int n = 1; n < 100; n++){
		std::string candidate = n == 1 ? stem + ext : stem + "_" + std::to_string(n) + ext;
		std::string dst = modelsDir + "/" + candidate;
		if(readFile(dst, existing)){
			if(existing == data){
				usedName = candidate;	// already there
				return true;
			}
			continue;
		}
		if(!EnsureParentDirectoriesForPath(dst.c_str())){
			err = message(_("can't create %s"), modelsDir);
			return false;
		}
		FILE *f = fopen(dst.c_str(), "wb");
		if(f == nil || fwrite(&data[0], 1, data.size(), f) != data.size()){
			if(f) fclose(f);
			err = message(_("can't write %s"), dst);
			return false;
		}
		fclose(f);
		usedName = candidate;
		return true;
	}
	err = message(_("too many files named %s"), name);
	return false;
}

static int
nextFreeId(bool character)
{
	if(character){
		for(int id = 20001; id <= 30000; id++)
			if(GetObjectDef(id) == nil)
				return id;
	}else{
		for(int id = -1000; id >= -30000; id--)
			if(GetObjectDef(EditorModelId(id)) == nil)
				return id;
	}
	return 0;
}

// Copies the DFF and TXD into <server>/models, adds its line to Haumea's list
// and loads the model. Returns its editor ID, or -1.
static int
addModelToServer(const std::string &dff, const std::string &txd, bool character, int id, int baseId,
	std::string &err)
{
	std::string server = serverDirForWriting();
	if(server.empty()){
		err = _("pick the server folder first");
		return -1;
	}
	std::string modelsDir = ServerArtworkDirectory(server);
	if(IsInGameDir(modelsDir.c_str())){
		err = _("that folder is inside the game folder; pick the server's folder");
		return -1;
	}
	std::vector<uint8> head;
	if(!readFile(dff, head) || head.size() < 12 || head[0] != 0x10){
		err = _("the DFF isn't a model file");
		return -1;
	}
	if(GetObjectDef(EditorModelId(id))){
		err = message(_("ID %d is already used"), id);
		return -1;
	}
	std::string dffName, txdName;
	if(!copyIntoModels(modelsDir, dff, dffName, err) || !copyIntoModels(modelsDir, txd, txdName, err))
		return -1;
	char line[512];
	if(character)
		snprintf(line, sizeof(line), "AddCharModel(%d, %d, \"%s\", \"%s\");", baseId, id, dffName.c_str(), txdName.c_str());
	else
		snprintf(line, sizeof(line), "AddSimpleModel(-1, %d, %d, \"%s\", \"%s\");", baseId, id, dffName.c_str(), txdName.c_str());
	std::string list = modelsDir + "/" + CUSTOM_MODELS_LIST;
	std::string text = readText(list);
	FILE *f = fopen(list.c_str(), "ab");
	if(f == nil){
		err = message(_("can't write %s"), list);
		return -1;
	}
	if(text.empty())
		fputs("// Haumea Studio's custom models for this server. Servers don't read this file: each map\n"
			"// registers the models it uses with these calls in its own code.\n", f);
	else if(text.back() != '\n')
		fputc('\n', f);
	fprintf(f, "%s\n", line);
	fclose(f);
	log("CustomModels: added %s to %s\n", line, list.c_str());

	if(gServerDir.empty())
		gServerDir = server;
	std::vector<std::string> problems;
	addModel(id, baseId, dffName, txdName, false, 0, 0, character, problems, true);
	if(!problems.empty()){
		err = problems[0];
		return -1;
	}
	return EditorModelId(id);
}

// ---- Old modloader/Haumea imports ----

struct LegacyImport { int id; std::string model, txd; };

static std::string
legacyDir(void)
{
	char root[1024];
	if(!GetGameRootDirectory(root, sizeof(root)))
		return "";
	return std::string(root) + "/modloader/Haumea";
}

static std::vector<LegacyImport>
readLegacyImports(void)
{
	std::vector<LegacyImport> out;
	std::string text = readText(legacyDir() + "/data/maps/haumea/custom.ide");
	size_t pos = 0;
	while(pos < text.size()){
		size_t end = text.find('\n', pos);
		if(end == std::string::npos) end = text.size();
		std::string line = text.substr(pos, end - pos);
		pos = end + 1;
		int id;
		char model[64], txd[64];
		if(sscanf(line.c_str(), " %d , %63[^, ] , %63[^, ]", &id, model, txd) == 3){
			LegacyImport li = { id, model, txd };
			out.push_back(li);
		}
	}
	return out;
}

int
FindLegacyImports(const std::vector<int> &unknownIds, std::vector<int> &legacyIds)
{
	legacyIds.clear();
	std::vector<LegacyImport> imports = readLegacyImports();
	for(size_t i = 0; i < unknownIds.size(); i++)
		for(size_t j = 0; j < imports.size(); j++)
			if(imports[j].id == unknownIds[i])
				legacyIds.push_back(unknownIds[i]);
	return (int)legacyIds.size();
}

int
RemapLegacyModel(int id)
{
	std::map<int, int>::iterator it = gLegacyRemap.find(id);
	return it == gLegacyRemap.end() ? -1 : it->second;
}

bool
MigrateLegacyImports(const std::vector<int> &ids, std::string &err)
{
	std::vector<LegacyImport> imports = readLegacyImports();
	std::string dir = legacyDir() + "/models/gta3.img/";
	for(size_t i = 0; i < ids.size(); i++)
		for(size_t j = 0; j < imports.size(); j++){
			if(imports[j].id != ids[i] || gLegacyRemap.count(ids[i]))
				continue;
			// The same files moved before keep the ID they got then.
			int editorId = -1;
			for(size_t m = 0; m < gModels.size() && editorId < 0; m++){
				std::string name = gModels[m].dffPath.substr(gModels[m].dffPath.find_last_of("/\\") + 1);
				if(rw::strcmp_ci(name.c_str(), (imports[j].model + ".dff").c_str()) == 0)
					editorId = gModels[m].editorId;
			}
			if(editorId < 0)
				editorId = addModelToServer(dir + imports[j].model + ".dff", dir + imports[j].txd + ".txd",
					false, nextFreeId(false), 19379, err);
			if(editorId < 0)
				return false;
			gLegacyRemap[ids[i]] = editorId;
		}
	return true;
}

const char*
GetCustomModelsMapPath(void)
{
	return gLastMapPath.empty() ? nil : gLastMapPath.c_str();
}

// ---- Add Custom Model dialog ----

static struct {
	bool open;
	char dff[1024], txd[1024];
	bool character;
	int id, baseId;
	char error[512];
} gDialog;

static bool
pickFile(char *dst, size_t size, const char *ext)
{
#ifdef _WIN32
	char filename[MAX_PATH] = "";
	char filter[64];
	snprintf(filter, sizeof(filter), "%s files%c*.%s%c", ext, 0, ext, 0);
	OPENFILENAMEA ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFilter = filter;
	ofn.lpstrFile = filename;
	ofn.nMaxFile = sizeof(filename);
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
	if(!GetOpenFileNameA(&ofn))
		return false;
	snprintf(dst, size, "%s", filename);
	return true;
#else
	if(!FileDialogAvailable())
		return false;
	std::string script = std::string("if command -v zenity >/dev/null 2>&1; then zenity --file-selection") +
		(ext ? std::string(" --file-filter='*.") + ext + "'" : std::string(" --directory")) +
		"; elif command -v kdialog >/dev/null 2>&1; then kdialog " +
		(ext ? std::string("--getopenfilename . '*.") + ext + "'" : std::string("--getexistingdirectory .")) + "; fi";
	std::string quoted = "'";
	for(char c : script)
		quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
	quoted += "'";
	FILE *pipe = popen(("sh -lc " + quoted).c_str(), "r");
	if(pipe == nil)
		return false;
	char line[1024];
	bool ok = fgets(line, sizeof(line), pipe) != nil;
	pclose(pipe);
	if(!ok)
		return false;
	size_t n = strlen(line);
	while(n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
		line[--n] = '\0';
	if(n == 0)
		return false;
	snprintf(dst, size, "%s", line);
	return true;
#endif
}

void
OpenAddCustomModelDialog(const char *droppedPath)
{
	memset(&gDialog, 0, sizeof(gDialog));
	gDialog.open = true;
	gDialog.id = nextFreeId(false);
	gDialog.baseId = 19379;
	if(droppedPath){
		// A dropped DFF or TXD: the other one usually sits next to it.
		std::string p = droppedPath;
		size_t dot = p.find_last_of('.');
		std::string stem = dot == std::string::npos ? p : p.substr(0, dot);
		snprintf(gDialog.dff, sizeof(gDialog.dff), "%s.dff", stem.c_str());
		snprintf(gDialog.txd, sizeof(gDialog.txd), "%s.txd", stem.c_str());
	}
}

// Adds a DFF/TXD pair to the server as a custom model: where it goes (the
// server), the two files, what it is (object or skin) and its IDs, then Add.
// The Add custom model dialog's add, for Namaka: id 0 picks a free one;
// baseId 0 takes the dialog's default (19379, or skin 7). Returns the Pawn id.
int
AddCustomModelToServer(const char *dff, const char *txd, bool character, int id, int baseId, std::string &err)
{
	if(id == 0)
		id = nextFreeId(character);
	if(baseId == 0)
		baseId = character ? 7 : 19379;
	bool idOk = character ? id >= 20001 && id <= 30000 : id <= -1000 && id >= -30000;
	if(!idOk){
		err = character ? "id: skins take 20001 to 30000" : "id: objects take -1000 to -30000";
		return 0;
	}
	int editorId = addModelToServer(dff, txd, character, id, baseId, err);
	if(editorId < 0)
		return 0;
	if(!character)
		SetSpawnObjectId(editorId);
	return id;
}

void
DrawAddCustomModelDialog(void)
{
	if(!gDialog.open)
		return;
	// ###addcustommodel, not L(): the ID the window has always had, so saved layouts stay.
	char title[160];
	snprintf(title, sizeof(title), "%s###addcustommodel", _("Add Custom Model"));
	ImGuiWindowFlags placed = UiPlaceToolWindow(title, 30.0f, true);
	if(!ImGui::Begin(title, &gDialog.open, ImGuiWindowFlags_NoCollapse | placed)){
		ImGui::End();
		return;
	}
	UiFitItemsToPanel();
	const char *about = _("Your own DFF and TXD, copied into the server's models folder. Maps that use them "
		"register them in their own code (AddSimpleModel, AddCharModel); the server (open.mp, or SA-MP 0.3.DL) "
		"sends them to players as they join, and players need the SA-MP 0.3.DL client. Nothing is written "
		"into the game folder.");
	std::string server = serverDirForWriting();
	if(server.empty()){
		// Asked here when nothing says which server: it becomes the workspace.
		ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(UI_COLOR_WARNING));
		ImGui::TextWrapped("%s", _("Which server are these models for?"));
		ImGui::PopStyleColor();
		ImGui::SetItemTooltip("%s", about);
		if(UiFooterButton(L("Choose the server folder..."))){
			char picked[1024] = "";
			if(PickFolderDialog(picked, sizeof(picked), _("Server folder (has gamemodes/, models/)"), nil)){
				SetWorkspaceDir(picked);
				LoadCustomModelsForMap(nil, std::string());
			}
		}
		ImGui::End();
		return;
	}
	// 1. Where it goes, and whether that server sends custom models at all.
	UiInfoRow(_("Server"), server.c_str(), about);
	if(ServerArtworkEnabled(server) == 0){
		ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(UI_COLOR_WARNING));
		ImGui::TextWrapped("%s", _("Artwork is off on this server: players won't get custom models."));
		ImGui::PopStyleColor();
		if(ImGui::Button(_("Turn on artwork"))){
			std::string err;
			if(ServerEnableArtwork(server, &err))
				Toast(TOAST_SAVE, _("Artwork is on in %s."), server.c_str());
			else
				snprintf(gDialog.error, sizeof(gDialog.error), "%s", err.c_str());
		}
		ImGui::SetItemTooltip("%s", _("Sets artwork.enable in config.json (open.mp), or useartwork 1 in server.cfg (SA-MP)."));
	}

	if(!gModels.empty()){
		if(ImGui::Button(L("Reload custom models")))
			Toast(TOAST_SPAWN, _("Custom models reloaded: %d"), ReloadCustomModels());
		ImGui::SetItemTooltip("%s", _("Reads the models' DFF and TXD files and the AddSimpleModel lines again. "
			"Files rebuilt on disk are also picked up by themselves."));
	}

	// 2. The model's two files: a path filling the row, Browse at its end.
	UiHeading(_("Model files"));
	float spacing = ImGui::GetStyle().ItemSpacing.x;
	float browseWidth = ImGui::CalcTextSize(_("Browse...")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	const char *labels[2] = { "DFF", "TXD" };
	char *fields[2] = { gDialog.dff, gDialog.txd };
	for(int i = 0; i < 2; i++){
		ImGui::PushID(i);
		char unused[64];
		UiFieldLabel(labels[i], unused, sizeof(unused));
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseWidth - spacing);
		ImGui::InputText(i == 0 ? "##dff" : "##txd", fields[i], sizeof(gDialog.dff));
		ImGui::SameLine();
		if(ImGui::Button(L("Browse...")))
			pickFile(fields[i], sizeof(gDialog.dff), i == 0 ? "dff" : "txd");
		ImGui::PopID();
	}

	// 3. What it is, and the IDs scripts use for it.
	UiHeading(_("Model"));
	int kind = gDialog.character ? 1 : 0;
	const char *kinds[2] = { L("Object (AddSimpleModel)"), L("Skin (AddCharModel)") };
	if(UiSegmented("##cm_kind", kinds, 2, &kind)){
		gDialog.character = kind == 1;
		gDialog.id = nextFreeId(gDialog.character);
		gDialog.baseId = gDialog.character ? 7 : 19379;
	}
	ImGui::Spacing();
	UiInputInt("ID", &gDialog.id);
	ImGui::SetItemTooltip("%s", gDialog.character ? _("20001 to 30000: the skin ID scripts use.") :
		_("-1000 to -30000: the model ID scripts use, as in CreateObject(-1000, ...)."));
	UiInputInt(gDialog.character ? L("Base skin") : L("Base model"), &gDialog.baseId);
	ImGui::SetItemTooltip("%s", gDialog.character ? _("The game skin whose settings the new skin takes.") :
		_("The game object whose settings the new model takes (19379 is a common choice)."));

	if(gDialog.error[0]){
		ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(UI_COLOR_ERROR));
		ImGui::TextWrapped("%s", gDialog.error);
		ImGui::PopStyleColor();
	}
	if(UiFooterButton(L("Add to server"), gDialog.dff[0] && gDialog.txd[0])){
		gDialog.error[0] = '\0';
		bool idOk = gDialog.character ? gDialog.id >= 20001 && gDialog.id <= 30000 :
			gDialog.id <= -1000 && gDialog.id >= -30000;
		if(!idOk)
			snprintf(gDialog.error, sizeof(gDialog.error), "%s", gDialog.character ?
				_("Skin IDs go from 20001 to 30000.") : _("Object IDs go from -1000 to -30000."));
		else{
			std::string err;
			int editorId = addModelToServer(gDialog.dff, gDialog.txd, gDialog.character,
				gDialog.id, gDialog.baseId, err);
			if(editorId < 0)
				snprintf(gDialog.error, sizeof(gDialog.error), "%s", err.c_str());
			else{
				if(!gDialog.character)
					SetSpawnObjectId(editorId);
				Toast(TOAST_SPAWN, _("Added %d to the server: Asset Browser > Custom models"), gDialog.id);
				gDialog.open = false;
			}
		}
	}
	ImGui::End();
}
