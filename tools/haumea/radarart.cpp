// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "i18n.h"
#include "radarart.h"
#include "icons.h"

#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// Radar art: the objects seen from above, as gang zones. Each object's
// mesh (or its collision: triangles, boxes, spheres; its bounding box
// without any) is rasterised into a grid of square cells, keeping the highest surface in
// each cell and the object it belongs to. The cells get their colour, and
// runs of one colour are merged greedily into rectangles, one gang zone each.

enum {
	MAX_RADAR_CELLS = 4000000,	// 2000 x 2000
	MAX_RADAR_ZONES = 20000,
};

void
RadarArtDefaults(RadarArtParams *params)
{
	memset(params, 0, sizeof(*params));
	params->source = RADAR_SOURCE_ALL;
	params->cellSize = 4.0f;
	params->shape = RADAR_SHAPE_MESH;
	params->aboveWater = true;
	params->colourMode = RADAR_COLOUR_RADAR;
	params->colour = 0x8C8C8C00;
	params->numColours = 6;
	params->alpha = 200;
	params->outline = false;
	params->outlineColour = 0x20202000;
	params->heightShading = true;
	params->heightLevels = 3;
	params->zoneMode = SAMP_ZONE_GLOBAL;
	strcpy(params->outGroup, "radar art");
	params->replace = true;
}

// ---- sources ----------------------------------------------------------------

static bool
isSourceObject(ObjectInst *inst)
{
	return inst && !inst->m_isDeleted && !inst->m_isVehicle && !IsPawnWorldObject(inst) &&
		GetSampElementKind(inst) == SAMP_ELEMENT_OBJECT && GetObjectDef(inst->m_objectId);
}

static void
collectSources(const RadarArtParams &params, std::vector<ObjectInst*> &out)
{
	CPtrList &list = params.source == RADAR_SOURCE_SELECTION ? selection : instances;
	for(CPtrNode *p = list.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!isSourceObject(inst))
			continue;
		if(params.source == RADAR_SOURCE_GROUP && strcmp(GetObjectPawnProperties(inst)->group, params.group) != 0)
			continue;
		out.push_back(inst);
	}
	// Creation order, so the same scene gives the same art.
	std::sort(out.begin(), out.end(), [](const ObjectInst *a, const ObjectInst *b){ return a->m_id < b->m_id; });
}

// ---- footprints ---------------------------------------------------------------

// What the game's radar draws a surface as, and its colour there: black
// roads, white buildings, grey paved ground, dark red railways (LongWayDrivers'
// minimap terrains, picked from the radar tiles).
enum RadarClass : uint8 {
	TERRAIN_NONE, TERRAIN_WATER, TERRAIN_ROAD, TERRAIN_RAIL, TERRAIN_SAND, TERRAIN_DIRT,
	TERRAIN_DESERT, TERRAIN_ROCK, TERRAIN_GRASS, TERRAIN_FIELD,
	TERRAIN_FOREST, TERRAIN_PAVED, TERRAIN_BUILDING, TERRAIN_SNOW, NUM_RADAR_CLASSES,
	TERRAIN_HIDDEN = 0xFF,	// an invisible material: not drawn, so not on the radar
};
static const uint32 radarPalette[NUM_RADAR_CLASSES] = {
	0x9F9C9F,	// none: as paved
	0x728AAD,	// water
	0x020000,	// road
	0x3A1111,	// rail
	0xE6C164,	// sand
	0x746553,	// dirt
	0x9C8672,	// desert
	0x736651,	// rock (dark desert)
	0x7B8A3A,	// grass
	0x7B8A3A,	// field
	0x3A6929,	// forest (dark grass)
	0x9F9C9F,	// paved
	0xFEFBFF,	// building
	0xE4E8EC,	// snow: whiter than the ground, greyer than the buildings
};

// eSurfaceType (gta-reversed): the collision's material.
static uint8
surfaceClass(int surface)
{
	switch(surface){
	case 1: case 2: case 3: return TERRAIN_ROAD;
	case 4: case 5: case 7: case 8: case 34: case 85: case 89: case 134: case 135: case 136: case 137:
	case 138: case 144: case 161: case 165: return TERRAIN_PAVED;
	case 9: case 10: case 11: case 15: case 16: case 17: case 19: case 20: case 80: case 81: case 87:
	case 115: case 117: case 118: case 120: case 133: case 146: case 147: case 148: case 149: case 150:
	case 151: case 152: case 160: return TERRAIN_GRASS;
	case 12: case 13: case 14: case 21: case 40: case 82: case 116: case 119: case 125: case 145:
	case 153: case 176: return TERRAIN_FIELD;
	case 22: case 23: case 41: case 83: case 84: case 111: case 112: case 113: case 114: case 122:
	case 128: case 129: case 130: case 132: case 143: return TERRAIN_FOREST;
	case 6: case 24: case 25: case 26: case 27: case 88: case 101: case 123: case 124:
	case 139: case 140: case 141: case 142: return TERRAIN_DIRT;
	case 86: return TERRAIN_DESERT;
	case 28: case 29: case 30: case 31: case 32: case 33: case 74: case 75: case 76: case 77: case 78:
	case 79: return TERRAIN_SAND;
	case 18: case 35: case 36: case 37: case 109: case 121: case 131: case 154: return TERRAIN_ROCK;
	case 38: case 39: case 96: case 97: case 98: case 99: case 100: case 110: case 126: case 127:
	case 155: case 156: case 157: return TERRAIN_WATER;
	case 178: return TERRAIN_RAIL;
	}
	return TERRAIN_NONE;
}

// A material by its model, TXD and texture names, as LongWayDrivers'
// minimap_autogen.py does it: the first group with a word in them.
static uint8
nameClass(const char *model, const char *txd, const char *texture)
{
	static const struct { uint8 cls; const char *words[12]; } groups[] = {
		{ TERRAIN_HIDDEN, { "invisible", "nocol", "transparent" } },
		{ TERRAIN_WATER, { "water", "river", "ocean", "canal", "pool", "lake", "sea_" } },
		{ TERRAIN_RAIL, { "rail", "traintrack", "tracks" } },
		{ TERRAIN_ROAD, { "road", "asphalt", "tarmac", "freeway", "highway", "hiway", "street", "junction",
			"runway", "motorway" } },
		{ TERRAIN_SNOW, { "snow", "ice_", "frost" } },
		{ TERRAIN_SAND, { "sand", "beach" } },
		{ TERRAIN_ROCK, { "rock", "cliff", "stone", "boulder", "mount" } },
		// The radar draws no trees or bushes, only the ground under them.
		{ TERRAIN_HIDDEN, { "tree", "pine", "leaf", "leaves", "bush", "hedge", "foliage", "palm", "branch", "shrub", "bark", "trunk" } },
		{ TERRAIN_FOREST, { "forest", "woodland" } },
		{ TERRAIN_GRASS, { "grass", "lawn", "meadow", "turf" } },
		{ TERRAIN_FIELD, { "field", "crop", "hay", "corn" } },
		{ TERRAIN_DESERT, { "desert", "des_" } },
		{ TERRAIN_DIRT, { "dirt", "mud", "gravel", "soil" } },
		{ TERRAIN_PAVED, { "concrete", "conc", "pavement", "pave", "cement", "slab", "carpark", "parking",
			"plaza", "sidewalk", "floor" } },
		{ TERRAIN_BUILDING, { "roof", "shingle", "brick", "window", "wall" } },
	};
	char text[192];
	snprintf(text, sizeof(text), "%s %s %s", model ? model : "", txd ? txd : "", texture ? texture : "");
	for(char *p = text; *p; p++)
		*p = (char)tolower((unsigned char)*p);
	for(const auto &g : groups)
		for(const char *w : g.words)
			if(w && strstr(text, w))
				return g.cls;
	return TERRAIN_NONE;
}

struct Tri {
	rw::V3d v[3];
	uint8 cls = TERRAIN_NONE;
	uint8 mat = 0xFF;	// the geometry's material: the Pawn material slot
};

