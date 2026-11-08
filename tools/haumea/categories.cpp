#include "haumea.h"
#include "categories.h"
#include "i18n.h"	// N_() in CATEGORY_LIST

#include <string.h>
#include <ctype.h>
#include <string>

// The Object Browser's categories: Haumea's own. Each model is filed at load
// from what the game's own files say about it: its name and TXD, the IDE
// file and folder it comes from, its flags (trees, night-time objects), its
// collision size, the vehicle type and class in vehicles.ide, and whether it
// is a skin or one of SA-MP's models. Rules below run in order and the first
// that matches wins; overrides at the end fix the few models the rules get
// wrong. Names are plain English, to be translated later.

const ObjCategory objCategories[] = {
#define CAT(id, name, parent) { name, parent },
	CATEGORY_LIST
#undef CAT
};
const int NUM_OBJ_CATEGORIES = (int)(sizeof(objCategories) / sizeof(objCategories[0]));

namespace {

struct KeywordRule {
	int category;
	const char *words[32];	// matched anywhere in the lower-case model name (and TXD name if 4+ letters)
};

// Our own keyword lists. Order matters: specific things before general ones.
const KeywordRule rules[] = {
	{ CAT_TREES, { "tree", "palm", "pine", "oak", "cedar", "birch", "elm", "cypress", "sequoia", "trunk" } },
	{ CAT_PLANTS, { "bush", "shrub", "hedge", "plant", "cactus", "fern", "flower", "weed", "reed", "leaf",
	                "leaves", "veg_", "ivy", "vine", "planter" } },
	{ CAT_GRASS, { "grass", "hay", "straw", "lawn" } },
	{ CAT_ROCKS, { "rock", "boulder", "cliff", "stone", "crag", "scree", "quarry", "volcano" } },
	{ CAT_AIRPORTS, { "airport", "runway", "helipad", "hangar", "hanger", "taxiway", "airstrip" } },
	{ CAT_RAILWAYS, { "rail", "track", "train", "tram", "station", "platform", "sleeper" } },
	{ CAT_BRIDGES, { "bridge", "brdg", "flyover", "overpass", "viaduct" } },
	{ CAT_TUNNELS, { "tunnel", "tunl", "underpass" } },
	{ CAT_ROADS, { "road", "street", "junc", "jct", "motorway", "freeway", "hiway", "highway",
	               "roundabout", "crossing", "xing", "kerb", "curb", "pave", "sidewalk" } },
	{ CAT_CAR_PARKS, { "carpark", "car_park", "parking", "cpark" } },
	{ CAT_WATER, { "seaside", "seawall", "beach", "dock", "pier", "jetty", "harbour", "harbor", "wharf",
	               "boat", "yacht", "buoy", "lighthouse", "canal", "shore", "coast", "marina", "pontoon" } },
	{ CAT_LIGHTS, { "light", "lamp", "lite", "lantern", "neon", "spotlt", "streetlt" } },
	{ CAT_SIGNS, { "sign", "billboard", "bboard", "poster", "advert", "logo", "statue", "monument",
	               "graffiti", "spraytag" } },
	{ CAT_FENCES, { "fence", "wall", "gate", "barrier", "railing", "bollard", "cone" } },
	{ CAT_STREET_FURNITURE, { "bench", "seat", "bin", "trash", "dumpster", "litter", "rubbish", "garbage",
	                          "mailbox", "postbox", "phone", "hydrant", "busstop", "bus_stop", "shelter",
	                          "vend", "parkmeter", "atm" } },
	{ CAT_INDUSTRY, { "crane", "scaff", "construct", "girder", "pipe", "tank", "silo", "chimney", "refinery",
	                  "generator", "pylon", "transformer", "cable", "wire", "telegraph", "ladder", "stair",
	                  "ramp" } },
	{ CAT_BOXES, { "crate", "box", "pallet", "carton", "parcel", "package" } },
	{ CAT_BARRELS, { "barrel", "drum", "container", "keg", "canister", "cylinder" } },
	{ CAT_FOOD, { "food", "drink", "bottle", "cup", "burger", "pizza", "donut", "cola", "beer", "wine",
	              "plate", "hugebowl", "saucepan", "kettle", "coffeemach" } },
	{ CAT_WEAPON_PROPS, { "gun", "rifle", "pistol", "grenade", "rocket", "missile", "bomb", "knife",
	                      "sword", "katana", "ammo", "weapon" } },
	{ CAT_SPORTS, { "ball", "hoop", "basket", "goal", "pool", "golf", "tennis", "skate", "swing", "slide",
	                "playground", "gym", "kart" } },
	{ CAT_FURNITURE, { "chair", "table", "desk", "sofa", "couch", "bed", "shelf", "shelves", "cabinet",
	                   "cupboard", "wardrobe", "drawer", "stool", "dresser", "bookcase", "telly" } },
	{ CAT_KITCHEN, { "kitchen", "cooker", "oven", "fridge", "freezer", "sink", "toilet", "bath", "shower",
	                 "basin", "washer", "microwave", "worktop" } },
	{ CAT_OTHER_INTERIOR, { "outlet", "socket", "lightswitch", "radiator", "carpet" } },
	{ CAT_SHOP_FITTINGS, { "counter", "till", "shop", "rack", "display", "fruit", "slot", "casino",
	                       "roulette", "blackjack", "poker", "jukebox" } },
	{ CAT_DOORS, { "door", "window", "shutter", "blind", "curtain" } },
	{ CAT_HOUSES, { "house", "hse", "hous", "home", "apart", "villa", "mansion", "shack", "cabin", "hut",
	                "caravan", "bungalow", "tenement" } },
	{ CAT_SHOPS, { "store", "market", "mall", "restaurant", "diner", "cafe", "club", "hotel", "motel",
	               "bank", "cinema", "petrol", "garage", "gasstat", "carwash", "office", "hosp",
	               "police", "church", "school", "stadium" } },
	{ CAT_TOWERS, { "tower", "twr", "skyscr", "highrise" } },
	{ CAT_INDUSTRIAL_BUILDINGS, { "warehouse", "wrhse", "factory", "shed", "depot", "works", "mill", "barn",
	                              "farm" } },
};

// SA-MP's own objects: things only SA-MP has, before the rules above.
const KeywordRule sampRules[] = {
	{ CAT_FURNITURE, { "fireplace", "firewood" } },
	{ CAT_SAMP_PROPS, { "fireext", "extinguisher" } },
	{ CAT_SAMP_WEARABLES, { "hat", "cap", "mask", "glasses", "bandana", "helmet", "beret", "watch",
	                        "headphone", "hair", "beard", "moustache", "headband", "hood", "earring",
	                        "necklace", "backpack", "parachute", "handcuff", "laserpointer", "bandage",
	                        "armour", "vest" } },
	{ CAT_SAMP_MARKERS, { "marker", "boundary", "area_", "checkpoint", "arrow", "icon" } },
	{ CAT_SAMP_EFFECTS, { "particle", "fire", "smoke", "explosion", "spark", "flare", "fountain", "steam",
	                      "water_", "blood", "prt_", "exhale", "flame", "overheat", "fakesnow", "puke" } },
	{ CAT_SAMP_TEXT, { "mattext", "matbox", "text", "letter", "number", "font", "gtasamap" } },
	{ CAT_SAMP_BLOCKS, { "cube", "block", "wall0", "floor", "tile", "plane", "plate", "tube", "ramp",
	                     "stunt", "loop", "mega", "concrete", "beam", "panel", "slab", "pyramid", "sphere",
	                     "cylinder", "cone", "base1", "base2", "base5", "wsbend", "wsstraight" } },
	{ CAT_SAMP_LIGHTS, { "light", "lamp", "neon", "spot", "glow" } },
};

// Corrections for models the rules can't place, by exact name.
struct Override { const char *name; int category; };
const Override overrides[] = {
	{ "playroom", CAT_OTHER_INTERIOR },
	{ "comms01", CAT_INDUSTRY },
	{ "comms02", CAT_INDUSTRY },
};

int lookup[NUMOBJECTDEFS];

bool contains(const std::string &hay, const char *needle) { return hay.find(needle) != std::string::npos; }

std::string lower(const char *s)
{
	std::string out = s ? s : "";
	for(char &c : out) c = (char)tolower((unsigned char)c);
	return out;
}

int matchRules(const KeywordRule *list, int n, const std::string &name, const std::string &txd, bool useTxd = true)
{
	for(int r = 0; r < n; r++)
		for(int w = 0; w < 24 && list[r].words[w]; w++)
			if(contains(name, list[r].words[w]))
				return list[r].category;
	if(!useTxd)
		return -1;
	for(int r = 0; r < n; r++)
		for(int w = 0; w < 24 && list[r].words[w]; w++)
			if(strlen(list[r].words[w]) >= 4 && contains(txd, list[r].words[w]))
				return list[r].category;
	return -1;
}

int vehicleCategory(const ObjectDef *obj)
{
	std::string type = lower(obj->m_vehicleType), cls = lower(obj->m_vehicleClass), name = lower(obj->m_name);
	static const char *emergency[] = { "police", "cop", "ambulan", "firetruk", "firela", "fbi", "swat",
	                                   "enforcer", "barracks", "rhino", "patriot", nil };
	for(int i = 0; emergency[i]; i++)
		if(contains(name, emergency[i]))
			return CAT_EMERGENCY;
	if(contains(name, "rc") && (type == "plane" || type == "heli" || type == "car" || type == "mtruck"))
		if(name.compare(0, 2, "rc") == 0)
			return CAT_RC;
	if(type == "bike" || type == "quad") return CAT_MOTORBIKES;
	if(type == "bmx") return CAT_BICYCLES;
	if(type == "boat") return CAT_BOATS;
	if(type == "plane") return CAT_PLANES;
	if(type == "heli") return CAT_HELICOPTERS;
	if(type == "train") return CAT_TRAINS;
	if(type == "trailer") return CAT_TRAILERS;
	if(cls == "worker" || cls == "big" || type == "mtruck") return CAT_WORK_VEHICLES;
	return CAT_CARS;
}

// The folder and file an IDE came from, lower case: "la", "lan.ide".
void ideOf(const ObjectDef *obj, std::string &folder, std::string &file)
{
	std::string path = lower(obj->m_file && obj->m_file->name ? obj->m_file->name : "");
	for(char &c : path) if(c == '\\') c = '/';
	size_t slash = path.rfind('/');
	file = slash == std::string::npos ? path : path.substr(slash + 1);
	folder.clear();
	if(slash != std::string::npos){
		size_t prev = path.rfind('/', slash - 1);
		folder = path.substr(prev == std::string::npos ? 0 : prev + 1, slash - (prev == std::string::npos ? 0 : prev + 1));
	}
}

// How big its collision is: the longest side and the height, in metres (0 without).
void sizeOf(const ObjectDef *obj, float *longest, float *height)
{
	*longest = *height = 0.0f;
	if(obj->m_colModel == nil)
		return;
	const CBox &box = obj->m_colModel->boundingBox;
	float dx = box.max.x - box.min.x, dy = box.max.y - box.min.y;
	*longest = dx > dy ? dx : dy;
	*height = box.max.z - box.min.z;
}

// SA-MP's objects get their own branch, filed by what kind of thing they are.
int sampBranch(int category)
{
	if(category < 0) return CAT_SAMP_OTHER;
	int top = category;
	while(objCategories[top].parent >= 0) top = objCategories[top].parent;
	switch(top){
	case CAT_NATURE_ROOT: return CAT_SAMP_NATURE;
	case CAT_TRANSPORT_ROOT: return CAT_SAMP_ROADS;
	case CAT_BUILDINGS_ROOT: return CAT_SAMP_BUILDINGS;
	case CAT_STREET_ROOT: return category == CAT_LIGHTS ? CAT_SAMP_LIGHTS : CAT_SAMP_STREET;
	case CAT_INTERIORS_ROOT: return CAT_SAMP_INTERIORS;
	case CAT_PROPS_ROOT: case CAT_INDUSTRY: case CAT_WATER: return CAT_SAMP_PROPS;
	}
	return CAT_SAMP_OTHER;
}

}	// namespace

