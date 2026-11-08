#include "haumea.h"
#include "modelmeta.h"
#include "gxt.h"
#include "categories.h"
#include "modloader.h"
#include "samp_loader.h"
#include "workspace.h"

#include <algorithm>
#include <chrono>
#include <ctype.h>
#include <map>
#include <set>
#include <string.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace {

std::vector<ModelMeta> gMeta;	// by model id
std::unordered_set<std::string> gDynamic;	// object.dat's names
std::unordered_set<std::string> gExplosive;	// the ones whose "causes explosion" column is set
std::vector<bool> gLodTarget;	// models that are another's far version

struct MetaZone { std::string name; int city; };
std::vector<MetaZone> gZones;

// ---- Details: polygons and materials from the DFF, texture names from the TXD ----

bool gDetailsWanted;
int gDetailsNext;	// the next model id to read
std::vector<std::vector<std::string>> gTxdTextures;	// by TXD slot, lower case
std::vector<bool> gTxdRead;

uint32
read32(const uint8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32)p[3] << 24;
}

enum { RW_STRUCT = 0x01, RW_MATLIST = 0x08, RW_GEOMETRY = 0x0F, RW_CLUMP = 0x10, RW_TEXNATIVE = 0x15,
	RW_TEXDICT = 0x16, RW_GEOMLIST = 0x1A };

// Walks a RenderWare chunk stream: adds each geometry's triangles and each
// material list's count.
void
walkDff(const uint8 *p, const uint8 *end, int *polygons, int *materials)
{
	while(p + 12 <= end){
		uint32 type = read32(p), size = read32(p + 4);
		const uint8 *body = p + 12;
		if(size > (uint32)(end - body))
			return;
		const uint8 *bodyEnd = body + size;
		if(type == RW_CLUMP || type == RW_GEOMLIST)
			walkDff(body, bodyEnd, polygons, materials);
		else if(type == RW_GEOMETRY){
			// Its struct: format flags, triangles, vertices, morph targets...
			if(body + 20 <= bodyEnd && read32(body) == RW_STRUCT)
				*polygons += (int)read32(body + 16);
			walkDff(body, bodyEnd, polygons, materials);
		}else if(type == RW_MATLIST){
			if(body + 16 <= bodyEnd && read32(body) == RW_STRUCT)
				*materials += (int)read32(body + 12);
		}
		p = bodyEnd;
	}
}

// A PC TXD's texture names: each native texture's struct starts with the
// platform, the filter flags and the 32-byte name.
void
readTxdNames(const uint8 *p, const uint8 *end, std::vector<std::string> &names)
{
	while(p + 12 <= end){
		uint32 type = read32(p), size = read32(p + 4);
		const uint8 *body = p + 12;
		if(size > (uint32)(end - body))
			return;
		if(type == RW_TEXDICT)
			readTxdNames(body, body + size, names);
		else if(type == RW_TEXNATIVE && body + 12 + 8 + 32 <= body + size && read32(body) == RW_STRUCT){
			const char *name = (const char*)body + 12 + 8;
			std::string n(name, strnlen(name, 32));
			for(char &c : n) c = (char)tolower((unsigned char)c);
			names.push_back(n);
		}
		p = body + size;
	}
}

void
readTxd(int slot)
{
	if(slot < 0)
		return;
	if((int)gTxdRead.size() <= slot){
		gTxdRead.resize(slot + 1);
		gTxdTextures.resize(slot + 1);
	}
	if(gTxdRead[slot])
		return;
	gTxdRead[slot] = true;
	TxdDef *txd = GetTxdDef(slot);
	if(txd == nil || txd->imageIndex < 0)
		return;
	int size = 0;
	uint8 *data = ReadFileFromImage(txd->imageIndex, &size);	// cdimage's buffer
	if(data && size > 0)
		readTxdNames(data, data + size, gTxdTextures[slot]);
}