static void
addBox(std::vector<Tri> &tris, const rw::V3d &mn, const rw::V3d &mx, uint8 cls = TERRAIN_NONE)
{
	rw::V3d c[8];
	for(int i = 0; i < 8; i++)
		c[i] = { i & 1 ? mx.x : mn.x, i & 2 ? mx.y : mn.y, i & 4 ? mx.z : mn.z };
	static const int faces[6][4] = {
		{ 0, 1, 3, 2 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 3, 7, 5 },
	};
	for(int f = 0; f < 6; f++){
		tris.push_back({ { c[faces[f][0]], c[faces[f][1]], c[faces[f][2]] }, cls });
		tris.push_back({ { c[faces[f][0]], c[faces[f][2]], c[faces[f][3]] }, cls });
	}
}

static void
addGeometry(std::vector<Tri> &tris, rw::Geometry *geo, const rw::Matrix *m, ObjectDef *obj)
{
	if(geo == nil || geo->morphTargets == nil || geo->morphTargets[0].vertices == nil || geo->triangles == nil)
		return;
	const TxdDef *txd = GetTxdDef(obj->m_txdSlot);
	std::vector<uint8> matClass(geo->matList.numMaterials, TERRAIN_NONE);
	for(int k = 0; k < geo->matList.numMaterials; k++){
		rw::Material *mat = geo->matList.materials[k];
		matClass[k] = nameClass(obj->m_name, txd ? txd->name : nil, mat && mat->texture ? mat->texture->name : nil);
	}
	const rw::V3d *verts = geo->morphTargets[0].vertices;
	for(int i = 0; i < geo->numTriangles; i++){
		Tri tri;
		int mat = geo->triangles[i].matId;
		tri.cls = mat < (int)matClass.size() ? matClass[mat] : TERRAIN_NONE;
		tri.mat = (uint8)std::min(mat, 0xFF);
		for(int k = 0; k < 3; k++){
			const rw::V3d &v = verts[geo->triangles[i].v[k]];
			if(m)
				rw::V3d::transformPoints(&tri.v[k], &v, 1, m);
			else
				tri.v[k] = v;
		}
		tris.push_back(tri);
	}
}

// The model's triangles as drawn: its most detailed atomic, or every atomic
// of a clump where its frames put them.
static void
addMesh(std::vector<Tri> &tris, ObjectDef *obj)
{
	if(obj == nil)
		return;
	if(!obj->IsLoaded() && !obj->m_cantLoad)
		obj->Load();
	if(obj->m_type == ObjectDef::ATOMIC && obj->m_atomics[0])
		addGeometry(tris, obj->m_atomics[0]->geometry, nil, obj);
	else if(obj->m_type == ObjectDef::CLUMP && obj->m_clump){
		rw::Frame *root = obj->m_clump->getFrame();
		rw::Matrix inv;
		if(root)
			rw::Matrix::invert(&inv, root->getLTM());
		FORLIST(lnk, obj->m_clump->atomics){
			rw::Atomic *atomic = rw::Atomic::fromClump(lnk);
			rw::Frame *frame = atomic->getFrame();
			if(frame == nil){
				addGeometry(tris, atomic->geometry, nil, obj);
				continue;
			}
			// The atomic's frame relative to the clump's.
			rw::Matrix m = *frame->getLTM();
			if(root){
				rw::Matrix ltm = m;
				rw::Matrix::mult(&m, &ltm, &inv);
			}
			addGeometry(tris, atomic->geometry, &m, obj);
		}
	}
}

// What most of a model's collision is, by area seen from above: for the
// mesh triangles whose names say nothing.
static uint8
collisionClass(ObjectDef *obj)
{
	CColModel *col = obj ? obj->m_colModel : nil;
	if(col == nil)
		return TERRAIN_NONE;
	double area[NUM_RADAR_CLASSES] = {};
	for(int i = 0; i < col->numTriangles; i++){
		const CColTriangle *t = &col->triangles[i];
		rw::V3d v[3];
		int idx[3] = { t->a, t->b, t->c };
		for(int k = 0; k < 3; k++)
			v[k] = col->flags & 0x80 ? col->compVertices[idx[k]].Uncompress() : col->vertices[idx[k]];
		area[surfaceClass(t->surface)] += fabs((v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y));
	}
	for(int i = 0; i < col->numBoxes; i++){
		const CColBox &b = col->boxes[i];
		area[surfaceClass(b.surface)] += 2.0 * (b.box.max.x - b.box.min.x) * (b.box.max.y - b.box.min.y);
	}
	int best = TERRAIN_NONE;
	for(int c = TERRAIN_NONE + 1; c < NUM_RADAR_CLASSES; c++)
		if(area[c] > area[best])
			best = c;
	return (uint8)best;
}

// A model's footprint in its own frame: its mesh or its collision, else its
// collision bounding box, else its mesh's box.
static const std::vector<Tri> &
modelFootprint(int model, int shape)
{
	static std::unordered_map<int, std::vector<Tri>> cache;
	int key = model * 2 + (shape == RADAR_SHAPE_COLLISION);
	std::unordered_map<int, std::vector<Tri>>::iterator it = cache.find(key);
	if(it != cache.end())
		return it->second;
	std::vector<Tri> &tris = cache[key];
	ObjectDef *obj = GetObjectDef(model);
	if(shape == RADAR_SHAPE_MESH){
		addMesh(tris, obj);
		if(!tris.empty()){
			uint8 fallback = collisionClass(obj);
			for(Tri &t : tris)
				if(t.cls == TERRAIN_NONE)
					t.cls = fallback;
			return tris;
		}
	}
	CColModel *col = obj ? obj->m_colModel : nil;
	if(col){
		for(int i = 0; i < col->numTriangles; i++){
			const CColTriangle *t = &col->triangles[i];
			Tri tri;
			tri.cls = surfaceClass(t->surface);
			int idx[3] = { t->a, t->b, t->c };
			for(int k = 0; k < 3; k++)
				tri.v[k] = col->flags & 0x80 ? col->compVertices[idx[k]].Uncompress() : col->vertices[idx[k]];
			tris.push_back(tri);
		}
		for(int i = 0; i < col->numBoxes; i++)
			addBox(tris, col->boxes[i].box.min, col->boxes[i].box.max, surfaceClass(col->boxes[i].surface));
		for(int i = 0; i < col->numSpheres; i++){
			// The top of the sphere as an octagon.
			const CSphere &s = col->spheres[i].sph;
			rw::V3d top = { s.center.x, s.center.y, s.center.z + s.radius };
			for(int k = 0; k < 8; k++){
				float a0 = k * (float)M_PI / 4.0f, a1 = (k + 1) * (float)M_PI / 4.0f;
				rw::V3d p0 = { s.center.x + cosf(a0) * s.radius, s.center.y + sinf(a0) * s.radius, s.center.z };
				rw::V3d p1 = { s.center.x + cosf(a1) * s.radius, s.center.y + sinf(a1) * s.radius, s.center.z };
				tris.push_back({ { top, p0, p1 }, surfaceClass(col->spheres[i].surface) });
			}
		}
		if(tris.empty())
			addBox(tris, col->boundingBox.min, col->boundingBox.max);
		return tris;
	}
	rw::Atomic *atomic = obj ? obj->m_atomics[0] : nil;
	rw::Geometry *geo = atomic ? atomic->geometry : nil;
	if(geo && geo->morphTargets && geo->morphTargets[0].vertices && geo->numVertices > 0){
		rw::V3d mn = geo->morphTargets[0].vertices[0], mx = mn;
		for(int i = 1; i < geo->numVertices; i++){
			const rw::V3d &v = geo->morphTargets[0].vertices[i];
			mn = { std::min(mn.x, v.x), std::min(mn.y, v.y), std::min(mn.z, v.z) };
			mx = { std::max(mx.x, v.x), std::max(mx.y, v.y), std::max(mx.z, v.z) };
		}
		addBox(tris, mn, mx);
	}
	return tris;
}