int
ClassifyObjectCategory(int id, bool samp)
{
	ObjectDef *obj = GetObjectDef(id);
	if(obj == nil)
		return -1;
	if(obj->m_isVehicle) return vehicleCategory(obj);
	if(obj->m_isPed) return CAT_PEOPLE;
	if(samp && id>=18668 && id<=18748) return CAT_SAMP_EFFECTS;
	std::string name = lower(obj->m_name), txd, folder, file;
	TxdDef *td = GetTxdDef(obj->m_txdSlot);
	txd = lower(td ? td->name : "");
	ideOf(obj, folder, file);
	for(const Override &o : overrides)
		if(name == o.name)
			return o.category;
	int byName = matchRules(rules, (int)(sizeof(rules)/sizeof(rules[0])), name, txd);
	if(samp){
		// SA-MP's own kinds by model name only: its TXDs are shared too widely.
		int own = matchRules(sampRules, (int)(sizeof(sampRules)/sizeof(sampRules[0])), name, txd, false);
		if(own == CAT_FURNITURE) return CAT_SAMP_INTERIORS;
		return own >= 0 ? own : sampBranch(byName);
	}
	if(file == "veh_mods.ide") return CAT_VEHICLE_PARTS;
	if(name.compare(0, 3, "lod") == 0 || txd.compare(0, 3, "lod") == 0) return CAT_LOD;
	if(obj->m_isTimed) return CAT_NIGHT;
	if(obj->m_isTree || obj->m_isPalmTree) return CAT_TREES;
	if(file == "vegepart.ide" || file == "procobj.ide"){
		if(byName == CAT_TREES || byName == CAT_PLANTS || byName == CAT_GRASS || byName == CAT_ROCKS)
			return byName;
		return CAT_PLANTS;
	}
	if(file == "seabed.ide") return CAT_GROUND;
	if(folder == "interior" || file.compare(0, 5, "props") == 0 || file == "propext.ide"){
		int top = byName;
		while(top >= 0 && objCategories[top].parent >= 0) top = objCategories[top].parent;
		return top == CAT_INTERIORS_ROOT ? byName : CAT_OTHER_INTERIOR;
	}
	if(byName >= 0)
		return byName;
	float longest, height;
	sizeOf(obj, &longest, &height);
	int city = folder == "la" ? CAT_IN_LS : folder == "sf" ? CAT_IN_SF : folder == "vegas" ? CAT_IN_LV :
		folder == "country" ? CAT_IN_COUNTRY : -1;
	if(city >= 0)
		// Big pieces of a city are its buildings and land; small ones are props.
		return longest >= 15.0f || height >= 8.0f ? city : CAT_OTHER_PROPS;
	if(file == "dynamic.ide" || file == "dynamic2.ide" || file == "multiobj.ide" || file == "barriers.ide")
		return CAT_OTHER_PROPS;
	if(folder == "leveldes") return longest >= 15.0f ? CAT_GROUND : CAT_OTHER_PROPS;
	return CAT_UNSORTED;
}

