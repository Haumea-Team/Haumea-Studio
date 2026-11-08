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
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#else
#include <dirent.h>
#endif

// SA-MP 0.3.DL and open.mp custom models: the server's models/artconfig.txt
// and AddSimpleModel/AddCharModel calls in its gamemode. The DFF and TXD
// files are read from the server's models folder where they are; nothing is
// copied into the game.

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
};

static std::string gServerDir;	// the server folder holding models/
static std::string gModelsDir;	// artwork.models_path / artpath, or models/
static std::vector<CustomModel> gModels;
static std::map<int, std::string> gTxdPaths;	// txd slot -> file
static std::map<std::string, int> gTxdSlots;	// file -> txd slot
static std::string gLastMapPath;
static std::map<int, int> gLegacyRemap;	// old modloader/Haumea ID -> editor ID
static std::string gLookupDir;	// where model files are looked up instead of gModelsDir

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
		if(isFile(dir + "/models/artconfig.txt") || isDir(dir + "/gamemodes"))
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
	bool translated = false)
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
	CustomModel model = { editorId, dffPath, pawnId, baseId, txdPath, timed, character, timeOn, timeOff };
	gModels.push_back(model);
}

static void
readModelCalls(const std::string &text, std::vector<std::string> &problems)
{
	std::smatch m;
	std::string::const_iterator from = text.begin();
	while(std::regex_search(from, text.end(), m, rAddSimpleModelTimed)){
		addModel(atoi(m[3].str().c_str()), atoi(m[2].str().c_str()), m[4], m[5], true,
			atoi(m[6].str().c_str()), atoi(m[7].str().c_str()), false, problems);
		from = m.suffix().first;
	}
	from = text.begin();
	while(std::regex_search(from, text.end(), m, rAddSimpleModel)){
		addModel(atoi(m[3].str().c_str()), atoi(m[2].str().c_str()), m[4], m[5], false, 0, 0, false, problems);
		from = m.suffix().first;
	}
	from = text.begin();
	while(std::regex_search(from, text.end(), m, rAddCharModel)){
		addModel(atoi(m[2].str().c_str()), atoi(m[1].str().c_str()), m[3], m[4], false, 0, 0, true, problems);
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
	readModelCalls(readText(gModelsDir + "/artconfig.txt"), problems);
	std::vector<std::string> gamemodes;
	listGamemodes(gServerDir + "/gamemodes", gamemodes);
	for(size_t i = 0; i < gamemodes.size(); i++)
		readModelCalls(readText(gamemodes[i]), problems);
	readModelCalls(source, problems);
	if(gModels.size() != before)
		log("CustomModels: %d custom model(s) from %s\n", (int)(gModels.size() - before), gModelsDir.c_str());
	for(size_t i = 0; i < problems.size(); i++)
		log("CustomModels: %s\n", problems[i].c_str());
	if(!problems.empty())
		Toast(TOAST_SPAWN, _("Custom models that couldn't be loaded: %d (see the log)"), (int)problems.size());
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
		lines.emplace_back(m.pawnId, line);
	}
	std::sort(lines.begin(), lines.end());
	std::string out;
	for(const auto &line : lines)
		out += line.second;
	return out;
}

int
AddSessionCustomModels(const std::string &dir, const std::string &artconfig)
{
	size_t before = gModels.size();
	std::vector<std::string> problems;
	gLookupDir = dir;
	readModelCalls(artconfig, problems);
	gLookupDir.clear();
	for(size_t i = 0; i < problems.size(); i++)
		log("CustomModels: session: %s\n", problems[i].c_str());
	return (int)(gModels.size() - before);
}

bool
MapUsesCustomModels(void)
{
	return !gModels.empty();
}

