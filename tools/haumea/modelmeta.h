#pragma once

#include <string>
#include <vector>

// What each model is, for the Object Browser's filters and the agent's index:
// what the game's files say about it (collision size, flags, draw distance,
// where it comes from), where the world places it (city and zone), and,
// read a few hundred models a frame once a filter or sort asks for them, its
// polygons, materials and texture names.

enum ModelSource { MODEL_SOURCE_GTA, MODEL_SOURCE_SAMP, MODEL_SOURCE_CUSTOM, NUM_MODEL_SOURCES };
enum ModelCity { CITY_LOS_SANTOS, CITY_SAN_FIERRO, CITY_LAS_VENTURAS, CITY_COUNTRYSIDE, CITY_INTERIORS,
	NUM_MODEL_CITIES };

struct ModelMeta
{
	bool valid;
	float width, depth, height;	// the collision box in metres (X, Y, Z); 0 without collision
	bool hasCollision;
	bool twoSided;	// drawn without backface culling
	bool isLod, hasLod;	// a far version of another model / has one
	float drawDistance;
	int source;	// ModelSource
	bool dynamic;	// in object.dat: it moves when hit
	bool explosive;	// object.dat's "causes explosion": it blows up when shot or hit
	bool breakable, timed, animated, upgrade, vehicle, skin;
	bool interior;	// defined in an interior IDE
	bool building;	// filed under Buildings and land, and stays put
	bool alpha;	// drawn last or additive, or without z writes
	int timeOn, timeOff;
	const char *ideFile;	// as the game file list names it, or ""
	const char *txdName;
	int placements;	// how often the world (IPL) places it
	unsigned cities;	// 1 << ModelCity for each city it is placed in
	std::vector<int> zones;	// GetMetaZoneName indices it is placed in
	// From the model's own files, once ModelMetaDetailsProgress() is 1: -1 until then.
	int polygons, materials;
};

// After the world and SA-MP's models are loaded, and again if they change.
void BuildModelMeta(void);
const ModelMeta *GetModelMeta(int id);	// nil for no model

// Polygons, materials and textures: started on first need, then read a few
// hundred models per ModelMetaTick (once a frame).
void RequestModelMetaDetails(void);
void ModelMetaTick(void);
float ModelMetaDetailsProgress(void);	// 0 to 1; 1 when everything is read
void ReadAllModelMetaDetails(void);	// at once (--dump-model-meta)
// A texture of the model's TXD (or its parents) whose name contains text, any case.
bool ModelHasTextureContaining(int id, const char *text);

// Zones where the world places models: names from the game text, each in a city.
int NumMetaZones(void);
const char *GetMetaZoneName(int zone);
int GetMetaZoneCity(int zone);
const char *GetModelCityName(int city);
const char *GetModelSourceName(int source);

// The open map's own objects, and every Pawn map in the workspace folder.
bool ModelInOpenMap(int id);
bool ModelInWorkspace(int id);

// --dump-model-meta: one tab-separated line per model, for the agent's index.
int DumpModelMeta(const char *path);