void
InitObjectCategories(void)
{
	for(int i = 0; i < NUMOBJECTDEFS; i++)
		lookup[i] = ClassifyObjectCategory(i, false);
}

int
GetObjectCategory(int modelId)
{
	if(modelId < 0 || modelId >= NUMOBJECTDEFS) return -1;
	return lookup[modelId];
}

void
SetObjectCategory(int modelId, int category)
{
	if(modelId >= 0 && modelId < NUMOBJECTDEFS)
		lookup[modelId] = category;
}

int
DumpObjectCategories(const char *path)
{
	FILE *f = fopen(path, "w");
	if(f == nil)
		return -1;
	int n = 0;
	for(int i = 0; i < NUMOBJECTDEFS; i++){
		ObjectDef *obj = GetObjectDef(i);
		if(obj == nil)
			continue;
		int cat = lookup[i];
		std::string where = cat >= 0 ? objCategories[cat].name : "-";
		for(int p = cat >= 0 ? objCategories[cat].parent : -1; p >= 0; p = objCategories[p].parent)
			where = std::string(objCategories[p].name) + " > " + where;
		TxdDef *td = GetTxdDef(obj->m_txdSlot);
		fprintf(f, "%d\t%s\t%s\t%s\t%s\n", i, obj->m_name, td ? td->name : "",
			obj->m_file && obj->m_file->name ? obj->m_file->name : "", where.c_str());
		n++;
	}
	fclose(f);
	return n;
}