void
readDetails(int id)
{
	ModelMeta &m = gMeta[id];
	ObjectDef *obj = GetObjectDef(id);
	if(obj == nil)
		return;
	m.polygons = m.materials = 0;
	int size = 0;
	if(const char *loose = GetCustomModelDffPath(id)){
		uint8 *data = ReadLooseFile(loose, &size);
		if(data){
			walkDff(data, data + size, &m.polygons, &m.materials);
			free(data);
		}
	}else if(obj->m_imageIndex >= 0){
		uint8 *data = ReadFileFromImage(obj->m_imageIndex, &size);	// cdimage's buffer
		if(data && size > 0)
			walkDff(data, data + size, &m.polygons, &m.materials);
	}
	for(int slot = obj->m_txdSlot, depth = 0; slot >= 0 && depth < 8; depth++){
		readTxd(slot);
		TxdDef *txd = GetTxdDef(slot);
		slot = txd ? txd->parentId : -1;
	}
}

// ---- What the game files say ----

std::unordered_set<std::string>
readDynamicNames(std::unordered_set<std::string> *explosive)
{
	// data/object.dat: one line per model that moves when hit. Column L
	// (the 12th) is "causes explosion": barrels, petrol pumps, SAM sites.
	std::unordered_set<std::string> names;
	FILE *f = fopen(GamePath("data/object.dat").c_str(), "rb");
	if(f == nil)
		return names;
	char line[512];
	while(fgets(line, sizeof(line), f)){
		if(line[0] == ';' || line[0] == '*' || line[0] == '#')
			continue;
		char *fields[13];
		int n = 0;
		for(char *t = strtok(line, " \t,\r\n"); t && n < 13; t = strtok(nil, " \t,\r\n"))
			fields[n++] = t;
		if(n == 0 || strlen(fields[0]) > 63)
			continue;
		std::string name(fields[0]);
		for(char &c : name) c = (char)tolower((unsigned char)c);
		names.insert(name);
		if(explosive && n > 11 && atoi(fields[11]) != 0)
			explosive->insert(name);
	}
	fclose(f);
	return names;
}

bool
pathHas(const char *path, const char *part)
{
	if(path == nil)
		return false;
	std::string lower(path);
	for(char &c : lower){
		c = (char)tolower((unsigned char)c);
		if(c == '\\') c = '/';
	}
	return lower.find(part) != std::string::npos;
}

int
cityAt(const ObjectInst *inst)
{
	if(inst->m_area != 0)
		return CITY_INTERIORS;
	switch(Zones::GetMapZoneLevel(inst->m_translation)){
	case 1: return CITY_LOS_SANTOS;
	case 2: return CITY_SAN_FIERRO;
	case 3: return CITY_LAS_VENTURAS;
	}
	return CITY_COUNTRYSIDE;
}

// ---- Mine: the open map and the workspace ----

std::vector<bool> gInOpenMap, gInWorkspace;
uint32 gOpenMapSeq = 0xFFFFFFFFu;
std::string gWorkspaceScanned;
bool gWorkspaceValid;

void
scanPawnFile(const std::string &path)
{
	FILE *f = fopen(path.c_str(), "rb");
	if(f == nil)
		return;
	std::string text;
	char buf[65536];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 16u << 20)
		text.append(buf, n);
	fclose(f);
	// CreateObject(model, ... and the streamer's and Texture Studio's kin.
	static const char *calls[] = { "CreateObject(", "CreateDynamicObject(", "CreateDynamicObjectEx(",
		"CreatePlayerObject(", "CreateDynamicObject (" };
	for(const char *call : calls)
		for(size_t at = text.find(call); at != std::string::npos; at = text.find(call, at + 1)){
			const char *p = text.c_str() + at + strlen(call);
			if(strncmp(call, "CreatePlayerObject", 18) == 0){
				p = strchr(p, ',');	// after playerid
				if(p == nil) continue;
				p++;
			}
			while(*p == ' ' || *p == '\t') p++;
			if(!isdigit((unsigned char)*p))
				continue;
			int id = EditorModelId(atoi(p));
			if(id >= 0 && id < (int)gInWorkspace.size())
				gInWorkspace[id] = true;
		}
}