// Collision of a custom model: the COL stored in its DFF, as 0.3.DL wants
// it, or one made from the model's own triangles.
void
SetupCustomModelCollision(ObjectDef *obj, rw::Clump *clump)
{
	if(obj == nil || obj->m_colModel || clump == nil)
		return;
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

// Copies the DFF and TXD into <server>/models, adds the artconfig.txt line
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
	std::string artconfig = modelsDir + "/artconfig.txt";
	std::string text = readText(artconfig);
	FILE *f = fopen(artconfig.c_str(), "ab");
	if(f == nil){
		err = message(_("can't write %s"), artconfig);
		return -1;
	}
	if(!text.empty() && text.back() != '\n')
		fputc('\n', f);
	fprintf(f, "%s\n", line);
	fclose(f);
	log("CustomModels: added %s to %s\n", line, artconfig.c_str());

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

void
DrawAddCustomModelDialog(void)
{
	if(!gDialog.open)
		return;
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0.0f), ImGuiCond_Appearing);
	// ###addcustommodel, not L(): the ID the window has always had, so saved layouts stay.
	char title[160];
	snprintf(title, sizeof(title), "%s###addcustommodel", _("Add Custom Model (SA-MP 0.3.DL / open.mp)"));
	if(!ImGui::Begin(title, &gDialog.open,
	   ImGuiWindowFlags_NoCollapse)){
		ImGui::End();
		return;
	}
	ImGui::TextWrapped("%s", _("Copies the DFF and TXD into the server's models folder and adds them to "
		"models/artconfig.txt, so SA-MP 0.3.DL and open.mp send them to players. "
		"Nothing is written into the game folder."));
	ImGui::Spacing();

	float browseWidth = ImGui::CalcTextSize(_("Browse...")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	std::string server = serverDirForWriting();
	if(server.empty()){
		// Asked here when nothing says which server: it becomes the workspace.
		ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f), "%s", _("Which server are these models for?"));
		if(ImGui::Button(L("Choose the server folder..."))){
			char picked[1024] = "";
			if(PickFolderDialog(picked, sizeof(picked), _("Server folder (has gamemodes/, models/)"), nil)){
				SetWorkspaceDir(picked);
				LoadCustomModelsForMap(nil, std::string());
			}
		}
		ImGui::End();
		return;
	}
	ImGui::TextDisabled(_("Server: %s"), server.c_str());
	const char *labels[2] = { "DFF", "TXD" };
	char *fields[2] = { gDialog.dff, gDialog.txd };
	for(int i = 0; i < 2; i++){
		ImGui::PushID(i);
		ImGui::SetNextItemWidth(-browseWidth - ImGui::GetStyle().ItemSpacing.x - ImGui::CalcTextSize("TXD").x - 8.0f);
		ImGui::InputText(i == 0 ? "##dff" : "##txd", fields[i], sizeof(gDialog.dff));
		ImGui::SameLine();
		if(ImGui::Button(L("Browse...")))
			pickFile(fields[i], sizeof(gDialog.dff), i == 0 ? "dff" : "txd");
		ImGui::SameLine();
		ImGui::TextUnformatted(labels[i]);
		ImGui::PopID();
	}

	if(ImGui::RadioButton(L("Object (AddSimpleModel)"), !gDialog.character)){
		gDialog.character = false;
		gDialog.id = nextFreeId(false);
		gDialog.baseId = 19379;
	}
	ImGui::SameLine();
	if(ImGui::RadioButton(L("Skin (AddCharModel)"), gDialog.character)){
		gDialog.character = true;
		gDialog.id = nextFreeId(true);
		gDialog.baseId = 7;
	}
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
	ImGui::InputInt("ID", &gDialog.id);
	ImGui::SetItemTooltip("%s", gDialog.character ? _("20001 to 30000: the skin ID scripts use.") :
		_("-1000 to -30000: the model ID scripts use, as in CreateObject(-1000, ...)."));
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
	ImGui::InputInt(gDialog.character ? L("Base skin") : L("Base model"), &gDialog.baseId);
	ImGui::SetItemTooltip("%s", gDialog.character ? _("The game skin whose settings the new skin takes.") :
		_("The game object whose settings the new model takes (19379 is a common choice)."));

	if(gDialog.error[0])
		ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", gDialog.error);
	ImGui::Spacing();
	if(ImGui::Button(L("Add to server"))){
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
				Toast(TOAST_SPAWN, _("Added %d to the server: Object Browser > Custom models"), gDialog.id);
				gDialog.open = false;
			}
		}
	}
	ImGui::End();
}