// World axes of a stored (conjugated) rotation, as the map tools use them.
static rw::Matrix
basisOf(const rw::Quat &stored)
{
	rw::Matrix m;
	m.rotate(rw::conj(stored), rw::COMBINEREPLACE);
	return m;
}

static rw::V3d
toWorld(const rw::Matrix &m, const rw::V3d &pos, const rw::V3d &v)
{
	return add(pos, add(add(scale(m.right, v.x), scale(m.up, v.y)), scale(m.at, v.z)));
}

// ---- texture colours ------------------------------------------------------------

static uint32
rgb565(uint16 c)
{
	uint32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
	return ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
}

static void
addRGB(double sum[4], uint32 rgb, double weight)
{
	sum[0] += ((rgb >> 16) & 0xFF) * weight;
	sum[1] += ((rgb >> 8) & 0xFF) * weight;
	sum[2] += (rgb & 0xFF) * weight;
	sum[3] += weight;
}

// The colours of DXT blocks: 1, 3 or 5 (alpha blocks first for 3 and 5).
static void
sumDxt(const uint8 *data, int size, int dxt, double sum[4])
{
	int blockSize = dxt == 1 ? 8 : 16;
	for(int at = 0; at + blockSize <= size; at += blockSize){
		const uint8 *b = data + at + (dxt == 1 ? 0 : 8);
		uint16 c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
		uint32 p[4] = { rgb565(c0), rgb565(c1), 0, 0 };
		bool transparent = dxt == 1 && c0 <= c1;
		for(int k = 0; k < 3; k++){
			int a = (p[0] >> (k * 8)) & 0xFF, z = (p[1] >> (k * 8)) & 0xFF;
			p[2] |= (uint32)(transparent ? (a + z) / 2 : (2 * a + z) / 3) << (k * 8);
			p[3] |= (uint32)(transparent ? 0 : (a + 2 * z) / 3) << (k * 8);
		}
		uint32 bits = b[4] | b[5] << 8 | b[6] << 16 | (uint32)b[7] << 24;
		for(int k = 0; k < 16; k++){
			int index = (bits >> (k * 2)) & 3;
			if(!(transparent && index == 3))
				addRGB(sum, p[index], 1.0);
		}
	}
}

// The average colour of a texture, read from its smallest level of at least
// 4x4 pixels; false when its format isn't readable here.
static bool
rasterColour(rw::Raster *raster, uint32 *out)
{
	if(raster == nil || raster->width <= 0 || raster->height <= 0)
		return false;
	int dxt = 0;
	bool bgra = false;
#ifdef RW_GL3
	{
		using namespace rw::gl3;
		Gl3Raster *natras = GETGL3RASTEREXT(raster);
		if(natras->isCompressed){
			switch(natras->internalFormat){
			case 0x83F0: case 0x83F1: dxt = 1; break;	// GL_COMPRESSED_RGB(A)_S3TC_DXT1_EXT
			case 0x83F2: dxt = 3; break;
			case 0x83F3: dxt = 5; break;
			default: return false;
			}
		}
	}
#elif defined(RW_D3D9)
	{
		rw::d3d::D3dRaster *natras = GETD3DRASTEREXT(raster);
		bgra = true;
		if(natras->customFormat){
			switch(natras->format){
			case 0x31545844: dxt = 1; break;	// DXT1
			case 0x32545844: case 0x33545844: dxt = 3; break;
			case 0x34545844: case 0x35545844: dxt = 5; break;
			default: return false;
			}
		}
	}
#else
	return false;
#endif
	int fmt = raster->format & 0xF00;
	if(dxt == 0 && fmt != rw::Raster::C8888 && fmt != rw::Raster::C888)
		return false;
	if(raster->format & (rw::Raster::PAL4 | rw::Raster::PAL8))
		return false;
	int levels = std::max(1, raster->getNumLevels());
	int level = 0;
	while(level + 1 < levels && (raster->width >> (level + 1)) >= 4 && (raster->height >> (level + 1)) >= 4)
		level++;
	int w = std::max(1, raster->width >> level), h = std::max(1, raster->height >> level);
	uint8 *pixels = raster->lock(level, rw::Raster::LOCKREAD);
	if(pixels == nil)
		return false;
	double sum[4] = { 0, 0, 0, 0 };
	if(dxt){
		int size = ((w + 3) / 4) * ((h + 3) / 4) * (dxt == 1 ? 8 : 16);
		sumDxt(pixels, size, dxt, sum);
	}else{
		int bpp = fmt == rw::Raster::C8888 ? 4 : 3;
		int stride = raster->stride > 0 ? raster->stride : w * bpp;
		for(int y = 0; y < h; y++)
			for(int x = 0; x < w; x++){
				const uint8 *px = pixels + y * stride + x * bpp;
				double alpha = bpp == 4 ? px[3] / 255.0 : 1.0;
				uint32 r = bgra ? px[2] : px[0], b = bgra ? px[0] : px[2];
				addRGB(sum, r << 16 | px[1] << 8 | b, alpha);
			}
	}
	raster->unlock(level);
	if(sum[3] <= 0.0)
		return false;
	*out = (uint32)(sum[0] / sum[3]) << 16 | (uint32)(sum[1] / sum[3]) << 8 | (uint32)(sum[2] / sum[3]);
	return true;
}

static uint32
textureColour(rw::Texture *texture, uint32 fallback)
{
	static std::unordered_map<rw::Texture*, uint32> cache;
	if(texture == nil)
		return fallback;
	std::unordered_map<rw::Texture*, uint32>::iterator it = cache.find(texture);
	if(it != cache.end())
		return it->second;
	uint32 c;
	if(!rasterColour(texture->raster, &c))
		c = fallback;
	cache[texture] = c;
	return c;
}

// A model's colour from above: its materials' textures (times their
// colour), weighted by the area of the upward-facing triangles that use them.
static uint32
modelColour(int model, uint32 fallback)
{
	static std::unordered_map<int, uint32> cache;
	std::unordered_map<int, uint32>::iterator it = cache.find(model);
	if(it != cache.end())
		return it->second;
	ObjectDef *obj = GetObjectDef(model);
	if(obj && !obj->IsLoaded() && !obj->m_cantLoad)
		obj->Load();
	uint32 colour = fallback;
	rw::Atomic *atomic = obj ? obj->m_atomics[0] : nil;
	rw::Geometry *geo = atomic ? atomic->geometry : nil;
	if(geo && geo->matList.numMaterials > 0){
		std::vector<double> weight(geo->matList.numMaterials, 0.0);
		bool haveTris = geo->triangles && geo->morphTargets && geo->morphTargets[0].vertices;
		for(int i = 0; haveTris && i < geo->numTriangles; i++){
			const rw::Triangle &t = geo->triangles[i];
			if(t.matId >= geo->matList.numMaterials)
				continue;
			const rw::V3d *v = geo->morphTargets[0].vertices;
			rw::V3d n = cross(sub(v[t.v[1]], v[t.v[0]]), sub(v[t.v[2]], v[t.v[0]]));
			if(n.z > 0.0f)
				weight[t.matId] += n.z;	// twice the area seen from above
		}
		double total = 0.0;
		for(size_t m = 0; m < weight.size(); m++)
			total += weight[m];
		if(total <= 0.0)
			std::fill(weight.begin(), weight.end(), 1.0);
		double sum[4] = { 0, 0, 0, 0 };
		for(int m = 0; m < geo->matList.numMaterials; m++){
			rw::Material *mat = geo->matList.materials[m];
			if(mat == nil || weight[m] <= 0.0)
				continue;
			uint32 c = textureColour(mat->texture, fallback);
			// Times the material colour, as the game draws it.
			uint32 r = ((c >> 16) & 0xFF) * mat->color.red / 255;
			uint32 g = ((c >> 8) & 0xFF) * mat->color.green / 255;
			uint32 b = (c & 0xFF) * mat->color.blue / 255;
			addRGB(sum, r << 16 | g << 8 | b, weight[m]);
		}
		if(sum[3] > 0.0)
			colour = (uint32)(sum[0] / sum[3]) << 16 | (uint32)(sum[1] / sum[3]) << 8 | (uint32)(sum[2] / sum[3]);
	}
	cache[model] = colour;
	return colour;
}