void
scanFolder(const std::string &dir, int depth)
{
	if(depth > 4)
		return;
	auto pawn = [](const std::string &name){
		size_t dot = name.rfind('.');
		if(dot == std::string::npos) return false;
		std::string ext = name.substr(dot);
		for(char &c : ext) c = (char)tolower((unsigned char)c);
		return ext == ".pwn" || ext == ".inc";
	};
#ifdef _WIN32
	WIN32_FIND_DATAA entry;
	HANDLE handle = FindFirstFileA((dir + "\\*").c_str(), &entry);
	if(handle == INVALID_HANDLE_VALUE)
		return;
	do{
		std::string name = entry.cFileName;
		if(name == "." || name == "..") continue;
		std::string path = dir + "\\" + name;
		if(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			scanFolder(path, depth + 1);
		else if(pawn(name))
			scanPawnFile(path);
	}while(FindNextFileA(handle, &entry));
	FindClose(handle);
#else
	DIR *d = opendir(dir.c_str());
	if(d == nil)
		return;
	while(dirent *ent = readdir(d)){
		std::string name = ent->d_name;
		if(name == "." || name == "..") continue;
		std::string path = dir + "/" + name;
		struct stat st;
		if(stat(path.c_str(), &st) != 0) continue;
		if(S_ISDIR(st.st_mode))
			scanFolder(path, depth + 1);
		else if(pawn(name))
			scanPawnFile(path);
	}
	closedir(d);
#endif
}

// What the model's definition says; also for custom models registered later.
void
fillModel(int id)
{
	ObjectDef *obj = GetObjectDef(id);
	if(obj == nil)
		return;
	ModelMeta &m = gMeta[id];
	m = ModelMeta();
	m.valid = true;
	m.polygons = m.materials = -1;
	if(obj->m_colModel){
		const CBox &box = obj->m_colModel->boundingBox;
		m.width = box.max.x - box.min.x;
		m.depth = box.max.y - box.min.y;
		m.height = box.max.z - box.min.z;
		m.hasCollision = m.width > 0.0f || m.depth > 0.0f || m.height > 0.0f;
	}
	m.twoSided = obj->m_noBackfaceCulling;
	std::string name(obj->m_name);
	for(char &c : name) c = (char)tolower((unsigned char)c);
	m.isLod = gLodTarget[id] || name.compare(0, 3, "lod") == 0;
	m.hasLod = GetLodForObject(id) >= 0;
	m.drawDistance = obj->GetLargestDrawDist();
	m.source = IsCustomModel(id) ? MODEL_SOURCE_CUSTOM : SampLoader::IsSampModel(id) ? MODEL_SOURCE_SAMP :
		MODEL_SOURCE_GTA;
	m.dynamic = gDynamic.count(name) != 0;
	m.explosive = gExplosive.count(name) != 0;
	m.breakable = obj->m_isDamageable;
	m.timed = obj->m_isTimed;
	m.timeOn = obj->m_timeOn;
	m.timeOff = obj->m_timeOff;
	m.animated = obj->m_animname[0] != '\0';
	m.vehicle = obj->m_isVehicle;
	m.skin = obj->m_isPed;
	m.ideFile = obj->m_file && obj->m_file->name ? obj->m_file->name : "";
	m.upgrade = pathHas(m.ideFile, "veh_mods.ide");
	m.interior = pathHas(m.ideFile, "/interior/") || pathHas(m.ideFile, "interior/");
	m.alpha = obj->m_drawLast || obj->m_additive || obj->m_noZwrite;
	// What the categories file under Buildings and land, and that stays put.
	int top = GetObjectCategory(id);
	while(top >= 0 && top < NUM_OBJ_CATEGORIES && objCategories[top].parent >= 0)
		top = objCategories[top].parent;
	m.building = top == CAT_BUILDINGS_ROOT && !m.dynamic;
	TxdDef *txd = GetTxdDef(obj->m_txdSlot);
	m.txdName = txd ? txd->name : "";
}

}	// namespace

