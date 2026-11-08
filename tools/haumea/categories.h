#pragma once

// The Object Browser's categories (categories.cpp): Haumea's own, filed at
// load from the game's files. CAT(id, name, parent): parents come first.
#define CATEGORY_LIST \
	CAT(CAT_NATURE_ROOT, N_("Nature"), -1) \
	CAT(CAT_TREES, N_("Trees"), CAT_NATURE_ROOT) \
	CAT(CAT_PLANTS, N_("Bushes and plants"), CAT_NATURE_ROOT) \
	CAT(CAT_GRASS, N_("Grass"), CAT_NATURE_ROOT) \
	CAT(CAT_ROCKS, N_("Rocks and cliffs"), CAT_NATURE_ROOT) \
	CAT(CAT_BUILDINGS_ROOT, N_("Buildings and land"), -1) \
	CAT(CAT_HOUSES, N_("Houses"), CAT_BUILDINGS_ROOT) \
	CAT(CAT_SHOPS, N_("Shops, offices and public buildings"), CAT_BUILDINGS_ROOT) \
	CAT(CAT_TOWERS, N_("Towers"), CAT_BUILDINGS_ROOT) \
	CAT(CAT_INDUSTRIAL_BUILDINGS, N_("Sheds, barns and warehouses"), CAT_BUILDINGS_ROOT) \
	CAT(CAT_IN_LS, N_("Los Santos"), CAT_BUILDINGS_ROOT) \
	CAT(CAT_IN_SF, N_("San Fierro"), CAT_BUILDINGS_ROOT) \
	CAT(CAT_IN_LV, N_("Las Venturas"), CAT_BUILDINGS_ROOT) \
	CAT(CAT_IN_COUNTRY, N_("Countryside"), CAT_BUILDINGS_ROOT) \
	CAT(CAT_GROUND, N_("Ground and seabed"), CAT_BUILDINGS_ROOT) \
	CAT(CAT_TRANSPORT_ROOT, N_("Roads, railways and airports"), -1) \
	CAT(CAT_ROADS, N_("Roads"), CAT_TRANSPORT_ROOT) \
	CAT(CAT_BRIDGES, N_("Bridges"), CAT_TRANSPORT_ROOT) \
	CAT(CAT_TUNNELS, N_("Tunnels"), CAT_TRANSPORT_ROOT) \
	CAT(CAT_RAILWAYS, N_("Railways"), CAT_TRANSPORT_ROOT) \
	CAT(CAT_CAR_PARKS, N_("Car parks"), CAT_TRANSPORT_ROOT) \
	CAT(CAT_AIRPORTS, N_("Airports and helipads"), CAT_TRANSPORT_ROOT) \
	CAT(CAT_STREET_ROOT, N_("Street objects"), -1) \
	CAT(CAT_LIGHTS, N_("Lights"), CAT_STREET_ROOT) \
	CAT(CAT_SIGNS, N_("Signs and statues"), CAT_STREET_ROOT) \
	CAT(CAT_FENCES, N_("Fences, walls and barriers"), CAT_STREET_ROOT) \
	CAT(CAT_STREET_FURNITURE, N_("Benches, bins and street furniture"), CAT_STREET_ROOT) \
	CAT(CAT_INDUSTRY, N_("Industry and construction"), -1) \
	CAT(CAT_WATER, N_("Water and coast"), -1) \
	CAT(CAT_PROPS_ROOT, N_("Props"), -1) \
	CAT(CAT_BOXES, N_("Boxes and crates"), CAT_PROPS_ROOT) \
	CAT(CAT_BARRELS, N_("Barrels and containers"), CAT_PROPS_ROOT) \
	CAT(CAT_FOOD, N_("Food and drink"), CAT_PROPS_ROOT) \
	CAT(CAT_WEAPON_PROPS, N_("Weapons"), CAT_PROPS_ROOT) \
	CAT(CAT_SPORTS, N_("Sports and play"), CAT_PROPS_ROOT) \
	CAT(CAT_OTHER_PROPS, N_("Other props"), CAT_PROPS_ROOT) \
	CAT(CAT_INTERIORS_ROOT, N_("Interiors"), -1) \
	CAT(CAT_FURNITURE, N_("Furniture"), CAT_INTERIORS_ROOT) \
	CAT(CAT_KITCHEN, N_("Kitchens and bathrooms"), CAT_INTERIORS_ROOT) \
	CAT(CAT_SHOP_FITTINGS, N_("Shop, bar and casino fittings"), CAT_INTERIORS_ROOT) \
	CAT(CAT_DOORS, N_("Doors and windows"), CAT_INTERIORS_ROOT) \
	CAT(CAT_OTHER_INTERIOR, N_("Rooms and other interior objects"), CAT_INTERIORS_ROOT) \
	CAT(CAT_VEHICLES_ROOT, N_("Vehicles"), -1) \
	CAT(CAT_CARS, N_("Cars"), CAT_VEHICLES_ROOT) \
	CAT(CAT_WORK_VEHICLES, N_("Vans, trucks and work vehicles"), CAT_VEHICLES_ROOT) \
	CAT(CAT_EMERGENCY, N_("Police, ambulance and fire"), CAT_VEHICLES_ROOT) \
	CAT(CAT_MOTORBIKES, N_("Motorbikes and quads"), CAT_VEHICLES_ROOT) \
	CAT(CAT_BICYCLES, N_("Bicycles"), CAT_VEHICLES_ROOT) \
	CAT(CAT_BOATS, N_("Boats"), CAT_VEHICLES_ROOT) \
	CAT(CAT_PLANES, N_("Planes"), CAT_VEHICLES_ROOT) \
	CAT(CAT_HELICOPTERS, N_("Helicopters"), CAT_VEHICLES_ROOT) \
	CAT(CAT_TRAINS, N_("Trains and trams"), CAT_VEHICLES_ROOT) \
	CAT(CAT_TRAILERS, N_("Trailers"), CAT_VEHICLES_ROOT) \
	CAT(CAT_RC, N_("Remote control"), CAT_VEHICLES_ROOT) \
	CAT(CAT_VEHICLE_PARTS, N_("Vehicle parts"), -1) \
	CAT(CAT_PEOPLE, N_("People (skins)"), -1) \
	CAT(CAT_NIGHT, N_("Night-time objects"), -1) \
	CAT(CAT_LOD, N_("Far versions (LOD)"), -1) \
	CAT(CAT_SAMP_ROOT, N_("SA-MP objects"), -1) \
	CAT(CAT_SAMP_BLOCKS, N_("Building blocks"), CAT_SAMP_ROOT) \
	CAT(CAT_SAMP_TEXT, N_("Text and materials"), CAT_SAMP_ROOT) \
	CAT(CAT_SAMP_LIGHTS, N_("Lights"), CAT_SAMP_ROOT) \
	CAT(CAT_SAMP_EFFECTS, N_("Effects and particles"), CAT_SAMP_ROOT) \
	CAT(CAT_SAMP_WEARABLES, N_("Wearable items"), CAT_SAMP_ROOT) \
	CAT(CAT_SAMP_MARKERS, N_("Markers and boundaries"), CAT_SAMP_ROOT) \
	CAT(CAT_SAMP_NATURE, N_("Nature"), CAT_SAMP_ROOT) \
	CAT(CAT_SAMP_ROADS, N_("Roads and railways"), CAT_SAMP_ROOT) \
	CAT(CAT_SAMP_BUILDINGS, N_("Buildings"), CAT_SAMP_ROOT) \
	CAT(CAT_SAMP_STREET, N_("Street objects"), CAT_SAMP_ROOT) \
	CAT(CAT_SAMP_INTERIORS, N_("Interiors"), CAT_SAMP_ROOT) \
	CAT(CAT_SAMP_PROPS, N_("Props"), CAT_SAMP_ROOT) \
	CAT(CAT_SAMP_OTHER, N_("Other SA-MP objects"), CAT_SAMP_ROOT) \
	CAT(CAT_CUSTOM, N_("Custom models"), -1) \
	CAT(CAT_UNSORTED, N_("Not sorted yet"), -1)

enum ObjectCategoryId {
#define CAT(id, name, parent) id,
	CATEGORY_LIST
#undef CAT
};

struct ObjCategory {
	const char *name;
	int parent;	// -1 for top-level
};
extern const ObjCategory objCategories[];
extern const int NUM_OBJ_CATEGORIES;

#define SAMP_CATEGORY_ROOT CAT_SAMP_ROOT
#define SAMP_CATEGORY_UNCAT CAT_SAMP_OTHER
#define CUSTOM_MODELS_CATEGORY CAT_CUSTOM

// The category rules for one model (SA-MP's models get their own branch).
int ClassifyObjectCategory(int id, bool samp);
// --dump-categories: id, name, TXD, IDE file and category path of every model
// (tab-separated); returns how many.
int DumpObjectCategories(const char *path);