// Distinct, radar-friendly colours for groups.
static uint32
groupColour(const char *group, uint32 fallback)
{
	static const uint32 palette[] = {
		0x4E79A7, 0xF28E2B, 0x59A14F, 0xE15759, 0x76B7B2, 0xEDC948,
		0xB07AA1, 0xFF9DA7, 0x9C755F, 0xBAB0AC, 0x2F6B3A, 0x8CD17D,
	};
	if(group == nil || group[0] == '\0')
		return fallback;
	uint32 hash = 2166136261u;
	for(const char *p = group; *p; p++)
		hash = (hash ^ (uint8)*p) * 16777619u;
	return palette[hash % (sizeof(palette)/sizeof(palette[0]))];
}

// k-means over the colours, weighted by how many cells have each; the
// first centres are the most common colours.
static void
quantise(std::vector<uint32> &cellColour, const std::vector<bool> &filled, int k)
{
	std::map<uint32, int> counts;
	for(size_t i = 0; i < cellColour.size(); i++)
		if(filled[i])
			counts[cellColour[i]]++;
	if((int)counts.size() <= k)
		return;
	std::vector<std::pair<int, uint32>> byCount;
	for(std::map<uint32, int>::iterator it = counts.begin(); it != counts.end(); ++it)
		byCount.push_back({ it->second, it->first });
	std::sort(byCount.begin(), byCount.end(), [](const std::pair<int, uint32> &a, const std::pair<int, uint32> &b){
		return a.first != b.first ? a.first > b.first : a.second < b.second;
	});
	auto ch = [](uint32 c, int i){ return (float)((c >> (16 - i * 8)) & 0xFF); };
	std::vector<float> centres;
	for(int i = 0; i < k; i++)
		for(int j = 0; j < 3; j++)
			centres.push_back(ch(byCount[i].second, j));
	std::map<uint32, int> nearest;
	for(int iteration = 0; iteration < 10; iteration++){
		std::vector<double> sums(k * 4, 0.0);
		for(size_t i = 0; i < byCount.size(); i++){
			uint32 c = byCount[i].second;
			int best = 0;
			float bestD = 1e30f;
			for(int j = 0; j < k; j++){
				float dr = ch(c, 0) - centres[j*3], dg = ch(c, 1) - centres[j*3+1], db = ch(c, 2) - centres[j*3+2];
				float d = dr*dr + dg*dg + db*db;
				if(d < bestD){
					bestD = d;
					best = j;
				}
			}
			nearest[c] = best;
			for(int j = 0; j < 3; j++)
				sums[best*4 + j] += ch(c, j) * byCount[i].first;
			sums[best*4 + 3] += byCount[i].first;
		}
		for(int j = 0; j < k; j++)
			if(sums[j*4 + 3] > 0.0)
				for(int c = 0; c < 3; c++)
					centres[j*3 + c] = (float)(sums[j*4 + c] / sums[j*4 + 3]);
	}
	std::vector<uint32> packed(k);
	for(int j = 0; j < k; j++)
		packed[j] = (uint32)(centres[j*3] + 0.5f) << 16 | (uint32)(centres[j*3+1] + 0.5f) << 8 |
			(uint32)(centres[j*3+2] + 0.5f);
	for(size_t i = 0; i < cellColour.size(); i++)
		if(filled[i])
			cellColour[i] = packed[nearest[cellColour[i]]];
}

// ---- building -------------------------------------------------------------------