void
BuildModelMeta(void)
{
	gMeta.assign(NUMOBJECTDEFS, ModelMeta());
	gZones.clear();
	gTxdRead.clear();
	gTxdTextures.clear();
	gDetailsNext = 0;
	gWorkspaceScanned.clear();
	gOpenMapSeq = 0xFFFFFFFFu;
	gExplosive.clear();
	gDynamic = readDynamicNames(&gExplosive);
	gLodTarget.assign(NUMOBJECTDEFS, false);
	for(int id = 0; id < NUMOBJECTDEFS; id++)
		if(GetObjectDef(id)){
			int lod = GetLodForObject(id);
			if(lod >= 0 && lod < NUMOBJECTDEFS)
				gLodTarget[lod] = true;
		}
	for(int id = 0; id < NUMOBJECTDEFS; id++)
		fillModel(id);

	// Where the world places each model: its city, and the zone by its name
	// in the game text (zones with the same name are one).
	std::map<std::string, int> zoneByName;
	std::unordered_map<int, int> zoneOf;	// navigation zone -> gZones index
	std::vector<std::set<int>> zonesOf(NUMOBJECTDEFS);
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_objectId < 0 || inst->m_objectId >= NUMOBJECTDEFS || !IsPawnWorldObject(inst))
			continue;
		ModelMeta &m = gMeta[inst->m_objectId];
		int city = cityAt(inst);
		m.placements++;
		m.cities |= 1u << city;
		if(city == CITY_INTERIORS)
			continue;
		int navig = Zones::FindNavigZone(inst->m_translation);
		if(navig < 0)
			continue;
		auto known = zoneOf.find(navig);
		int zone;
		if(known != zoneOf.end())
			zone = known->second;
		else{
			const char *key = Zones::GetNavigZoneText(navig);
			std::string name = GxtLookup(key);
			if(name.empty()) name = key;
			auto byName = zoneByName.find(name);
			if(byName == zoneByName.end()){
				zone = (int)gZones.size();
				gZones.push_back({ name, city });
				zoneByName[name] = zone;
			}else
				zone = byName->second;
			zoneOf[navig] = zone;
		}
		zonesOf[inst->m_objectId].insert(zone);
	}
	for(int id = 0; id < NUMOBJECTDEFS; id++)
		gMeta[id].zones.assign(zonesOf[id].begin(), zonesOf[id].end());
	log("ModelMeta: %d zones\n", (int)gZones.size());
}

const ModelMeta*
GetModelMeta(int id)
{
	if(id < 0 || id >= (int)gMeta.size())
		return nil;
	if(!gMeta[id].valid && GetObjectDef(id))
		fillModel(id);	// a custom model registered since
	return gMeta[id].valid ? &gMeta[id] : nil;
}

void
ReadAllModelMetaDetails(void)
{
	for(; gDetailsNext < (int)gMeta.size(); gDetailsNext++)
		if(GetModelMeta(gDetailsNext))
			readDetails(gDetailsNext);
}

void
RequestModelMetaDetails(void)
{
	gDetailsWanted = true;
}

void
ModelMetaTick(void)
{
	if(!gDetailsWanted || gDetailsNext >= (int)gMeta.size())
		return;
	// About 4 ms a frame, so the editor stays smooth while it reads.
	auto start = std::chrono::steady_clock::now();
	while(gDetailsNext < (int)gMeta.size() &&
	      std::chrono::steady_clock::now() - start < std::chrono::milliseconds(4)){
		for(int n = 0; n < 32 && gDetailsNext < (int)gMeta.size(); n++, gDetailsNext++)
			if(gMeta[gDetailsNext].valid)
				readDetails(gDetailsNext);
	}
}

float
ModelMetaDetailsProgress(void)
{
	return gMeta.empty() ? 1.0f : (float)gDetailsNext / (float)gMeta.size();
}

bool
ModelHasTextureContaining(int id, const char *text)
{
	ObjectDef *obj = GetObjectDef(id);
	if(obj == nil || text == nil || text[0] == '\0')
		return obj != nil;
	std::string needle(text);
	for(char &c : needle) c = (char)tolower((unsigned char)c);
	for(int slot = obj->m_txdSlot, depth = 0; slot >= 0 && depth < 8; depth++){
		if(slot < (int)gTxdTextures.size())
			for(const std::string &name : gTxdTextures[slot])
				if(name.find(needle) != std::string::npos)
					return true;
		TxdDef *txd = GetTxdDef(slot);
		slot = txd ? txd->parentId : -1;
	}
	return false;
}

