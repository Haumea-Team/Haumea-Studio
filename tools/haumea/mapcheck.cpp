// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "i18n.h"
#include "trains.h"
#include "rec.h"
#include "mapcheck.h"
#include "workspace.h"
#include "textdraws.h"
#include "encoding.h"
#include "radarart.h"
#include "materialtext.h"
#include "modelmeta.h"
#include "anim.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#else
#define strcasecmp _stricmp
#endif
#include <sys/stat.h>
#include <map>
#include <regex>
#include <set>

namespace {

// The Map Check panel shows the results in the UI's language; the agent
// (agentbridge's map_check) gets them in English.
bool gTranslated;

const char*
tr(const char *english)
{
	return gTranslated ? _(english) : english;
}

// open.mp SDK values.hpp; SA-MP 0.3.7 has the same numbers.
const int OBJECTS_037 = 1000;	// OBJECT_POOL_SIZE_037, and SA-MP's MAX_OBJECTS
const int OBJECTS_OMP = 2000;	// OBJECT_POOL_SIZE, with artwork on and no 0.3.7 clients
const int VEHICLES = 2000;
const int PICKUPS = 4096;
const int LABELS = 1024;
const int ACTORS = 1000;
const int GANG_ZONES = 1024;
const int CLASSES = 320;
const int MAP_ICONS = 100;	// SetPlayerMapIcon iconid 0-99
const int CLIENT_OBJECTS = 1000;	// objects a 0.3.7 client creates at once
const float STREAMER_OBJECT_SD = 300.0f;
const int MIN_NAME = 3, MAX_NAME = 24;
const int GLOBAL_TEXTDRAWS = 2048, PLAYER_TEXTDRAWS = 256;	// GLOBAL_/PLAYER_TEXTDRAW_POOL_SIZE
const int MAX_TEXTDRAW_STR_LENGTH = 800;
const float TEXTDRAW_CANVAS_W = 640.0f, TEXTDRAW_CANVAS_H = 448.0f;

struct ServerConfig
{
	bool found;
	bool artwork;
	bool allow037;
	int maxPlayers;
};

bool
fileExists(const std::string &path)
{
	struct stat st;
	if(stat(path.c_str(), &st) != 0)
		return false;
#ifdef _WIN32
	return (st.st_mode & _S_IFMT) == _S_IFREG;
#else
	return S_ISREG(st.st_mode);
#endif
}

std::string
readFile(const std::string &path)
{
	std::string text;
	FILE *f = fopen(path.c_str(), "rb");
	if(f == nil)
		return text;
	char buf[4096];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), f)) > 0)
		text.append(buf, n);
	fclose(f);
	return text;
}

// The few config.json values that change limits; open.mp's defaults otherwise.
ServerConfig
readServerConfig(void)
{
	ServerConfig config = { false, false, true, 50 };
	if(GetWorkspaceDir()[0] == '\0')
		return config;
	std::string text = readFile(WorkspacePath("config.json"));
	if(text.empty())
		return config;
	config.found = true;
	std::smatch m;
	if(std::regex_search(text, m, std::regex(R"re("max_players"\s*:\s*(\d+))re")))
		config.maxPlayers = atoi(m[1].str().c_str());
	if(std::regex_search(text, m, std::regex(R"re("artwork"\s*:\s*\{[^}]*"enable"\s*:\s*(true|false))re")))
		config.artwork = m[1] == "true";
	if(std::regex_search(text, m, std::regex(R"re("allow_037_clients"\s*:\s*(true|false))re")))
		config.allow037 = m[1] == "true";
	return config;
}

bool
validName(const char *name)
{
	int len = (int)strlen(name);
	if(len < MIN_NAME || len > MAX_NAME)
		return false;
	// open.mp's player pool: letters, digits and ][_$=()
	for(const char *p = name; *p; p++)
		if(!isalnum((unsigned char)*p) && !strchr("][_$=()", *p))
			return false;
	return true;
}

// {RRGGBB} embeds: a '{' that does not start one shows as text in game.
bool
badColourCode(const char *text)
{
	for(const char *p = text; *p; p++){
		if(*p != '{')
			continue;
		bool ok = strlen(p) >= 8 && p[7] == '}';
		for(int i = 1; ok && i <= 6; i++)
			ok = isxdigit((unsigned char)p[i]) != 0;
		if(!ok)
			return true;
		p += 7;
	}
	return false;
}

rw::Geometry*
instanceGeometry(ObjectInst *inst)
{
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil)
		return nil;
	if(obj->m_type == ObjectDef::ATOMIC){
		if(inst->m_rwObject)
			return ((rw::Atomic*)inst->m_rwObject)->geometry;
		return obj->m_atomics[0] ? obj->m_atomics[0]->geometry : nil;
	}
	rw::Clump *clump = inst->m_rwObject ? (rw::Clump*)inst->m_rwObject : obj->m_clump;
	if(clump)
		FORLIST(lnk, clump->atomics)
			return rw::Atomic::fromClump(lnk)->geometry;
	return nil;
}

struct Checker
{
	MapCheckResult *result;

	void add(int severity, const char *category, std::vector<int> insts, const char *fmt, ...)
	{
		char text[512];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(text, sizeof(text), fmt, ap);
		va_end(ap);
		MapCheckItem item;
		item.severity = severity;
		item.category = category;
		item.text = text;
		item.instances.swap(insts);
		result->items.push_back(item);
	}
	void add(int severity, const char *category, ObjectInst *inst, const char *fmt, ...)
	{
		char text[512];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(text, sizeof(text), fmt, ap);
		va_end(ap);
		std::vector<int> insts;
		if(inst)
			insts.push_back(inst->m_id);
		add(severity, category, insts, "%s", text);
	}
};

std::vector<int>
ids(const std::vector<ObjectInst*> &insts)
{
	std::vector<int> out;
	for(ObjectInst *inst : insts)
		if(!IsRadarArtStandIn(inst))	// not in the map to select
			out.push_back(inst->m_id);
	return out;
}

const char*
label(const ObjectInst *inst, char *buf, size_t size)
{
	const ObjectPawnProperties *p = GetObjectPawnProperties(inst);
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	int kind = GetSampElementKind(inst);
	const char *what = kind != SAMP_ELEMENT_OBJECT ? GetSampElementKindName(kind) :
		obj ? obj->m_name : tr(N_("object"));
	if(p->name[0])
		snprintf(buf, size, "%s (%s)", p->name, what);
	else
		snprintf(buf, size, "%s #%d", what, inst->m_id);
	return buf;
}

}	// namespace

const char*
MapCheckSeverityName(int severity)
{
	return severity == MAPCHECK_ERROR ? _("Problem") : severity == MAPCHECK_WARNING ? _("Warning") : _("Note");
}