bool
RadarArtBuild(const RadarArtParams &params, RadarArtResult *result, std::string *error)
{
	*result = RadarArtResult();
	result->cellSize = params.cellSize;
	if(!(params.cellSize >= 0.5f && params.cellSize <= 1000.0f)){
		if(error) *error = N_("the cell size must be 0.5 to 1000 m");
		return false;
	}
	std::vector<ObjectInst*> sources;
	collectSources(params, sources);
	if(sources.empty()){
		if(error) *error = params.source == RADAR_SOURCE_SELECTION ? N_("select some objects first") :
			params.source == RADAR_SOURCE_GROUP ? N_("that group has no objects") : N_("the map has no objects");
		return false;
	}

	// Each source's footprint in the world, and the bounds of them all.
	std::vector<std::vector<Tri>> world(sources.size());
	rw::V3d mn = { 1e30f, 1e30f, 1e30f }, mx = { -1e30f, -1e30f, -1e30f };
	for(size_t s = 0; s < sources.size(); s++){
		const std::vector<Tri> &local = modelFootprint(sources[s]->m_objectId, params.shape);
		if(local.empty()){
			result->skipped++;
			continue;
		}
		rw::Matrix m = basisOf(sources[s]->m_rotation);
		// SetObjectMaterial: a slot's texture says what the surface is now
		// (snow, road, an invisible texture...).
		uint8 slotClass[MAX_PAWN_MATERIALS];
		std::fill(std::begin(slotClass), std::end(slotClass), (uint8)TERRAIN_NONE);
		for(int k = 0; k < MAX_PAWN_MATERIALS; k++){
			const ObjectMaterialOverride *o = GetObjectMaterialOverride((const ObjectInst*)sources[s], k);
			if(o && o->active && !o->textEnabled && o->hasTexture && o->sourceModel != 0)
				slotClass[k] = nameClass(nil, o->txdName, o->textureName);
		}
		world[s].resize(local.size());
		for(size_t t = 0; t < local.size(); t++){
			// A road stays a road (black, or brown when dirt) under snow or any
			// other texture, as on the radar; only an invisible one hides it.
			uint8 own = local[t].cls, over = local[t].mat < MAX_PAWN_MATERIALS ? slotClass[local[t].mat] : TERRAIN_NONE;
			bool road = own == TERRAIN_ROAD || own == TERRAIN_RAIL;
			world[s][t].cls = over == TERRAIN_NONE || (road && over != TERRAIN_HIDDEN && over != TERRAIN_DIRT) ? own : over;
			for(int k = 0; k < 3; k++){
				rw::V3d v = toWorld(m, sources[s]->m_translation, local[t].v[k]);
				world[s][t].v[k] = v;
				mn = { std::min(mn.x, v.x), std::min(mn.y, v.y), std::min(mn.z, v.z) };
				mx = { std::max(mx.x, v.x), std::max(mx.y, v.y), std::max(mx.z, v.z) };
			}
		}
	}
	if(mn.x > mx.x){
		if(error) *error = N_("none of the objects has a footprint");
		return false;
	}
	float cs = params.cellSize;
	// Cells line up with multiples of the cell size, so art made in parts meets.
	float x0 = floorf(mn.x / cs) * cs, y0 = floorf(mn.y / cs) * cs;
	long long cols = (long long)ceilf((mx.x - x0) / cs) + 1, rows = (long long)ceilf((mx.y - y0) / cs) + 1;
	if(cols * rows > MAX_RADAR_CELLS){
		float need = sqrtf((mx.x - mn.x) * (mx.y - mn.y) / MAX_RADAR_CELLS);
		if(error){
			char buf[128];
			snprintf(buf, sizeof(buf), "too many cells for this area; use cells of at least %.1f m", ceilf(need * 10.0f) / 10.0f);
			*error = buf;
		}
		return false;
	}
	result->cols = (int)cols;
	result->rows = (int)rows;

	size_t numCells = (size_t)(cols * rows);
	std::vector<float> top(numCells, -1e30f);
	std::vector<int> owner(numCells, -1);
	std::vector<uint8> cellClass(numCells, TERRAIN_NONE);
	for(size_t s = 0; s < sources.size(); s++){
		if(world[s].empty())
			continue;
		result->objects++;
		for(const Tri &t : world[s]){
			if(t.cls == TERRAIN_HIDDEN)
				continue;
			const rw::V3d &a = t.v[0], &b = t.v[1], &c = t.v[2];
			float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
			if(fabsf(area) < 1e-6f)
				continue;	// a wall seen edge on
			int cx0 = std::max(0, (int)floorf((std::min(a.x, std::min(b.x, c.x)) - x0) / cs));
			int cx1 = std::min((int)cols - 1, (int)floorf((std::max(a.x, std::max(b.x, c.x)) - x0) / cs));
			int cy0 = std::max(0, (int)floorf((std::min(a.y, std::min(b.y, c.y)) - y0) / cs));
			int cy1 = std::min((int)rows - 1, (int)floorf((std::max(a.y, std::max(b.y, c.y)) - y0) / cs));
			for(int cy = cy0; cy <= cy1; cy++)
				for(int cx = cx0; cx <= cx1; cx++){
					// The cell's centre, by barycentric weights.
					float px = x0 + (cx + 0.5f) * cs, py = y0 + (cy + 0.5f) * cs;
					float w0 = ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) / area;
					float w1 = ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) / area;
					float w2 = 1.0f - w0 - w1;
					if(w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
						continue;
					float z = w0 * a.z + w1 * b.z + w2 * c.z;
					size_t i = (size_t)cy * cols + cx;
					if(z > top[i]){
						top[i] = z;
						owner[i] = (int)s;
						cellClass[i] = t.cls;
					}
				}
		}
	}

	// Colours.
	std::vector<bool> filled(numCells);
	std::vector<uint32> colour(numCells, 0);
	float zmin = 1e30f, zmax = -1e30f;
	std::vector<uint32> sourceColour(sources.size(), params.colour >> 8);
	for(size_t s = 0; s < sources.size(); s++){
		if(params.colourMode == RADAR_COLOUR_GROUP)
			sourceColour[s] = groupColour(GetObjectPawnProperties(sources[s])->group, params.colour >> 8);
		else if(params.colourMode == RADAR_COLOUR_TEXTURE && !world[s].empty())
			sourceColour[s] = modelColour(sources[s]->m_objectId, params.colour >> 8);
	}
	for(size_t i = 0; i < numCells; i++){
		filled[i] = owner[i] >= 0;
		if(filled[i] && params.aboveWater){
			// Under the water the radar shows the water, not the object.
			float wz;
			size_t cx = i % (size_t)cols, cy = i / (size_t)cols;
			if(WaterLevel::GetWaterHeight(x0 + (cx + 0.5f) * cs, y0 + (cy + 0.5f) * cs, &wz) && top[i] < wz)
				filled[i] = false;
		}
		if(!filled[i])
			continue;
		result->cells++;
		colour[i] = params.colourMode == RADAR_COLOUR_RADAR ? radarPalette[cellClass[i]] : sourceColour[owner[i]];
		zmin = std::min(zmin, top[i]);
		zmax = std::max(zmax, top[i]);
	}
	if(result->cells == 0){
		if(error) *error = N_("the footprints are smaller than one cell; use smaller cells");
		return false;
	}
	if(params.colourMode == RADAR_COLOUR_TEXTURE)
		quantise(colour, filled, clamp(params.numColours, 1, 64));
	// The radar's colours are flat; its roads have black edges.
	bool radar = params.colourMode == RADAR_COLOUR_RADAR;
	int levels = clamp(params.heightLevels, 2, 16);
	if(params.heightShading && !radar && zmax - zmin > 0.5f)
		for(size_t i = 0; i < numCells; i++){
			if(!filled[i])
				continue;
			int level = std::min(levels - 1, (int)((top[i] - zmin) / (zmax - zmin) * levels));
			float f = 0.55f + 0.45f * level / (levels - 1);
			uint32 c = colour[i];
			colour[i] = (uint32)(((c >> 16) & 0xFF) * f) << 16 | (uint32)(((c >> 8) & 0xFF) * f) << 8 |
				(uint32)((c & 0xFF) * f);
		}
	if(params.outline){
		std::vector<uint32> outlined = colour;
		for(long long cy = 0; cy < rows; cy++)
			for(long long cx = 0; cx < cols; cx++){
				size_t i = (size_t)(cy * cols + cx);
				if(!filled[i])
					continue;
				bool edge = cx == 0 || cy == 0 || cx == cols - 1 || cy == rows - 1 ||
					!filled[i - 1] || !filled[i + 1] || !filled[i - cols] || !filled[i + cols];
				if(edge)
					outlined[i] = params.outlineColour >> 8;
			}
		colour.swap(outlined);
	}

	// Greedy rectangles: the longest run of one colour along a row, then as
	// many rows up as repeat it exactly.
	uint32 alpha = (uint32)clamp(params.alpha, 0, 255);
	std::vector<bool> used(numCells);
	for(long long cy = 0; cy < rows; cy++)
		for(long long cx = 0; cx < cols; cx++){
			size_t i = (size_t)(cy * cols + cx);
			if(!filled[i] || used[i])
				continue;
			uint32 c = colour[i];
			long long w = 1;
			while(cx + w < cols && filled[i + w] && !used[i + w] && colour[i + w] == c)
				w++;
			long long h = 1;
			for(; cy + h < rows; h++){
				size_t row = (size_t)((cy + h) * cols + cx);
				bool same = true;
				for(long long k = 0; k < w && same; k++)
					same = filled[row + k] && !used[row + k] && colour[row + k] == c;
				if(!same)
					break;
			}
			float z = -1e30f;
			for(long long y = 0; y < h; y++)
				for(long long x = 0; x < w; x++){
					size_t j = (size_t)((cy + y) * cols + cx + x);
					used[j] = true;
					z = std::max(z, top[j]);
				}
			RadarArtZone zone = { x0 + cx * cs, y0 + cy * cs, x0 + (cx + w) * cs, y0 + (cy + h) * cs, z, c << 8 | alpha };
			result->zones.push_back(zone);
		}
	return true;
}

bool
RadarArtFitCellSize(const RadarArtParams &params, int maxZones, float *cellSize, std::string *error)
{
	if(maxZones < 1){
		if(error) *error = N_("fit to at least one zone");
		return false;
	}
	// Fewer, bigger cells make fewer zones (nearly always), so search the
	// cell size on a log scale and keep the smallest that fits.
	RadarArtParams p = params;
	RadarArtResult r;
	std::string why;
	float lo = 0.5f, hi = 1000.0f, best = -1.0f;
	for(int i = 0; i < 24 && hi - lo > 0.05f; i++){
		float mid = sqrtf(lo * hi);
		p.cellSize = mid;
		bool ok = RadarArtBuild(p, &r, &why);
		if(ok && (int)r.zones.size() <= maxZones){
			best = mid;
			hi = mid;
		}else if(!ok && why.find("too many cells") == std::string::npos && why.find("smaller than one cell") == std::string::npos){
			if(error) *error = why;
			return false;
		}else if(!ok && why.find("smaller than one cell") != std::string::npos)
			hi = mid;	// too coarse to show anything
		else
			lo = mid;
	}
	if(best < 0.0f){
		if(error) *error = N_("no cell size gets it down to that many zones");
		return false;
	}
	// Rounded up to 0.1 m; the grid then lines up differently, so check it
	// still fits and nudge it up if not.
	float cs = ceilf(best * 10.0f) / 10.0f;
	for(int i = 0; i < 50; i++, cs += 0.1f){
		p.cellSize = cs;
		if(RadarArtBuild(p, &r, &why) && (int)r.zones.size() <= maxZones){
			*cellSize = cs;
			return true;
		}
	}
	*cellSize = best;
	return true;
}

// ---- applying -------------------------------------------------------------------

// ---- the map's layers -------------------------------------------------------------

static std::vector<RadarArtLayer> gLayers;
static bool gLayersIn3D;
static int gLayersVersion;	// the 3D batch is rebuilt when it changes

const std::vector<RadarArtLayer> &
GetRadarArtLayers(void)
{
	return gLayers;
}

int
CountRadarArtZones(void)
{
	int n = 0;
	for(const RadarArtLayer &l : gLayers)
		n += (int)l.zones.size();
	return n;
}