int NumMetaZones(void) { return (int)gZones.size(); }
const char *GetMetaZoneName(int zone) { return zone >= 0 && zone < (int)gZones.size() ? gZones[zone].name.c_str() : ""; }
int GetMetaZoneCity(int zone) { return zone >= 0 && zone < (int)gZones.size() ? gZones[zone].city : -1; }

const char*
GetModelCityName(int city)
{
	static const char *names[NUM_MODEL_CITIES] = { "Los Santos", "San Fierro", "Las Venturas", "Countryside",
		"Interiors" };
	return city >= 0 && city < NUM_MODEL_CITIES ? names[city] : "";
}

const char*
GetModelSourceName(int source)
{
	static const char *names[NUM_MODEL_SOURCES] = { "GTA San Andreas", "SA-MP", "Custom (0.3.DL)" };
	return source >= 0 && source < NUM_MODEL_SOURCES ? names[source] : "";
}

bool
ModelInOpenMap(int id)
{
	uint32 seq = GetLatestChangeSeq();
	if(seq != gOpenMapSeq){
		gOpenMapSeq = seq;
		gInOpenMap.assign(NUMOBJECTDEFS, false);
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(!inst->m_isDeleted && !IsPawnWorldObject(inst) && inst->m_objectId >= 0 &&
			   inst->m_objectId < NUMOBJECTDEFS)
				gInOpenMap[inst->m_objectId] = true;
		}
	}
	return id >= 0 && id < (int)gInOpenMap.size() && gInOpenMap[id];
}

bool
ModelInWorkspace(int id)
{
	const char *dir = GetWorkspaceDir();
	if(dir == nil || dir[0] == '\0')
		return false;
	if(gWorkspaceScanned != dir){
		gWorkspaceScanned = dir;
		gInWorkspace.assign(NUMOBJECTDEFS, false);
		scanFolder(dir, 0);
	}
	return id >= 0 && id < (int)gInWorkspace.size() && gInWorkspace[id];
}

int
DumpModelMeta(const char *path)
{
	FILE *f = fopen(path, "w");
	if(f == nil)
		return -1;
	fputs("id\tname\tcategory\tsource\tide\ttxd\twidth\tdepth\theight\tcollision\tlod\thas_lod\tdraw_distance\t"
		"dynamic\texplosive\tbreakable\ttimed\tanimated\tupgrade\tinterior\talpha\tplacements\tcities\tzones\tpolygons\tmaterials\t"
		"textures\n", f);
	int n = 0;
	for(int id = 0; id < (int)gMeta.size(); id++){
		const ModelMeta &m = gMeta[id];
		ObjectDef *obj = GetObjectDef(id);
		if(!m.valid || obj == nil)
			continue;
		int category = GetObjectCategory(id);
		std::string cities, zones;
		for(int c = 0; c < NUM_MODEL_CITIES; c++)
			if(m.cities & 1u << c)
				cities += std::string(cities.empty() ? "" : ",") + GetModelCityName(c);
		for(int z : m.zones)
			zones += std::string(zones.empty() ? "" : ",") + GetMetaZoneName(z);
		int textures = 0;
		for(int slot = obj->m_txdSlot, depth = 0; slot >= 0 && depth < 8; depth++){
			if(slot < (int)gTxdTextures.size())
				textures += (int)gTxdTextures[slot].size();
			TxdDef *txd = GetTxdDef(slot);
			slot = txd ? txd->parentId : -1;
		}
		fprintf(f, "%d\t%s\t%s\t%s\t%s\t%s\t%.2f\t%.2f\t%.2f\t%d\t%d\t%d\t%.0f\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%d\t%d\t%d\n",
			PawnModelId(id), obj->m_name, category >= 0 && category < NUM_OBJ_CATEGORIES ? objCategories[category].name : "",
			GetModelSourceName(m.source), m.ideFile, m.txdName, m.width, m.depth, m.height, m.hasCollision,
			m.isLod, m.hasLod, m.drawDistance, m.dynamic, m.explosive, m.breakable, m.timed, m.animated, m.upgrade,
			m.interior, m.alpha, m.placements, cities.c_str(), zones.c_str(), m.polygons, m.materials, textures);
		n++;
	}
	fclose(f);
	return n;
}