void
RunMapCheck(int pawnFormat, MapCheckResult *result, bool translated)
{
	gTranslated = translated;
	result->items.clear();
	result->pawnFormat = pawnFormat;
	Checker c = { result };
	bool streamer = pawnFormat == PAWN_CODE_STREAMER;
	ServerConfig config = readServerConfig();
	char name[160];

	std::vector<ObjectInst*> objects, vehicles, byKind[NUM_SAMP_ELEMENT_KINDS], customModels;
	std::map<std::string, std::vector<ObjectInst*>> variables;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted || !PawnObjectNeedsExport(inst))
			continue;
		int kind = GetSampElementKind(inst);
		if(inst->m_isVehicle)
			vehicles.push_back(inst);
		else if(kind == SAMP_ELEMENT_OBJECT)
			objects.push_back(inst);
		byKind[kind].push_back(inst);
		if(IsCustomModel(inst->m_objectId))
			customModels.push_back(inst);
		const ObjectPawnProperties *props = GetObjectPawnProperties(inst);
		if(props->name[0])
			variables[props->name].push_back(inst);
	}
	// Radar art's zones count as the gang zones the Pawn file makes of them.
	struct StandIns {
		std::vector<ObjectInst*> list;
		~StandIns() { FreeRadarArtStandIns(list); }
	} standIns;
	MakeRadarArtStandIns(standIns.list);
	byKind[SAMP_ELEMENT_GANGZONE].insert(byKind[SAMP_ELEMENT_GANGZONE].end(), standIns.list.begin(), standIns.list.end());

	// --- Limits ---
	const char *LIMITS = tr(N_("Limits"));
	int objectLimit = OBJECTS_037;
	const char *objectWhy = tr(N_("SA-MP 0.3.7 and open.mp with 0.3.7 clients allowed"));
	if(config.found && config.artwork && !config.allow037){
		objectLimit = OBJECTS_OMP;
		objectWhy = tr(N_("open.mp with artwork on and no 0.3.7 clients (config.json)"));
	}
	if(!streamer){
		if((int)objects.size() > objectLimit)
			c.add(MAPCHECK_ERROR, LIMITS, ids(objects), tr(N_("%d objects, but CreateObject stops at %d (%s). "
				"Objects past the limit are not created; save with Streamer code instead.")),
				(int)objects.size(), objectLimit, objectWhy);
		else if((int)objects.size() > objectLimit * 9 / 10)
			c.add(MAPCHECK_WARNING, LIMITS, std::vector<int>(), tr(N_("%d of %d objects (%s): the rest of the gamemode "
				"has little room left.")), (int)objects.size(), objectLimit, objectWhy);
	}else if(objects.size() > CLIENT_OBJECTS){
		// The streamer creates what is near a player; find the busiest spot.
		// Objects in 300 m cells: a spot's neighbours are in the 3x3 cells around it.
		int worst = 0;
		ObjectInst *worstAt = nil;
		auto cellOf = [](float v){ return (int)floorf(v / STREAMER_OBJECT_SD); };
		std::map<std::pair<int, int>, std::vector<ObjectInst*>> cells;
		for(ObjectInst *inst : objects)
			cells[{ cellOf(inst->m_translation.x), cellOf(inst->m_translation.y) }].push_back(inst);
		size_t step = objects.size() > 4000 ? objects.size() / 4000 + 1 : 1;
		for(size_t i = 0; i < objects.size(); i += step){
			int n = 0;
			rw::V3d at = objects[i]->m_translation;
			int cx = cellOf(at.x), cy = cellOf(at.y);
			for(int dx = -1; dx <= 1; dx++)
			for(int dy = -1; dy <= 1; dy++){
				auto cell = cells.find({ cx + dx, cy + dy });
				if(cell == cells.end())
					continue;
				for(ObjectInst *other : cell->second){
					rw::V3d d = sub(other->m_translation, at);
					if(d.x*d.x + d.y*d.y + d.z*d.z < STREAMER_OBJECT_SD*STREAMER_OBJECT_SD)
						n++;
				}
			}
			if(n > worst){
				worst = n;
				worstAt = objects[i];
			}
		}
		if(worst > CLIENT_OBJECTS)
			c.add(MAPCHECK_WARNING, LIMITS, worstAt, tr(N_("%d objects within %.0f m of one spot: a 0.3.7 client "
				"shows at most %d at once, so the streamer will leave some out there.")),
				worst, STREAMER_OBJECT_SD, CLIENT_OBJECTS);
	}
	// Gang zones by how they are shown: only global ones share the server's
	// pool; every player has their own 1024, global ones included.
	std::vector<ObjectInst*> zonesBy[NUM_SAMP_ZONE_MODES];
	for(ObjectInst *inst : byKind[SAMP_ELEMENT_GANGZONE])
		zonesBy[GetObjectPawnProperties(inst)->zoneMode].push_back(inst);
	byKind[SAMP_ELEMENT_GANGZONE] = zonesBy[SAMP_ZONE_GLOBAL];
	int globalZones = (int)zonesBy[SAMP_ZONE_GLOBAL].size();
	int playerZones = globalZones + (int)zonesBy[SAMP_ZONE_PLAYER].size();
	if(!zonesBy[SAMP_ZONE_PLAYER].empty() && playerZones > GANG_ZONES)
		c.add(MAPCHECK_ERROR, LIMITS, ids(zonesBy[SAMP_ZONE_PLAYER]), tr(N_("%d gang zones for every player (%d per-player, "
			"%d global), over the %d a player can have; the extra ones are not shown.")), playerZones,
			(int)zonesBy[SAMP_ZONE_PLAYER].size(), globalZones, GANG_ZONES);
	if(!zonesBy[SAMP_ZONE_STREAMED].empty()){
		std::vector<float> bounds;
		for(ObjectInst *inst : zonesBy[SAMP_ZONE_STREAMED]){
			const ObjectPawnProperties *p = GetObjectPawnProperties(inst);
			bounds.push_back(inst->m_translation.x - p->zoneSizeX * 0.5f);
			bounds.push_back(inst->m_translation.y - p->zoneSizeY * 0.5f);
			bounds.push_back(inst->m_translation.x + p->zoneSizeX * 0.5f);
			bounds.push_back(inst->m_translation.y + p->zoneSizeY * 0.5f);
		}
		StreamedZoneGrid grid;
		GetStreamedZoneGrid(bounds, &grid);
		if(grid.capacity + playerZones > GANG_ZONES)
			c.add(MAPCHECK_WARNING, LIMITS, ids(zonesBy[SAMP_ZONE_STREAMED]), tr(N_("Up to %d streamed gang zones around "
				"one spot, plus %d shown to everyone: a player can have %d, so some near there are left out. "
				"Use bigger cells or fewer colours in the radar art.")), grid.capacity, playerZones, GANG_ZONES);
	}
	std::vector<ObjectInst*> ompZones = zonesBy[SAMP_ZONE_PLAYER];
	ompZones.insert(ompZones.end(), zonesBy[SAMP_ZONE_STREAMED].begin(), zonesBy[SAMP_ZONE_STREAMED].end());
	struct { int kind; int limit; bool streamerFree; const char *what; } pools[] = {
		{ SAMP_ELEMENT_PICKUP, PICKUPS, true, N_("pickups") },
		{ SAMP_ELEMENT_LABEL, LABELS, true, N_("3D text labels") },
		{ SAMP_ELEMENT_ACTOR, ACTORS, true, N_("actors") },
		{ SAMP_ELEMENT_GANGZONE, GANG_ZONES, false, N_("gang zones") },
		{ SAMP_ELEMENT_CLASS, CLASSES, false, N_("player classes") },
		{ SAMP_ELEMENT_MAPICON, MAP_ICONS, true, N_("map icons") },
	};
	for(auto &pool : pools){
		int n = (int)byKind[pool.kind].size();
		if(n > pool.limit && !(streamer && pool.streamerFree))
			c.add(MAPCHECK_ERROR, LIMITS, ids(byKind[pool.kind]), tr(N_("%d %s, over the server's limit of %d; "
				"the extra ones are not created.")), n, tr(pool.what), pool.limit);
	}
	// A train takes a vehicle id for the engine and each carriage.
	int vehicleIds = 0, trainIds = 0;
	for(ObjectInst *inst : vehicles){
		int n = IsTrainEngineModel(inst->m_objectId) ? GetTrainVehicleIds(inst->m_objectId) : 1;
		vehicleIds += n;
		if(n > 1)
			trainIds += n;
	}
	if(vehicleIds > VEHICLES){
		if(trainIds > 0)
			c.add(MAPCHECK_ERROR, LIMITS, ids(vehicles), tr(N_("%d vehicle ids (%d of them for trains, 4 each), over the "
				"limit of %d.")), vehicleIds, trainIds, VEHICLES);
		else
			c.add(MAPCHECK_ERROR, LIMITS, ids(vehicles), tr(N_("%d vehicles, over the limit of %d.")), vehicleIds, VEHICLES);
	}

	// --- Trains ---
	const char *TRAINS = tr(N_("Trains"));
	for(ObjectInst *inst : vehicles){
		int model = inst->m_objectId;
		if(IsTrainEngineModel(model)){
			if(!inst->m_vehicleStatic)
				c.add(MAPCHECK_ERROR, TRAINS, inst, tr(N_("%s: trains are only made with AddStaticVehicle(Ex); open.mp's "
					"CreateVehicle refuses them. Tick Static vehicle.")), label(inst, name, sizeof(name)));
			TrainCar cars[TRAIN_MAX_CARS];
			float off = 0.0f;
			if(ComputeTrainCars(model, inst->m_translation, GetTrainInstAngle(inst), cars, &off) > 0 && off > 3.0f)
				c.add(MAPCHECK_WARNING, TRAINS, inst, tr(N_("%s is %.0f m off the track: in game it stands on the nearest "
					"track instead, at %.1f, %.1f, %.1f.")), label(inst, name, sizeof(name)), off,
					cars[0].position.x, cars[0].position.y, cars[0].position.z);
		}else if(model == 569 || model == 570)
			c.add(MAPCHECK_WARNING, TRAINS, inst, tr(N_("%s: a carriage on its own isn't made in game (the client says \"bad "
				"train carriages\"); place a %s, which brings three.")), label(inst, name, sizeof(name)),
				model == 570 ? tr(N_("Brown Streak (538)")) : tr(N_("Freight (537)")));
		else if(model == 590)
			c.add(MAPCHECK_WARNING, TRAINS, inst, tr(N_("%s: no train in the game pulls the box freight, so it stands "
				"alone, off any track.")), label(inst, name, sizeof(name)));
	}

	// --- Removed buildings ---
	// One RemoveBuildingForPlayer per removed or changed world building, and one
	// per -1 area removal (which covers every building inside it).
	const int REMOVALS = 1000;	// SA-MP 0.3.7 removes at most this many per player
	std::vector<ObjectInst*> removedOne, inArea;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!IsPawnWorldObject(inst))
			continue;
		bool covered = InsidePawnAreaRemoval(inst);
		if(!inst->m_isDeleted && covered)
			inArea.push_back(inst);
		else if(PawnObjectNeedsExport(inst) && !(inst->m_isDeleted && covered))
			removedOne.push_back(inst);
	}
	int areas = (int)GetPawnAreaRemovals().size();
	int lines = areas + (int)removedOne.size();
	if(lines > REMOVALS)
		c.add(MAPCHECK_ERROR, LIMITS, ids(removedOne), tr(N_("%d RemoveBuildingForPlayer calls, over SA-MP's %d per player: "
			"the rest stay in the world.")), lines, REMOVALS);
	else if(areas > 0)
		c.add(MAPCHECK_INFO, LIMITS, ids(removedOne), tr(N_("%d RemoveBuildingForPlayer calls: %d for whole areas (model -1) "
			"and %d for single buildings.")), lines, areas, (int)removedOne.size());
	if(!inArea.empty())
		c.add(MAPCHECK_WARNING, LIMITS, ids(inArea), tr(N_("%d world building(s) shown here are inside a model -1 removal: "
			"in game they stay removed.")), (int)inArea.size());
	int npcs = (int)byKind[SAMP_ELEMENT_NPC].size();
	if(npcs > 0){
		if(config.found && npcs > config.maxPlayers)
			c.add(MAPCHECK_ERROR, LIMITS, ids(byKind[SAMP_ELEMENT_NPC]), tr(N_("%d NPCs, but config.json allows %d players "
				"and every NPC takes a player slot.")), npcs, config.maxPlayers);
		else
			c.add(MAPCHECK_INFO, LIMITS, std::vector<int>(), tr(N_("%d NPC(s) will take %d player slot(s)%s.")), npcs, npcs,
				config.found ? "" : tr(N_(" (no config.json in the workspace to check max_players against)")));
	}

	// --- Models ---
	const char *MODELS = tr(N_("Models"));
	const PawnImportReport &report = GetLastPawnImportReport();
	if(report.unknownModelObjects > 0){
		std::string list, custom;
		for(int id : report.unknownModels){
			// AddSimpleModel IDs live at CUSTOM_MODEL_BASE + 1000..30000 in the editor.
			bool isCustom = id >= CUSTOM_MODEL_BASE + 1000 && id <= CUSTOM_MODEL_BASE + 30000;
			std::string &to = isCustom ? custom : list;
			if(to.size() < 200)
				to += (to.empty() ? "" : ", ") + std::to_string(isCustom ? PawnModelId(id) : id);
		}
		c.add(MAPCHECK_ERROR, MODELS, std::vector<int>(), tr(N_("%d object(s) with models the editor doesn't know were left "
			"out when the map was opened, and saving drops them.%s%s%s%s")), report.unknownModelObjects,
			list.empty() ? "" : tr(N_(" IDs: ")), list.c_str(),
			custom.empty() ? "" : list.empty() ? tr(N_(" Custom IDs not in artconfig.txt or AddSimpleModel: ")) :
				tr(N_(". Custom IDs not in artconfig.txt or AddSimpleModel: ")), custom.c_str());
		// What to do about it.
		c.add(MAPCHECK_INFO, MODELS, std::vector<int>(), tr(N_("To keep them: a SA-MP 0.3.DL or open.mp custom model "
			"goes in the workspace's models/artconfig.txt (AddSimpleModel) with its DFF and TXD; anything "
			"else needs a similar game or SA-MP object in its place (search the Object Browser).")));
	}
	std::vector<ObjectInst*> tiltedTrees, explosive;
	for(ObjectInst *inst : objects){
		ObjectDef *obj = GetObjectDef(inst->m_objectId);
		if(obj == nil || obj->m_cantLoad){
			c.add(MAPCHECK_ERROR, MODELS, inst, tr(N_("%s: its model can't be loaded.")), label(inst, name, sizeof(name)));
			continue;
		}
		// Trees and palms sway: CEntity::ModifyMatrixForTreeInWind (gta-reversed
		// Entity.cpp) rewrites the up axis's X and Y every frame, so a tilt is lost.
		// The up axis's Z is 1 - 2(x^2 + y^2); under cos(0.5 deg) it leans.
		const rw::Quat &q = inst->m_rotation;
		if(obj->m_type == ObjectDef::ATOMIC && (obj->m_isTree || obj->m_isPalmTree) && !obj->m_isTag &&
		   1.0f - 2.0f*(q.x*q.x + q.y*q.y) < 0.99996f)
			tiltedTrees.push_back(inst);
		const ModelMeta *meta = GetModelMeta(inst->m_objectId);
		if(meta && meta->explosive)
			explosive.push_back(inst);
		const ObjectPawnProperties *p = GetObjectPawnProperties(inst);
		if(p->moveEnabled && length(p->moveOffset) < 0.001f && !p->moveRotates)
			c.add(MAPCHECK_WARNING, MODELS, inst, tr(N_("%s moves 0 m and doesn't turn, so its MoveObject does nothing.")),
				label(inst, name, sizeof(name)));
	}
	if(!tiltedTrees.empty())
		c.add(MAPCHECK_WARNING, MODELS, ids(tiltedTrees), tr(N_("%d tree(s) or palm(s) tilted on X or Y: the game sways "
			"them in the wind and stands them upright, so the tilt doesn't show in game.")), (int)tiltedTrees.size());
	if(!explosive.empty())
		c.add(MAPCHECK_INFO, MODELS, ids(explosive), tr(N_("%d object(s) explode when shot or hit (object.dat), such as "
			"barrels and petrol pumps. Each player can blow them up on their own screen.")), (int)explosive.size());

	// --- Positions ---
	const char *PLACES = tr(N_("Positions"));
	std::vector<ObjectInst*> outside, offMap;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted || !PawnObjectNeedsExport(inst))
			continue;
		float x = fabsf(inst->m_translation.x), y = fabsf(inst->m_translation.y), z = fabsf(inst->m_translation.z);
		if(x > SA_MP_WORLD_BOUND || y > SA_MP_WORLD_BOUND || z > SA_MP_WORLD_BOUND)
			outside.push_back(inst);
		else if(x > SA_GAME_WATER_BOUND || y > SA_GAME_WATER_BOUND)
			offMap.push_back(inst);
	}
	if(!outside.empty())
		c.add(MAPCHECK_ERROR, PLACES, ids(outside), tr(N_("%d element(s) outside SA-MP's world bounds (\xC2\xB1%d): "
			"the server clamps or ignores those positions.")), (int)outside.size(), SA_MP_WORLD_BOUND);
	if(!offMap.empty())
		c.add(MAPCHECK_INFO, PLACES, ids(offMap), tr(N_("%d element(s) beyond the San Andreas map (\xC2\xB1%d). "
			"They work, but the game's water, map and radar end there.")), (int)offMap.size(), SA_GAME_WATER_BOUND);

	// --- Materials ---
	const char *MATERIALS = tr(N_("Materials"));
	std::map<std::string, std::vector<int>> missingFonts;	// lowercase name -> objects
	std::map<std::string, std::string> missingFontNames, missingFontPreviews;
	for(ObjectInst *inst : objects){
		if(!HasObjectMaterialOverrides(inst))
			continue;
		rw::Geometry *geometry = instanceGeometry(inst);
		int count = geometry ? geometry->matList.numMaterials : -1;
		for(int slot = 0; slot < MAX_PAWN_MATERIALS; slot++){
			const ObjectMaterialOverride *m = GetObjectMaterialOverride(inst, slot);
			if(m == nil || (!m->active && !m->textEnabled))
				continue;
			if(count >= 0 && slot >= count)
				c.add(MAPCHECK_WARNING, MATERIALS, inst, tr(N_("%s: material slot %d is set, but the model has %d slot(s), "
					"so it does nothing.")), label(inst, name, sizeof(name)), slot, count);
			if(m->active && !m->textEnabled){
				// The TXD and texture as the client looks them up (game, SA-MP
				// or the workspace's custom models).
				char why[512];
				int check = CheckMaterialTexture(*m, why, sizeof(why), gTranslated);
				if(check != MATERIAL_TEXTURE_OK)
					c.add(check == MATERIAL_TEXTURE_ERROR ? MAPCHECK_ERROR :
						check == MATERIAL_TEXTURE_NOTE ? MAPCHECK_INFO : MAPCHECK_WARNING, MATERIALS, inst,
						tr(N_("%s: material slot %d: %s")), label(inst, name, sizeof(name)), slot, why);
			}
			if(m->textEnabled){
				std::string preview;
				if(DescribeMaterialFont(m->font, m->bold, &preview) == MATERIAL_FONT_MISSING){
					std::string key = m->font;
					for(char &ch : key) ch = tolower((unsigned char)ch);
					std::vector<int> &list = missingFonts[key];
					if(list.empty() || list.back() != inst->m_id)
						list.push_back(inst->m_id);
					missingFontNames[key] = m->font;
					missingFontPreviews[key] = preview;
				}
				if(badColourCode(m->text))
					c.add(MAPCHECK_WARNING, MATERIALS, inst, tr(N_("%s: material text in slot %d has a '{' that isn't a "
						"{RRGGBB} colour code; it shows as text.")), label(inst, name, sizeof(name)), slot);
				if((int)strlen(m->text) >= PAWN_MATERIAL_TEXT_LEN - 1)
					c.add(MAPCHECK_WARNING, MATERIALS, inst, tr(N_("%s: material text in slot %d is at the %d-character "
						"limit and may have been cut.")), label(inst, name, sizeof(name)), slot, PAWN_MATERIAL_TEXT_LEN - 1);
			}
		}
	}

	for(auto &f : missingFonts)
		c.add(MAPCHECK_INFO, MATERIALS, f.second, tr(N_("%d object(s) use the font \"%s\", which isn't installed here: "
			"the preview uses %s. Players see it only if they have the font. To preview it, put its .ttf in "
			"the fonts folder (Object Info, Open fonts folder) and rescan.")), (int)f.second.size(),
			missingFontNames[f.first].c_str(), missingFontPreviews[f.first].c_str());

	// --- Elements ---
	const char *ELEMENTS = tr(N_("Elements"));
	for(int kind : { SAMP_ELEMENT_ACTOR, SAMP_ELEMENT_NPC })
		for(ObjectInst *inst : byKind[kind]){
			const ObjectPawnProperties *p = GetObjectPawnProperties(inst);
			int problem = p->animName[0] ? AnimPedProblem(p->animLib, p->animName) : ANIM_PED_OK;
			if(problem == ANIM_PED_OBJECT_ONLY)
				c.add(MAPCHECK_WARNING, ELEMENTS, inst, tr(N_("%s plays %s / %s, which moves only an object's parts: "
					"the skin stands still.")), label(inst, name, sizeof(name)), p->animLib, p->animName);
			else if(problem == ANIM_PED_DEFORMS)
				c.add(MAPCHECK_WARNING, ELEMENTS, inst, tr(N_("%s plays %s / %s, which twists the skin out of shape.")),
					label(inst, name, sizeof(name)), p->animLib, p->animName);
		}
	for(ObjectInst *inst : byKind[SAMP_ELEMENT_LABEL]){
		const ObjectPawnProperties *p = GetObjectPawnProperties(inst);
		if(p->labelText[0] == '\0')
			c.add(MAPCHECK_WARNING, ELEMENTS, inst, tr(N_("%s has no text.")), label(inst, name, sizeof(name)));
		if(badColourCode(p->labelText))
			c.add(MAPCHECK_WARNING, ELEMENTS, inst, tr(N_("%s: a '{' that isn't a {RRGGBB} colour code shows as text.")),
				label(inst, name, sizeof(name)));
		if((int)strlen(p->labelText) >= PAWN_LABEL_TEXT_LEN - 1)
			c.add(MAPCHECK_WARNING, ELEMENTS, inst, tr(N_("%s is at the %d-character limit and may have been cut.")),
				label(inst, name, sizeof(name)), PAWN_LABEL_TEXT_LEN - 1);
	}
	for(ObjectInst *inst : byKind[SAMP_ELEMENT_SOUND]){
		const ObjectPawnProperties *p = GetObjectPawnProperties(inst);
		if(p->soundKind == SAMP_SOUND_STREAM &&
		   strncmp(p->soundUrl, "http://", 7) != 0 && strncmp(p->soundUrl, "https://", 8) != 0)
			c.add(MAPCHECK_ERROR, ELEMENTS, inst, tr(N_("%s: PlayAudioStreamForPlayer needs an http:// or https:// URL%s.")),
				label(inst, name, sizeof(name)), p->soundUrl[0] ? "" : tr(N_(", and it has none")));
	}
	for(auto &v : variables)
		if(v.second.size() > 1)
			c.add(MAPCHECK_ERROR, ELEMENTS, ids(v.second), tr(N_("%d elements share the variable name \"%s\": "
				"the Pawn code won't compile.")), (int)v.second.size(), v.first.c_str());
	std::vector<ObjectInst*> playerAttachmentsSeen;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted || inst->m_pawnProperties == nil ||
		   inst->m_pawnProperties->attachParent == PAWN_ATTACH_NONE)
			continue;
		const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
		bool playerPreview = properties->attachParent == PAWN_ATTACH_PLAYER;
		ObjectInst *parent = playerPreview ? nil : GetInstanceByID(properties->attachParent);
		if(!playerPreview && (parent == nil || parent->m_isDeleted)){
			c.add(MAPCHECK_ERROR, ELEMENTS, inst, tr(N_("%s is attached to an object that was deleted.")),
				label(inst, name, sizeof(name)));
			continue;
		}
		if(properties->attachBone >= 1){
			if(!playerPreview && !IsPlayerAttachmentTarget(parent)){
				c.add(MAPCHECK_ERROR, ELEMENTS, inst, tr(N_("%s uses a player bone attachment, but its target isn't a player or NPC.")),
					label(inst, name, sizeof(name)));
				continue;
			}
			int currentKind = playerPreview ? -1 : GetSampElementKind(parent);
			int currentSkin = playerPreview ? properties->attachSkin : GetObjectPawnProperties(parent)->elementModel;
			for(ObjectInst *previous : playerAttachmentsSeen){
				const ObjectPawnProperties *old = GetObjectPawnProperties(previous);
				if(old->attachSlot != properties->attachSlot)
					continue;
				bool oldGlobal = old->attachParent == PAWN_ATTACH_PLAYER;
				ObjectInst *oldTarget = old->attachParent >= 0 ? GetInstanceByID(old->attachParent) : nil;
				int oldKind = oldTarget ? GetSampElementKind(oldTarget) : -1;
				int oldSkin = oldGlobal ? old->attachSkin : oldTarget ? GetObjectPawnProperties(oldTarget)->elementModel : -1;
				bool samePlayer = playerPreview || oldGlobal || old->attachParent == properties->attachParent;
				if(!samePlayer && (currentKind == SAMP_ELEMENT_CLASS || currentKind == SAMP_ELEMENT_NPC) &&
				   (oldKind == SAMP_ELEMENT_CLASS || oldKind == SAMP_ELEMENT_NPC))
					samePlayer = currentSkin == oldSkin &&
						(currentKind == SAMP_ELEMENT_CLASS || oldKind == SAMP_ELEMENT_CLASS);
				if(!samePlayer)
					continue;
				char currentLabel[128], previousLabel[128];
				label(inst, currentLabel, sizeof(currentLabel));
				label(previous, previousLabel, sizeof(previousLabel));
				c.add(MAPCHECK_ERROR, ELEMENTS, inst, tr(N_("%s shares player attachment slot %d with %s.")),
					currentLabel, properties->attachSlot, previousLabel);
				c.add(MAPCHECK_ERROR, ELEMENTS, previous, tr(N_("%s shares player attachment slot %d with %s.")),
					previousLabel, properties->attachSlot, currentLabel);
			}
			playerAttachmentsSeen.push_back(inst);
		}else if(parent && !parent->m_isVehicle && GetSampElementKind(parent) != SAMP_ELEMENT_OBJECT){
			c.add(MAPCHECK_ERROR, ELEMENTS, inst, tr(N_("%s is attached to a target that can't be used by Pawn.")),
				label(inst, name, sizeof(name)));
		}
	}

	// --- NPCs (open.mp) ---
	const char *NPCS = tr(N_("NPCs"));
	std::set<std::string> usedPaths;
	std::map<std::string, std::vector<ObjectInst*>> npcNames;
	std::map<std::pair<int32, int>, std::vector<ObjectInst*>> seats;	// (vehicle, seat)
	for(ObjectInst *inst : byKind[SAMP_ELEMENT_NPC]){
		const ObjectPawnProperties *p = GetObjectPawnProperties(inst);
		npcNames[p->npcName].push_back(inst);
		ObjectInst *vehicle = GetNpcVehicle(inst);
		if(p->npcVehicle >= 0 && vehicle == nil)
			c.add(MAPCHECK_ERROR, NPCS, inst, tr(N_("%s sits in a vehicle that was deleted, so it is saved on foot.")),
				label(inst, name, sizeof(name)));
		if(vehicle)
			seats[std::make_pair(vehicle->m_id, p->npcSeat)].push_back(inst);
		if(p->npcRecording[0]){
			int type = RecTypeOfName(p->npcRecording);
			if(type == REC_DRIVER && !(vehicle && p->npcSeat == 0))
				c.add(MAPCHECK_ERROR, NPCS, inst, tr(N_("%s plays driver recording %s but isn't driving a vehicle: "
					"open.mp reports it playing, yet the NPC never moves. Put it in a vehicle as the driver.")),
					label(inst, name, sizeof(name)), p->npcRecording);
			else if(type == REC_ONFOOT && vehicle)
				c.add(MAPCHECK_WARNING, NPCS, inst, tr(N_("%s plays on-foot recording %s while in a vehicle; "
					"it needs to be on foot.")), label(inst, name, sizeof(name)), p->npcRecording);
		}
		if(!validName(p->npcName))
			c.add(MAPCHECK_ERROR, NPCS, inst, tr(N_("%s: \"%s\" isn't a valid player name (%d-%d letters, digits or ][_$=()), "
				"so NPC_Create fails.")), label(inst, name, sizeof(name)), p->npcName, MIN_NAME, MAX_NAME);
		if(p->npcRecording[0]){
			std::string file = p->npcRecording;
			if(file.size() < 4 || strcasecmp(file.c_str() + file.size() - 4, ".rec") != 0)
				file += ".rec";
			if(GetWorkspaceDir()[0] && !fileExists(WorkspacePath(("npcmodes/recordings/" + file).c_str())))
				c.add(MAPCHECK_ERROR, NPCS, inst, tr(N_("%s: recording %s isn't in the workspace's npcmodes/recordings.")),
					label(inst, name, sizeof(name)), file.c_str());
		}else if(p->npcPlaysNode){
			char node[64];
			snprintf(node, sizeof(node), "scriptfiles/NPCs/nodes/NODES%d.DAT", p->npcNode);
			if(GetWorkspaceDir()[0] && !fileExists(WorkspacePath(node)))
				c.add(MAPCHECK_INFO, NPCS, inst, tr(N_("%s walks node file %d, which isn't in the workspace (%s). "
					"open.mp downloads it on first use; offline servers need it copied there.")),
					label(inst, name, sizeof(name)), p->npcNode, node);
		}else if(p->pathName[0]){
			usedPaths.insert(p->pathName);
			std::vector<ObjectInst*> points;
			GetNpcPathPoints(p->pathName, points);
			if(points.empty())
				c.add(MAPCHECK_ERROR, NPCS, inst, tr(N_("%s follows path \"%s\", which has no points.")),
					label(inst, name, sizeof(name)), p->pathName);
			else if(points.size() == 1)
				c.add(MAPCHECK_WARNING, NPCS, inst, tr(N_("%s follows path \"%s\", which has one point: it walks there "
					"and stops.")), label(inst, name, sizeof(name)), p->pathName);
		}
	}
	for(auto &seat : seats)
		if(seat.second.size() > 1)
			c.add(MAPCHECK_ERROR, NPCS, ids(seat.second), tr(N_("%d NPCs sit in the same seat (%d) of one vehicle: "
				"they can't both sit there.")), (int)seat.second.size(), seat.first.second);
	for(auto &n : npcNames)
		if(n.second.size() > 1 && n.first[0])
			c.add(MAPCHECK_ERROR, NPCS, ids(n.second), tr(N_("%d NPCs are named \"%s\": names must be unique, so only "
				"one is created.")), (int)n.second.size(), n.first.c_str());
	std::map<std::string, std::vector<ObjectInst*>> unusedPaths;
	for(ObjectInst *inst : byKind[SAMP_ELEMENT_PATHPOINT]){
		const ObjectPawnProperties *p = GetObjectPawnProperties(inst);
		if(usedPaths.find(p->pathName) == usedPaths.end())
			unusedPaths[p->pathName].push_back(inst);
	}
	for(auto &path : unusedPaths)
		c.add(MAPCHECK_INFO, NPCS, ids(path.second), tr(N_("Path \"%s\" (%d point(s)) isn't followed by any NPC.")),
			path.first.empty() ? tr(N_("(no name)")) : path.first.c_str(), (int)path.second.size());
	// open.mp walks a path once; repeating it needs the generated helper hooked up.
	std::vector<ObjectInst*> repeating;
	for(ObjectInst *inst : byKind[SAMP_ELEMENT_NPC])
		if(GetObjectPawnProperties(inst)->npcPathRepeat != NPC_PATH_ONCE)
			repeating.push_back(inst);
	if(!repeating.empty())
		c.add(MAPCHECK_INFO, NPCS, ids(repeating), tr(N_("%d NPC(s) loop or walk back and forth along their path: "
			"call the map's <prefix>_OnNPCFinishMovePath(npcid, pathid) from your gamemode's "
			"OnNPCFinishMovePath, or they stop at the path's end.")), (int)repeating.size());

	// --- Checkpoints (streamer only) ---
	const char *CHECKPOINTS = tr(N_("Checkpoints"));
	std::vector<ObjectInst*> plainCps, raceCps, markers;
	std::map<std::string, std::vector<ObjectInst*>> races;
	std::set<std::string> playable;
	for(ObjectInst *inst : byKind[SAMP_ELEMENT_CHECKPOINT]){
		const ObjectPawnProperties *p = GetObjectPawnProperties(inst);
		if(p->cpKind == SAMP_CHECKPOINT_RACE){
			raceCps.push_back(inst);
			races[p->cpRace].push_back(inst);
			if(p->cpRacePlayable && p->cpRace[0])
				playable.insert(p->cpRace);
		}else
			plainCps.push_back(inst);
		// Playable races go to haumea_races.inc in either format.
		if(p->cpKind != SAMP_CHECKPOINT_RACE || !p->cpRacePlayable || !p->cpRace[0])
			markers.push_back(inst);
	}
	if(!streamer && !markers.empty())
		c.add(MAPCHECK_WARNING, CHECKPOINTS, ids(markers), tr(N_("%d checkpoint(s) are not written "
			"in native code (a player has one checkpoint at a time, set by the script). Save as Streamer code.")),
			(int)markers.size());
	else if(plainCps.size() + raceCps.size() > 1)
		c.add(MAPCHECK_INFO, CHECKPOINTS, std::vector<int>(), tr(N_("%d checkpoint(s) and %d race checkpoint(s): a player "
			"sees one of each at a time, so the streamer shows the nearest.")), (int)plainCps.size(), (int)raceCps.size());
	for(auto &race : races){
		const char *raceName = race.first.empty() ? tr(N_("(no name)")) : race.first.c_str();
		if(race.second.size() == 1)
			c.add(MAPCHECK_WARNING, CHECKPOINTS, ids(race.second), tr(N_("Race \"%s\" has one checkpoint: there is "
				"nothing to race to.")), raceName);
		if(race.second.size() == 1 && GetObjectPawnProperties(race.second[0])->cpRaceMode == SAMP_RACE_CIRCUIT &&
		   playable.count(race.first))
			c.add(MAPCHECK_ERROR, CHECKPOINTS, ids(race.second), tr(N_("Race \"%s\" is a circuit with one checkpoint: "
				"StartMapRace refuses it (a circuit needs at least 2).")), raceName);
		std::map<int, std::vector<ObjectInst*>> orders;
		for(ObjectInst *inst : race.second)
			orders[GetObjectPawnProperties(inst)->cpOrder].push_back(inst);
		for(auto &order : orders)
			if(order.second.size() > 1)
				c.add(MAPCHECK_ERROR, CHECKPOINTS, ids(order.second), tr(N_("Race \"%s\": %d checkpoints share order %d, "
					"so which one comes first is undefined.")), raceName, (int)order.second.size(), order.first);
	}

	if(!playable.empty()){
		std::string a = WorkspacePath("qawno/include/haumea_races.inc"), b = WorkspacePath("pawno/include/haumea_races.inc");
		if(!fileExists(a) && !fileExists(b))
			c.add(MAPCHECK_INFO, CHECKPOINTS, std::vector<int>(), tr(N_("%d playable race(s) need haumea_races.inc, "
				"which isn't in the workspace's includes: without it the map compiles but has no races. A race "
				"checkpoint's settings can save it there.")), (int)playable.size());
	}

	// --- Dynamic areas (streamer only) ---
	const char *AREAS = tr(N_("Dynamic areas"));
	std::vector<ObjectInst*> limited;
	std::map<std::string, int> uses;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted || inst->m_isVehicle || !PawnObjectNeedsExport(inst))
			continue;
		const ObjectPawnProperties *props = GetObjectPawnProperties(inst);
		if(props->areaName[0] && SampElementTakesArea(GetSampElementKind(inst))){
			limited.push_back(inst);
			uses[props->areaName]++;
		}
	}
	if(!streamer){
		if(!byKind[SAMP_ELEMENT_AREA].empty())
			c.add(MAPCHECK_WARNING, AREAS, ids(byKind[SAMP_ELEMENT_AREA]), tr(N_("%d dynamic area(s) are not written in "
				"native code (they are the streamer's). Save as Streamer code.")), (int)byKind[SAMP_ELEMENT_AREA].size());
		if(!limited.empty())
			c.add(MAPCHECK_WARNING, AREAS, ids(limited), tr(N_("%d item(s) are set to show only inside a dynamic area, "
				"but native code shows them everywhere. Save as Streamer code.")), (int)limited.size());
	}else{
		for(ObjectInst *inst : limited){
			const ObjectPawnProperties *props = GetObjectPawnProperties(inst);
			ObjectInst *area = FindAreaByName(props->areaName);
			if(area == nil){
				c.add(MAPCHECK_WARNING, AREAS, inst, tr(N_("%s is set to show only inside area \"%s\", which is not in "
					"the map: it is written without one and shows everywhere.")),
					label(inst, name, sizeof(name)), props->areaName);
				continue;
			}
			const ObjectPawnProperties *ap = GetObjectPawnProperties(area);
			bool world = ap->virtualWorld < 0 || props->virtualWorld < 0 || ap->virtualWorld == props->virtualWorld;
			bool interior = area->m_area <= 0 || inst->m_area <= 0 || area->m_area == inst->m_area;
			if(!world || !interior)
				c.add(MAPCHECK_WARNING, AREAS, { inst->m_id, area->m_id }, tr(N_("%s only shows inside area \"%s\", "
					"but they are in different virtual worlds or interiors: no player is ever in both.")),
					label(inst, name, sizeof(name)), ap->name);
		}
		for(ObjectInst *area : byKind[SAMP_ELEMENT_AREA]){
			const ObjectPawnProperties *ap = GetObjectPawnProperties(area);
			if(ap->areaShape == SAMP_AREA_POLYGON && ap->areaPointCount < 3)
				c.add(MAPCHECK_ERROR, AREAS, area, tr(N_("Polygon area \"%s\" has %d corner(s): it needs 3 or more, "
					"or nobody is ever inside it.")), ap->name, ap->areaPointCount);
			else if(uses.count(ap->name) == 0)
				c.add(MAPCHECK_INFO, AREAS, area, tr(N_("Area \"%s\" has no items limited to it (fine if the script "
					"uses it for something else).")), ap->name);
		}
	}

	// --- TextDraws ---
	const char *TEXTDRAWS = tr(N_("TextDraws"));
	const std::vector<TextDraw> &textDraws = GetTextDraws();
	int globalTds = 0, playerTds = 0;
	for(size_t i = 0; i < textDraws.size(); i++){
		const TextDraw &td = textDraws[i];
		(td.perPlayer ? playerTds : globalTds)++;
		const char *tdName = td.name[0] ? td.name : tr(N_("(unnamed)"));
		MapCheckItem item;
		item.category = TEXTDRAWS;
		item.textDraw = (int)i;
		char text[512];
		auto addTd = [&](int severity, const char *message){
			snprintf(text, sizeof(text), tr(N_("TextDraw %s: %s")), tdName, message);
			item.severity = severity;
			item.text = text;
			result->items.push_back(item);
		};
		if(td.x < 0.0f || td.x > TEXTDRAW_CANVAS_W || td.y < 0.0f || td.y > TEXTDRAW_CANVAS_H)
			addTd(MAPCHECK_WARNING, tr(N_("its position is off the 640x448 canvas, so players won't see it.")));
		if(td.font == TD_FONT_SPRITE){
			if(td.text.empty() || td.text.find(':') == std::string::npos)
				addTd(MAPCHECK_ERROR, tr(N_("a sprite needs its text as TXD:texture.")));
		}else if(td.font != TD_FONT_PREVIEW){
			if(td.text.empty())
				addTd(MAPCHECK_WARNING, tr(N_("empty text can crash 0.3.7 clients; use \"_\" for nothing.")));
			else if(badColourCode(td.text.c_str()))
				addTd(MAPCHECK_INFO, tr(N_("has a '{' that isn't a {RRGGBB} colour code.")));
		}
		if((int)td.text.size() > MAX_TEXTDRAW_STR_LENGTH)
			addTd(MAPCHECK_ERROR, tr(N_("its text is longer than the 800-character limit.")));
	}
	// An old map's own textdraws are created in game too, beside the file's.
	const std::vector<TextDraw> &mapTextDraws = GetMapTextDraws();
	for(size_t i = 0; i < mapTextDraws.size(); i++)
		(mapTextDraws[i].perPlayer ? playerTds : globalTds)++;
	if(!mapTextDraws.empty())
		c.add(MAPCHECK_INFO, TEXTDRAWS, std::vector<int>(), tr(N_("%d textdraw(s) are still in the map, read-only: "
			"TextDraws > Move to a separate file to edit them.")), (int)mapTextDraws.size());
	if(globalTds > GLOBAL_TEXTDRAWS)
		c.add(MAPCHECK_ERROR, TEXTDRAWS, std::vector<int>(), tr(N_("%d global textdraws, over the limit of %d.")),
			globalTds, GLOBAL_TEXTDRAWS);
	if(playerTds > PLAYER_TEXTDRAWS)
		c.add(MAPCHECK_ERROR, TEXTDRAWS, std::vector<int>(), tr(N_("%d player textdraws, over the limit of %d per player.")),
			playerTds, PLAYER_TEXTDRAWS);

	// --- Text encoding ---
	// SA-MP and open.mp clients draw a string's bytes as Windows-1252.
	const char *TEXT = tr(N_("Text"));
	bool cp1252 = GetPawnTextEncoding() == PAWN_TEXT_CP1252;
	std::vector<int> utf8Text;
	// format: the whole sentence, with the element's name and the missing characters.
	auto checkText = [&](ObjectInst *inst, int textDraw, const char *format, const char *who, const char *text, bool shown){
		if(text == nil || IsAscii(text))
			return;
		if(!cp1252){
			if(shown && inst)
				utf8Text.push_back(inst->m_id);
			else if(shown)
				utf8Text.push_back(-1);
			return;
		}
		std::string missing = Cp1252Unencodable(text);
		if(missing.empty())
			return;
		char message[512];
		snprintf(message, sizeof(message), format, inst ? label(inst, name, sizeof(name)) : who, missing.c_str());
		if(inst){
			c.add(shown ? MAPCHECK_WARNING : MAPCHECK_INFO, TEXT, inst, "%s", message);
		}else{
			MapCheckItem item;
			item.category = TEXT;
			item.textDraw = textDraw;
			item.severity = MAPCHECK_WARNING;
			item.text = message;
			result->items.push_back(item);
		}
	};
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted || !PawnObjectNeedsExport(inst))
			continue;
		const ObjectPawnProperties *props = GetObjectPawnProperties(inst);
		if(props->elementKind == SAMP_ELEMENT_LABEL)
			checkText(inst, -1, tr(N_("%s's text has %s, which Windows-1252 (what SA-MP and open.mp clients draw) "
				"doesn't have: it is saved as '?'.")), nil, props->labelText, true);
		for(int slot = 0; slot < MAX_PAWN_MATERIALS; slot++){
			const ObjectMaterialOverride *m = GetObjectMaterialOverride(inst, slot);
			if(m && m->textEnabled)
				checkText(inst, -1, tr(N_("%s's material text has %s, which Windows-1252 (what SA-MP and open.mp clients "
					"draw) doesn't have: it is saved as '?'.")), nil, m->text, true);
		}
		checkText(inst, -1, tr(N_("%s's group has %s, which Windows-1252 (what SA-MP and open.mp clients draw) "
			"doesn't have: it is saved as '?'.")), nil, props->group, false);
		checkText(inst, -1, tr(N_("%s's note has %s, which Windows-1252 (what SA-MP and open.mp clients draw) "
			"doesn't have: it is saved as '?'.")), nil, props->comment, false);
	}
	for(size_t i = 0; i < textDraws.size(); i++){
		checkText(nil, (int)i, tr(N_("TextDraw %s has %s, which Windows-1252 (what SA-MP and open.mp clients draw) "
			"doesn't have: it is saved as '?'.")), textDraws[i].name[0] ? textDraws[i].name : tr(N_("(unnamed)")),
			textDraws[i].text.c_str(), true);
	}
	if(!utf8Text.empty()){
		std::vector<int> insts;
		for(int id : utf8Text)
			if(id >= 0)
				insts.push_back(id);
		c.add(MAPCHECK_WARNING, TEXT, insts, tr(N_("%d text(s) with accents or symbols are saved as UTF-8, but SA-MP "
			"and open.mp clients draw Windows-1252, so they show garbled (\"Ã§\" for \"ç\"). Save As > "
			"Windows-1252 fixes it.")), (int)utf8Text.size());
	}

	// --- Server version ---
	const char *SERVER = tr(N_("Server"));
	int ompOnly = (int)(byKind[SAMP_ELEMENT_NPC].size() + byKind[SAMP_ELEMENT_PATHPOINT].size());
	if(ompOnly > 0)
		c.add(MAPCHECK_WARNING, SERVER, ids(byKind[SAMP_ELEMENT_NPC]), tr(N_("The map has NPCs or NPC paths: their NPC_* "
			"calls only compile and run on open.mp (with its NPCs component), not on a SA-MP 0.3.7 server.")));
	if(!ompZones.empty())
		c.add(MAPCHECK_WARNING, SERVER, ids(ompZones), tr(N_("%d per-player or streamed gang zone(s): "
			"CreatePlayerGangZone only compiles and runs on open.mp, not on a SA-MP 0.3.7 server. For 0.3.7, "
			"set them to Global (Object Info > Shown as, or radar art's Shown as).")), (int)ompZones.size());
	if(!customModels.empty()){
		bool ready = config.found && config.artwork;
		c.add(ready ? MAPCHECK_INFO : MAPCHECK_WARNING, SERVER, ids(customModels), tr(N_("%d element(s) use custom models: "
			"the server needs them in models/artconfig.txt (or AddSimpleModel) and artwork enabled%s; players "
			"need SA-MP 0.3.DL or an open.mp client.%s")), (int)customModels.size(),
			config.found ? (config.artwork ? "" : tr(N_(", but config.json has artwork.enable off"))) : "",
			config.found && config.allow037 ? tr(N_(" 0.3.7 clients are allowed and won't see them.")) : "");
	}

	result->numErrors = result->numWarnings = 0;
	for(const MapCheckItem &item : result->items){
		if(item.severity == MAPCHECK_ERROR) result->numErrors++;
		else if(item.severity == MAPCHECK_WARNING) result->numWarnings++;
	}
}

std::string
MapCheckSummary(const MapCheckResult &result)
{
	char buf[96];
	if(result.numErrors && result.numWarnings)
		snprintf(buf, sizeof(buf), _("Problems: %d, warnings: %d"), result.numErrors, result.numWarnings);
	else if(result.numErrors)
		snprintf(buf, sizeof(buf), _("Problems: %d"), result.numErrors);
	else if(result.numWarnings)
		snprintf(buf, sizeof(buf), _("Warnings: %d"), result.numWarnings);
	else
		buf[0] = '\0';
	return buf;
}