static void
setLayers(const std::vector<RadarArtLayer> &layers)
{
	gLayers = layers;
	gLayersVersion++;
	BumpChangeSeq();
}

void
SetRadarArtLayers(const std::vector<RadarArtLayer> &layers, bool recordUndo)
{
	std::shared_ptr<void> before = std::make_shared<std::vector<RadarArtLayer>>(gLayers);
	setLayers(layers);
	if(recordUndo)
		UndoRecordRadarArt(before, std::make_shared<std::vector<RadarArtLayer>>(gLayers));
}

void
ClearRadarArtLayers(void)
{
	gLayers.clear();
	gLayersVersion++;
}

void
RestoreRadarArtLayers(const std::shared_ptr<void> &snapshot)
{
	if(snapshot)
		setLayers(*(const std::vector<RadarArtLayer>*)snapshot.get());
}

// Above any id the map hands out, so they sort after the map's own zones.
static const int32 STAND_IN_ID = 0x40000000;

void
MakeRadarArtStandIns(std::vector<ObjectInst*> &out)
{
	int proxy = GetSampElementProxyModel(SAMP_ELEMENT_GANGZONE);
	int32 id = STAND_IN_ID;
	for(const RadarArtLayer &l : gLayers)
		for(const RadarArtZone &z : l.zones){
			ObjectInst *inst = new ObjectInst;
			memset(inst, 0, sizeof(ObjectInst));
			inst->m_id = id++;
			inst->m_objectId = proxy;
			inst->m_imageIndex = -1;
			inst->m_binInstIndex = -1;
			inst->m_lodId = -1;
			inst->m_rotation.w = 1.0f;
			inst->m_translation = { (z.minX + z.maxX) * 0.5f, (z.minY + z.maxY) * 0.5f, z.z };
			ObjectPawnProperties *p = new ObjectPawnProperties;
			InitObjectPawnProperties(p);
			p->elementKind = SAMP_ELEMENT_GANGZONE;
			p->zoneSizeX = z.maxX - z.minX;
			p->zoneSizeY = z.maxY - z.minY;
			p->zoneColor = z.colour;
			p->zoneMode = l.zoneMode;
			snprintf(p->group, sizeof(p->group), "%s", l.group);
			inst->m_pawnProperties = p;
			out.push_back(inst);
		}
}

void
FreeRadarArtStandIns(std::vector<ObjectInst*> &standIns)
{
	for(ObjectInst *inst : standIns)
		delete inst;
	standIns.clear();
}

bool
IsRadarArtStandIn(const ObjectInst *inst)
{
	return inst && inst->m_id >= STAND_IN_ID;
}

bool
RadarArtLayersShownIn3D(void)
{
	return gLayersIn3D;
}

void
SetRadarArtLayersShownIn3D(bool on)
{
	gLayersIn3D = on;
}

// Every zone as a flat quad a little over its top, in one batch.
void
RenderRadarArtLayers(void)
{
	if(!gLayersIn3D || gLayers.empty())
		return;
	static std::vector<rw::RWDEVICE::Im3DVertex> verts;
	static std::vector<uint16> indices;
	static int builtVersion = -1;
	if(builtVersion != gLayersVersion){
		builtVersion = gLayersVersion;
		verts.clear();
		for(const RadarArtLayer &l : gLayers)
			for(const RadarArtZone &z : l.zones){
				float x[4] = { z.minX, z.maxX, z.maxX, z.minX }, y[4] = { z.minY, z.minY, z.maxY, z.maxY };
				for(int k = 0; k < 4; k++){
					rw::RWDEVICE::Im3DVertex v;
					v.setX(x[k]); v.setY(y[k]); v.setZ(z.z + 0.3f);
					v.setColor(z.colour >> 24 & 0xFF, z.colour >> 16 & 0xFF, z.colour >> 8 & 0xFF,
						std::min(160u, z.colour & 0xFF));
					v.setU(0.0f); v.setV(0.0f);
					verts.push_back(v);
				}
			}
		indices.clear();
		for(int q = 0; q < 16384; q++){
			uint16 b = (uint16)(q * 4);
			uint16 ix[6] = { b, (uint16)(b + 1), (uint16)(b + 2), b, (uint16)(b + 2), (uint16)(b + 3) };
			indices.insert(indices.end(), ix, ix + 6);
		}
	}
	rw::SetRenderStatePtr(rw::TEXTURERASTER, nil);
	rw::SetRenderState(rw::VERTEXALPHA, 1);
	rw::SetRenderState(rw::ZWRITEENABLE, 0);
	rw::SetRenderState(rw::CULLMODE, rw::CULLNONE);
	// 16-bit indices: at most 16384 quads a batch.
	for(size_t first = 0; first < verts.size(); first += 16384 * 4){
		int n = (int)std::min(verts.size() - first, (size_t)16384 * 4);
		rw::im3d::Transform(&verts[first], n, nil, rw::im3d::EVERYTHING);
		rw::im3d::RenderIndexedPrimitive(rw::PRIMTYPETRILIST, indices.data(), n / 4 * 6);
		rw::im3d::End();
	}
	rw::SetRenderState(rw::ZWRITEENABLE, 1);
}

// Map Objects: a row for each layer above the map's objects.
void
RadarArtLayersOutliner(void)
{
	if(gLayers.empty())
		return;
	static const char *modes[] = { N_("global"), N_("per player"), N_("streamed") };
	int remove = -1;
	for(size_t i = 0; i < gLayers.size(); i++){
		const RadarArtLayer &l = gLayers[i];
		ImGui::PushID((int)i);
		// The layer's name and size, its show and delete buttons at the row's right end.
		float button = ImGui::GetFrameHeight(), spacing = ImGui::GetStyle().ItemSpacing.x;
		float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		char text[192];
		snprintf(text, sizeof(text), _("Radar art \"%s\": %d zones, %s"), l.group, (int)l.zones.size(),
			_(modes[std::min(std::max(l.zoneMode, 0), 2)]));
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(ICON_FA_VECTOR_SQUARE);
		ImGui::SameLine();
		UiElidedText(text, std::max(ImGui::GetFontSize(), right - ImGui::GetCursorPosX() - (button + spacing) * 2));
		ImGui::SameLine();
		if(ImGui::GetCursorPosX() < right - button * 2 - spacing)
			ImGui::SetCursorPosX(right - button * 2 - spacing);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		if(ImGui::Button(gLayersIn3D ? ICON_FA_EYE "##radar_layer_3d" : ICON_FA_EYE_SLASH "##radar_layer_3d", ImVec2(button, button)))
			gLayersIn3D = !gLayersIn3D;
		ImGui::SetItemTooltip("%s", gLayersIn3D ? _("Shown in the 3D view: click to hide.") :
			_("Click to show the zones in the 3D view. The radar (Windows > Show Radar) always shows them."));
		ImGui::SameLine();
		if(ImGui::Button(ICON_FA_TRASH "##radar_layer_delete", ImVec2(button, button)))
			remove = (int)i;
		ImGui::PopStyleColor();
		ImGui::SetItemTooltip("%s", _("Delete this radar art (Ctrl+Z brings it back)."));
		ImGui::PopID();
	}
	ImGui::Separator();
	if(remove >= 0){
		std::vector<RadarArtLayer> layers = gLayers;
		layers.erase(layers.begin() + remove);
		SetRadarArtLayers(layers, true);
	}
}

int
RadarArtApply(const RadarArtParams &params, std::string *error)
{
	if(params.outGroup[0] == '\0'){
		if(error) *error = N_("name the group the zones go in");
		return -1;
	}
	RadarArtResult result;
	if(!RadarArtBuild(params, &result, error))
		return -1;
	if((int)result.zones.size() > MAX_RADAR_ZONES){
		if(error) *error = std::to_string(result.zones.size()) + " zones is more than " +
			std::to_string((int)MAX_RADAR_ZONES) + "; use bigger cells";
		return -1;
	}
	if(result.zones.empty()){
		if(error) *error = N_("could not make the zones");
		return -1;
	}
	UndoBeginGroup();
	std::vector<RadarArtLayer> layers = gLayers;
	if(params.replace){
		layers.erase(std::remove_if(layers.begin(), layers.end(), [&](const RadarArtLayer &l){
			return strcmp(l.group, params.outGroup) == 0;
		}), layers.end());
		// Gang zone elements in that group, from before radar art had layers.
		std::vector<ObjectInst*> old;
		for(CPtrNode *p = instances.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(inst && !inst->m_isDeleted && GetSampElementKind(inst) == SAMP_ELEMENT_GANGZONE &&
			   strcmp(GetObjectPawnProperties(inst)->group, params.outGroup) == 0)
				old.push_back(inst);
		}
		for(size_t i = 0; i < old.size(); i++)
			old[i]->Delete();
		if(!old.empty())
			UndoRecordDelete(old.data(), (int)old.size());
	}
	RadarArtLayer layer;
	snprintf(layer.group, sizeof(layer.group), "%s", params.outGroup);
	layer.zoneMode = params.zoneMode;
	layer.zones = result.zones;
	layers.push_back(layer);
	SetRadarArtLayers(layers, true);
	UndoEndGroup();
	return (int)result.zones.size();
}

// ---- streamed grid ----------------------------------------------------------------

void
GetStreamedZoneGrid(const std::vector<float> &bounds, StreamedZoneGrid *grid)
{
	*grid = StreamedZoneGrid();
	size_t n = bounds.size() / 4;
	if(n == 0){
		grid->cols = grid->rows = 1;
		return;
	}
	const float cs = STREAMED_ZONE_CELL;
	float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
	for(size_t i = 0; i < n; i++){
		float cx = (bounds[i*4] + bounds[i*4+2]) * 0.5f, cy = (bounds[i*4+1] + bounds[i*4+3]) * 0.5f;
		mnx = std::min(mnx, cx); mxx = std::max(mxx, cx);
		mny = std::min(mny, cy); mxy = std::max(mxy, cy);
	}
	grid->x0 = (floorf(mnx / cs) - 1.0f) * cs;
	grid->y0 = (floorf(mny / cs) - 1.0f) * cs;
	grid->cols = (int)(floorf(mxx / cs) - floorf(mnx / cs)) + 3;
	grid->rows = (int)(floorf(mxy / cs) - floorf(mny / cs)) + 3;
	std::vector<int> count(grid->cols * grid->rows, 0);
	for(size_t i = 0; i < n; i++){
		float cx = (bounds[i*4] + bounds[i*4+2]) * 0.5f, cy = (bounds[i*4+1] + bounds[i*4+3]) * 0.5f;
		int col = clamp((int)floorf((cx - grid->x0) / cs), 0, grid->cols - 1);
		int row = clamp((int)floorf((cy - grid->y0) / cs), 0, grid->rows - 1);
		count[row * grid->cols + col]++;
	}
	for(int row = 0; row < grid->rows; row++)
		for(int col = 0; col < grid->cols; col++){
			int sum = 0;
			for(int dy = -1; dy <= 1; dy++)
				for(int dx = -1; dx <= 1; dx++){
					int r = row + dy, c = col + dx;
					if(r >= 0 && c >= 0 && r < grid->rows && c < grid->cols)
						sum += count[r * grid->cols + c];
				}
			grid->capacity = std::max(grid->capacity, sum);
		}
}

// ---- the panel --------------------------------------------------------------------

static RadarArtParams gParams;
static bool gParamsReady;
static RadarArtResult gResult;
static std::string gError;
static bool gHaveResult;
static bool gPreview = true;
static int gFitTo = 1000;
static double gChangedAt = -1.0;
static RadarArtParams gBuiltParams;
static uint32 gBuiltSeq;
static size_t gBuiltSelection;

static size_t
selectionKey(void)
{
	size_t key = 0;
	for(CPtrNode *p = selection.first; p; p = p->next)
		key = key * 31 + (size_t)((ObjectInst*)p->item)->m_id + 1;
	return key;
}

static void
rebuildIfNeeded(void)
{
	bool changed = !gHaveResult || memcmp(&gParams, &gBuiltParams, sizeof(gParams)) != 0 ||
		gBuiltSeq != GetLatestChangeSeq() ||
		(gParams.source == RADAR_SOURCE_SELECTION && gBuiltSelection != selectionKey());
	if(!changed){
		gChangedAt = -1.0;
		return;
	}
	// Wait for the sliders to settle: building can take a moment.
	double now = ImGui::GetTime();
	if(gChangedAt < 0.0)
		gChangedAt = now;
	if(gHaveResult && now - gChangedAt < 0.3)
		return;
	gError.clear();
	if(!RadarArtBuild(gParams, &gResult, &gError))
		gResult = RadarArtResult();
	gHaveResult = true;
	gBuiltParams = gParams;
	gBuiltSeq = GetLatestChangeSeq();
	gBuiltSelection = selectionKey();
	gChangedAt = -1.0;
}

static bool
colourEdit(const char *label, uint32 *rgb00)
{
	float c[3] = { ((*rgb00 >> 24) & 0xFF) / 255.0f, ((*rgb00 >> 16) & 0xFF) / 255.0f, ((*rgb00 >> 8) & 0xFF) / 255.0f };
	if(!UiColorEdit3(label, c, ImGuiColorEditFlags_NoInputs))
		return false;
	*rgb00 = (uint32)(c[0] * 255.0f + 0.5f) << 24 | (uint32)(c[1] * 255.0f + 0.5f) << 16 |
		(uint32)(c[2] * 255.0f + 0.5f) << 8;
	return true;
}

static void
drawPreview(void)
{
	size_t n = std::min(gResult.zones.size(), (size_t)6000);
	for(size_t i = 0; i < n; i++){
		const RadarArtZone &z = gResult.zones[i];
		rw::RGBA c = { (uint8)(z.colour >> 24), (uint8)(z.colour >> 16), (uint8)(z.colour >> 8), 255 };
		float h = z.z + 0.5f;
		RenderLine({ z.minX, z.minY, h }, { z.maxX, z.minY, h }, c, c);
		RenderLine({ z.maxX, z.minY, h }, { z.maxX, z.maxY, h }, c, c);
		RenderLine({ z.maxX, z.maxY, h }, { z.minX, z.maxY, h }, c, c);
		RenderLine({ z.minX, z.maxY, h }, { z.minX, z.minY, h }, c, c);
	}
}

void
RadarArtPanel(void)
{
	if(!gParamsReady){
		RadarArtDefaults(&gParams);
		gParamsReady = true;
	}
	if(!UiSection(LI(ICON_FA_SQUARE, "Radar art from objects"))){
		gHaveResult = false;	// built again when opened
		return;
	}
	ImGui::PushID("radar_art");
	RadarArtParams &p = gParams;

	static const char *sources[] = { N_("All objects"), N_("Selection"), N_("A group") };
	UiCombo(_("Objects"), &p.source, sources, IM_ARRAYSIZE(sources));
	ImGui::SetItemTooltip(_("The objects drawn: every scripted object, the selection, or one group.\n"
		"Vehicles, SA-MP elements and unchanged world objects are left out."));
	if(p.source == RADAR_SOURCE_GROUP){
		std::vector<std::string> groups;
		for(CPtrNode *n = instances.first; n; n = n->next){
			ObjectInst *inst = (ObjectInst*)n->item;
			if(!isSourceObject(inst))
				continue;
			const char *g = GetObjectPawnProperties(inst)->group;
			if(g[0] && std::find(groups.begin(), groups.end(), g) == groups.end())
				groups.push_back(g);
		}
		std::sort(groups.begin(), groups.end());
		if(UiBeginCombo(_("Group"), p.group[0] ? p.group : _("(pick one)"))){
			for(size_t i = 0; i < groups.size(); i++)
				if(ImGui::Selectable(groups[i].c_str(), groups[i] == p.group))
					snprintf(p.group, sizeof(p.group), "%s", groups[i].c_str());
			if(groups.empty())
				ImGui::TextDisabled(_("No object is in a group."));
			ImGui::EndCombo();
		}
	}

	static const char *shapes[] = { N_("Model"), N_("Collision") };
	UiCombo(_("Shape"), &p.shape, shapes, IM_ARRAYSIZE(shapes));
	ImGui::SetItemTooltip(_("Model: the objects as they are drawn, seen from above.\n"
		"Collision: their collision, which is often boxes (a tree only its trunk)."));
	UiDragFloat(_("Cell size"), &p.cellSize, 0.1f, 0.5f, 1000.0f, "%.1f m");
	ImGui::SetItemTooltip(_("The art's pixel size. Smaller cells follow the shapes more\n"
		"closely and take more gang zones."));
	p.cellSize = clamp(p.cellSize, 0.5f, 1000.0f);
	// The zone count to fit, its button filling the rest of the row.
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
	ImGui::InputInt("##fit_to", &gFitTo, 0);
	gFitTo = clamp(gFitTo, 1, (int)MAX_RADAR_ZONES);
	ImGui::SameLine();
	if(ImGui::Button(_("Fit to this many zones"), ImVec2(-FLT_MIN, 0))){
		float cs;
		std::string why;
		if(RadarArtFitCellSize(p, gFitTo, &cs, &why))
			p.cellSize = cs;
		else
			ToastFor(TOAST_SELECTION, 4.0f, _("Radar art: %s"), _(why.c_str()));
	}
	ImGui::SetItemTooltip(_("Picks the smallest cell size whose art needs at most this many zones."));
	UiCheckbox(_("Leave out what's under water"), &p.aboveWater);
	ImGui::SetItemTooltip(_("Parts of the objects below the water's surface (a rock's\n"
		"wide base, a pier's legs) aren't drawn, as the radar shows the water there."));

	ImGui::Spacing();
	ImGui::Spacing();
	static const char *colourModes[] = { N_("One colour"), N_("By group"), N_("From textures"), N_("Radar colours") };
	UiCombo(_("Colour"), &p.colourMode, colourModes, IM_ARRAYSIZE(colourModes));
	ImGui::SetItemTooltip(_("Radar colours: the game's own map colours, by what each surface is\n"
		"(roads, grass, sand, rock, water...), from its collision and texture names.\n"
		"From textures: each model's colour seen from above, from its\n"
		"textures, reduced to a few colours so neighbouring cells merge."));
	if(p.colourMode == RADAR_COLOUR_TEXTURE){
		UiSliderInt(_("Colours"), &p.numColours, 1, 16);
		ImGui::SetItemTooltip(_("Fewer colours make fewer zones."));
	}
	bool radar = p.colourMode == RADAR_COLOUR_RADAR;
	if(!radar){
		colourEdit(p.colourMode == RADAR_COLOUR_ONE ? _("Zone colour") : _("Fallback colour"), &p.colour);
		if(p.colourMode != RADAR_COLOUR_ONE)
			ImGui::SetItemTooltip("%s", p.colourMode == RADAR_COLOUR_GROUP ? _("For objects without a group.") :
				_("For models whose textures can't be read."));
	}
	UiSliderInt(_("Opacity"), &p.alpha, 0, 255);
	UiCheckbox(_("Outline"), &p.outline);
	ImGui::SetItemTooltip(_("Draws the footprints' edge cells in their own colour."));
	if(p.outline){
		ImGui::SameLine();
		colourEdit("##outline_colour", &p.outlineColour);
	}
	if(!radar){
		UiCheckbox(_("Height shading"), &p.heightShading);
		ImGui::SetItemTooltip(_("Higher surfaces are lighter, in a few steps."));
		if(p.heightShading)
			UiSliderInt(_("Steps"), &p.heightLevels, 2, 8);
	}

	ImGui::Spacing();
	ImGui::Spacing();
	static const char *modes[] = { N_("Global zones"), N_("Per-player zones"), N_("Streamed per-player zones") };
	UiCombo(_("Shown as"), &p.zoneMode, modes, IM_ARRAYSIZE(modes));
	ImGui::SetItemTooltip(_("Global: GangZoneCreate, 1024 on the whole server.\n"
		"Per-player: CreatePlayerGangZone for every player, 1024 each.\n"
		"Streamed: per-player zones made around each player as they move,\n"
		"in %.0f m cells (streamer areas), so a map can have many more."), STREAMED_ZONE_CELL);
	if(p.zoneMode != SAMP_ZONE_GLOBAL){
		UiBadge(_("open.mp only"), _("CreatePlayerGangZone is an open.mp native; SA-MP 0.3.7 has no per-player zones."));
		if(p.zoneMode == SAMP_ZONE_STREAMED){
			ImGui::SameLine();
			UiBadge(_("streamer"), _("The map includes the streamer for its areas."));
		}
	}
	UiInputText(_("Group##out_group"), p.outGroup, sizeof(p.outGroup));
	ImGui::SetItemTooltip(_("The group the zones go in."));
	UiCheckbox(_("Replace this group's zones"), &p.replace);

	rebuildIfNeeded();
	ImGui::Spacing();
	ImGui::Spacing();
	// The result as quiet lines, then preview and the one main action.
	if(!gError.empty()){
		ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(UI_COLOR_ERROR));
		ImGui::TextWrapped("%s", gError.c_str());
		ImGui::PopStyleColor();
	}
	else if(gHaveResult){
		int n = (int)gResult.zones.size();
		bool over = false;
		if(p.zoneMode == SAMP_ZONE_STREAMED){
			std::vector<float> bounds;
			for(const RadarArtZone &z : gResult.zones){
				bounds.push_back(z.minX); bounds.push_back(z.minY);
				bounds.push_back(z.maxX); bounds.push_back(z.maxY);
			}
			StreamedZoneGrid grid;
			GetStreamedZoneGrid(bounds, &grid);
			over = grid.capacity > 1024;
			ImGui::Text(_("%d zones, at most %d around a player"), n, grid.capacity);
		}else{
			over = n > 1024;
			ImGui::Text(_("%d zones"), n);
		}
		if(over){
			ImGui::SameLine();
			ImGui::TextColored(UiSemanticColor(UI_COLOR_ERROR), _("(over 1024)"));
			ImGui::SetItemTooltip("%s", p.zoneMode == SAMP_ZONE_GLOBAL ?
				_("The server makes at most 1024 gang zones. Use bigger cells, fewer colours,\n"
				"or streamed zones.") : _("A player sees at most 1024 gang zones. Use bigger cells or fewer colours."));
		}
		ImGui::TextDisabled(gResult.skipped ? _("%d objects, %d x %d cells (some without a footprint)") :
			_("%d objects, %d x %d cells"), gResult.objects, gResult.cols, gResult.rows);
	}else
		ImGui::TextDisabled(_("Working..."));
	UiCheckbox(_("Preview##radar_preview"), &gPreview);
	ImGui::BeginDisabled(!gError.empty() || !gHaveResult || gResult.zones.empty());
	ImVec4 fill = UiAccentFill();
	ImGui::PushStyleColor(ImGuiCol_Button, fill);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(fill.x*1.12f, fill.y*1.12f, fill.z*1.12f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(fill.x*0.9f, fill.y*0.9f, fill.z*0.9f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
	bool apply = ImGui::Button(_("Apply##radar_apply"), ImVec2(-FLT_MIN, 0));
	ImGui::PopStyleColor(4);
	if(apply){
		std::string why;
		int made = RadarArtApply(p, &why);
		if(made < 0)
			ToastFor(TOAST_SELECTION, 4.0f, _("Radar art: %s"), _(why.c_str()));
		else
			Toast(TOAST_SELECTION, _("Radar art: %d gang zone(s) in \"%s\""), made, p.outGroup);
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip(_("Makes the zones, as one undo step."));
	if(gPreview && gHaveResult && gError.empty())
		drawPreview();
	ImGui::PopID();
}
