#include "haumea.h"
#include "modloader.h"
#include <limits.h>
#include <algorithm>

namespace WaterLevel
{

#define WATERSTARTX params.waterStart.x
#define WATERENDX params.waterEnd.x
#define WATERSTARTY params.waterStart.y
#define WATERENDY params.waterEnd.y
#define WATERSMALLSECTORSZX ((WATERENDX - WATERSTARTX)/128)
#define WATERSMALLSECTORSZY ((WATERENDY - WATERSTARTY)/128)
#define WATERSECTORSZX ((WATERENDX - WATERSTARTX)/64)
#define WATERSECTORSZY ((WATERENDY - WATERSTARTY)/64)

static int ms_nNoOfWaterLevels;
static float ms_aWaterZs[48];
static CRect ms_aWaterRects[48];	// Seems to be unused
static uint8 aWaterBlockList[64][64];
static uint8 aWaterFineBlockList[128][128];

rw::Texture *gpWaterTex;
rw::Raster *gpWaterRaster;

// SA
WaterVertex waterVertices[NUMWATERVERTICES];
int numWaterVertices;
WaterQuad waterQuads[NUMWATERQUADS];
int numWaterQuads;
WaterTri waterTris[NUMWATERTRIS];
int numWaterTris;

// Game engine limits (what the game actually supports)
#define SA_MAX_QUADS 301
#define SA_MAX_TRIS 6
#define SA_MAX_VERTICES 1021

// Editor state

// Accessor API

void
InitialiseWaterpro(void)
{
	FILE *file;

	ms_nNoOfWaterLevels = 0;
	if(file = fopen_ci("DATA\\waterpro.dat", "rb"), file == nil)
		return;
	fread(&ms_nNoOfWaterLevels, 1, sizeof(ms_nNoOfWaterLevels), file);
	fread(&ms_aWaterZs, 1, sizeof(ms_aWaterZs), file);
	fread(&ms_aWaterRects, 1, sizeof(ms_aWaterRects), file);
	fread(&aWaterBlockList, 1, sizeof(aWaterBlockList), file);
	fread(&aWaterFineBlockList, 1, sizeof(aWaterFineBlockList), file);
	fclose(file);
}

void
InitialiseWater(void)
{
	WaterVertex v[4];
	int flags;
	int nfields;

	FILE *file;
	char *line;
	if(file = fopen_ci("data/water.dat", "rb"), file == nil)
		return;
	int h = 0, w = 0;
	while(line = FileLoader::LoadLine(file)){
		if(line[0] == ';' || line[0] == '*' || line[0] == 'p')
			continue;
		flags = 1;
		nfields = sscanf(line,
			"%f %f %f %f %f %f %f "
			"%f %f %f %f %f %f %f "
			"%f %f %f %f %f %f %f "
			"%f %f %f %f %f %f %f "
			"%d",
			&v[0].pos.x, &v[0].pos.y, &v[0].pos.z, &v[0].speed.x, &v[0].speed.y, &v[0].waveunk, &v[0].waveheight,
			&v[1].pos.x, &v[1].pos.y, &v[1].pos.z, &v[1].speed.x, &v[1].speed.y, &v[1].waveunk, &v[1].waveheight,
			&v[2].pos.x, &v[2].pos.y, &v[2].pos.z, &v[2].speed.x, &v[2].speed.y, &v[2].waveunk, &v[2].waveheight,
			&v[3].pos.x, &v[3].pos.y, &v[3].pos.z, &v[3].speed.x, &v[3].speed.y, &v[3].waveunk, &v[3].waveheight,
			&flags);
		if(nfields == 28 || nfields == 29){
			// quad
			if(numWaterVertices+4 > NUMWATERVERTICES ||
			   numWaterQuads+1 > NUMWATERQUADS){
				log("warning: too much water (%d vertices, %d quads, %d tris)\n",
					numWaterVertices, numWaterQuads, numWaterTris);
				continue;
			}
			WaterQuad *q = &waterQuads[numWaterQuads++];
			q->indices[0] = numWaterVertices++;
			q->indices[1] = numWaterVertices++;
			q->indices[2] = numWaterVertices++;
			q->indices[3] = numWaterVertices++;
			waterVertices[q->indices[0]] = v[0];
			waterVertices[q->indices[1]] = v[1];
			waterVertices[q->indices[2]] = v[2];
			waterVertices[q->indices[3]] = v[3];
			q->flags = flags;
		}else{
			if(numWaterVertices+3 > NUMWATERVERTICES ||
			   numWaterTris+1 > NUMWATERTRIS){
				log("warning: too much water (%d vertices, %d quads, %d tris)\n",
					numWaterVertices, numWaterQuads, numWaterTris);
				continue;
			}
			// triangle
			WaterTri *t = &waterTris[numWaterTris++];
			t->indices[0] = numWaterVertices++;
			t->indices[1] = numWaterVertices++;
			t->indices[2] = numWaterVertices++;
			waterVertices[t->indices[0]] = v[0];
			waterVertices[t->indices[1]] = v[1];
			waterVertices[t->indices[2]] = v[2];
			t->flags = flags;
		}
	}
	fclose(file);
}

void
Initialise(void)
{
	if(params.water == GAME_SA)
		InitialiseWater();
	else
		InitialiseWaterpro();

	TxdPush();
	int ptxd = FindTxdSlot("particle");
	TxdMakeCurrent(ptxd);
	if(gpWaterTex == nil)
		gpWaterTex = rw::Texture::read(params.waterTex, nil);
	gpWaterRaster = gpWaterTex->raster;
	TxdPop();
}

#define TEMPBUFFERVERTSIZE 4096
#define TEMPBUFFERINDEXSIZE 16384
static int TempBufferIndicesStored;
static int TempBufferVerticesStored;
static rw::RWDEVICE::Im3DVertex TempVertexBuffer[TEMPBUFFERVERTSIZE];
static uint16 TempIndexBuffer[TEMPBUFFERINDEXSIZE];

float TEXTURE_ADDU, TEXTURE_ADDV;

static void
RenderAndEmptyRenderBuffer(void)
{
	assert(TempBufferVerticesStored <= TEMPBUFFERVERTSIZE);
	assert(TempBufferIndicesStored <= TEMPBUFFERINDEXSIZE);
	if(TempBufferVerticesStored){
		rw::im3d::Transform(TempVertexBuffer, TempBufferVerticesStored, nil, rw::im3d::EVERYTHING);
		rw::im3d::RenderIndexedPrimitive(rw::PRIMTYPETRILIST, TempIndexBuffer, TempBufferIndicesStored);
		rw::im3d::End();
	}
	TempBufferVerticesStored = 0;
	TempBufferIndicesStored = 0;
}

void
RenderOneFlatSmallWaterPoly(float x, float y, float z, rw::RGBA const &color)
{
	if(TempBufferVerticesStored+4 >= TEMPBUFFERVERTSIZE ||
	   TempBufferIndicesStored+6 >= TEMPBUFFERINDEXSIZE)
		RenderAndEmptyRenderBuffer();

	int i = TempBufferVerticesStored;
	TempVertexBuffer[i + 0].setX(x);
	TempVertexBuffer[i + 0].setY(y);
	TempVertexBuffer[i + 0].setZ(z);
	TempVertexBuffer[i + 0].setU(TEXTURE_ADDU);
	TempVertexBuffer[i + 0].setV(TEXTURE_ADDV);
	TempVertexBuffer[i + 0].setColor(color.red, color.green, color.blue, color.alpha);

	TempVertexBuffer[i + 1].setX(x);
	TempVertexBuffer[i + 1].setY(y + WATERSMALLSECTORSZY);
	TempVertexBuffer[i + 1].setZ(z);
	TempVertexBuffer[i + 1].setU(TEXTURE_ADDU);
	TempVertexBuffer[i + 1].setV(TEXTURE_ADDV + 1.0f);
	TempVertexBuffer[i + 1].setColor(color.red, color.green, color.blue, color.alpha);

	TempVertexBuffer[i + 2].setX(x + WATERSMALLSECTORSZX);
	TempVertexBuffer[i + 2].setY(y + WATERSMALLSECTORSZY);
	TempVertexBuffer[i + 2].setZ(z);
	TempVertexBuffer[i + 2].setU(TEXTURE_ADDU + 1.0f);
	TempVertexBuffer[i + 2].setV(TEXTURE_ADDV + 1.0f);
	TempVertexBuffer[i + 2].setColor(color.red, color.green, color.blue, color.alpha);

	TempVertexBuffer[i + 3].setX(x + WATERSMALLSECTORSZX);
	TempVertexBuffer[i + 3].setY(y);
	TempVertexBuffer[i + 3].setZ(z);
	TempVertexBuffer[i + 3].setU(TEXTURE_ADDU + 1.0f);
	TempVertexBuffer[i + 3].setV(TEXTURE_ADDV);
	TempVertexBuffer[i + 3].setColor(color.red, color.green, color.blue, color.alpha);

	TempBufferVerticesStored += 4;
	TempIndexBuffer[TempBufferIndicesStored++] = i+0;
	TempIndexBuffer[TempBufferIndicesStored++] = i+1;
	TempIndexBuffer[TempBufferIndicesStored++] = i+2;
	TempIndexBuffer[TempBufferIndicesStored++] = i+0;
	TempIndexBuffer[TempBufferIndicesStored++] = i+2;
	TempIndexBuffer[TempBufferIndicesStored++] = i+3;
}

void
RenderOneFlatLargeWaterPoly(float x, float y, float z, rw::RGBA const &color)
{
	if(TempBufferVerticesStored+4 >= TEMPBUFFERVERTSIZE ||
	   TempBufferIndicesStored+6 >= TEMPBUFFERINDEXSIZE)
		RenderAndEmptyRenderBuffer();

	int i = TempBufferVerticesStored;
	TempVertexBuffer[i + 0].setX(x);
	TempVertexBuffer[i + 0].setY(y);
	TempVertexBuffer[i + 0].setZ(z);
	TempVertexBuffer[i + 0].setU(TEXTURE_ADDU);
	TempVertexBuffer[i + 0].setV(TEXTURE_ADDV);
	TempVertexBuffer[i + 0].setColor(color.red, color.green, color.blue, color.alpha);

	TempVertexBuffer[i + 1].setX(x);
	TempVertexBuffer[i + 1].setY(y + WATERSECTORSZY);
	TempVertexBuffer[i + 1].setZ(z);
	TempVertexBuffer[i + 1].setU(TEXTURE_ADDU);
	TempVertexBuffer[i + 1].setV(TEXTURE_ADDV + 2.0f);
	TempVertexBuffer[i + 1].setColor(color.red, color.green, color.blue, color.alpha);

	TempVertexBuffer[i + 2].setX(x + WATERSECTORSZX);
	TempVertexBuffer[i + 2].setY(y + WATERSECTORSZY);
	TempVertexBuffer[i + 2].setZ(z);
	TempVertexBuffer[i + 2].setU(TEXTURE_ADDU + 2.0f);
	TempVertexBuffer[i + 2].setV(TEXTURE_ADDV + 2.0f);
	TempVertexBuffer[i + 2].setColor(color.red, color.green, color.blue, color.alpha);

	TempVertexBuffer[i + 3].setX(x + WATERSECTORSZX);
	TempVertexBuffer[i + 3].setY(y);
	TempVertexBuffer[i + 3].setZ(z);
	TempVertexBuffer[i + 3].setU(TEXTURE_ADDU + 2.0f);
	TempVertexBuffer[i + 3].setV(TEXTURE_ADDV);
	TempVertexBuffer[i + 3].setColor(color.red, color.green, color.blue, color.alpha);

	TempBufferVerticesStored += 4;
	TempIndexBuffer[TempBufferIndicesStored++] = i+0;
	TempIndexBuffer[TempBufferIndicesStored++] = i+1;
	TempIndexBuffer[TempBufferIndicesStored++] = i+2;
	TempIndexBuffer[TempBufferIndicesStored++] = i+0;
	TempIndexBuffer[TempBufferIndicesStored++] = i+2;
	TempIndexBuffer[TempBufferIndicesStored++] = i+3;
}

// The texture repeats every 32 units. Binding a raster gives it the wrap
// mode of whatever was drawn last (and librw's GL3 address state only takes
// with no raster bound), so with something clamped before it the water
// clamped too: the sea past the map's edge was one flat colour.
static void
BindWaterTexture(void)
{
	rw::SetRenderStatePtr(rw::TEXTURERASTER, nil);
	rw::SetRenderState(rw::TEXTUREADDRESS, rw::Texture::WRAP);
	rw::SetRenderStatePtr(rw::TEXTURERASTER, gpWaterTex->raster);
}

void
RenderWaterpro(void)
{
	int i, j;

	BindWaterTexture();
	rw::SetRenderState(rw::VERTEXALPHA, 1);
	rw::SetRenderState(rw::FOGENABLE, gEnableFog);

	rw::RGBA color = { 255, 255, 255, 255 };
	Timecycle::ColourSet *cs = &Timecycle::currentColours;
	if(params.water == GAME_III){
		color.red = clamp(cs->amb.red + cs->dir.red, 0.0f, 1.0f)*255.0f;
		color.green = clamp(cs->amb.green + cs->dir.green, 0.0f, 1.0f)*255.0f;
		color.blue = clamp(cs->amb.blue + cs->dir.blue, 0.0f, 1.0f)*255.0f;
	}else
		rw::convColor(&color, &cs->water);
	TEXTURE_ADDU = 0.0f;
	TEXTURE_ADDV = 0.0f;
	float x, y, z;

	rw::Sphere sph;
	// Small polys
	sph.radius = sqrt(WATERSMALLSECTORSZX*WATERSMALLSECTORSZX*2.0f)/2.0f;
	for(i = 0; i < 128; i++)
		for(j = 0; j < 128; j++){
			if(aWaterFineBlockList[i][j] & 0x80)
				continue;
			x = WATERSMALLSECTORSZX*i + WATERSTARTX;
			y = WATERSMALLSECTORSZY*j + WATERSTARTY;
			z = ms_aWaterZs[aWaterFineBlockList[i][j]];
			sph.center.x = x + WATERSMALLSECTORSZX/2;
			sph.center.y = y + WATERSMALLSECTORSZY/2;
			sph.center.z = z;
			if(TheCamera.m_rwcam->frustumTestSphere(&sph) != rw::Camera::SPHEREOUTSIDE)
				RenderOneFlatSmallWaterPoly(x, y, z, color);
		}

/*
	// Large polys
	sph.radius = sqrt(WATERSECTORSZX*WATERSECTORSZX*2.0f)/2.0f;
	for(i = 0; i < 64; i++)
		for(j = 0; j < 64; j++){
			if(aWaterBlockList[i][j] & 0x80)
				continue;
			x = WATERSECTORSZX*i + WATERSTARTX;
			y = WATERSECTORSZY*j + WATERSTARTY;
			z = ms_aWaterZs[aWaterBlockList[i][j]];
			sph.center.x = x + WATERSECTORSZX/2;
			sph.center.y = y + WATERSECTORSZY/2;
			sph.center.z = z;
			if(Scene.camera->frustumTestSphere(&sph) != rw::Camera::SPHEREOUTSIDE)
				RenderOneFlatLargeWaterPoly(x, y, z, color);
		}
*/
	RenderAndEmptyRenderBuffer();
}

static const float uvscale = 1/32.0f;

// This is an editor-only backdrop for the larger SA-MP workspace. It is not
// added to water.dat: GTA SA's native water loader only supports coordinates
// in the smaller ±3,000 range below.
static void
RenderEditorWaterQuad(float x0, float y0, float x1, float y1, float z,
                      rw::RGBA const &color)
{
	if(TempBufferVerticesStored+4 >= TEMPBUFFERVERTSIZE ||
	   TempBufferIndicesStored+6 >= TEMPBUFFERINDEXSIZE)
		RenderAndEmptyRenderBuffer();

	int i = TempBufferVerticesStored;
	// From a corner on the texture's 32-unit grid, so it lines up with the
	// native water while the values stay small.
	float ox = floorf(x0/32.0f)*32.0f, oy = floorf(y0/32.0f)*32.0f;
	float u0 = (x0-ox)*uvscale;
	float u1 = (x1-ox)*uvscale;
	float v0 = (y0-oy)*uvscale;
	float v1 = (y1-oy)*uvscale;
	TempVertexBuffer[i + 0].setX(x0);
	TempVertexBuffer[i + 0].setY(y0);
	TempVertexBuffer[i + 0].setZ(z);
	TempVertexBuffer[i + 0].setU(u0);
	TempVertexBuffer[i + 0].setV(v0);
	TempVertexBuffer[i + 0].setColor(color.red, color.green, color.blue, color.alpha);
	TempVertexBuffer[i + 1].setX(x0);
	TempVertexBuffer[i + 1].setY(y1);
	TempVertexBuffer[i + 1].setZ(z);
	TempVertexBuffer[i + 1].setU(u0);
	TempVertexBuffer[i + 1].setV(v1);
	TempVertexBuffer[i + 1].setColor(color.red, color.green, color.blue, color.alpha);
	TempVertexBuffer[i + 2].setX(x1);
	TempVertexBuffer[i + 2].setY(y1);
	TempVertexBuffer[i + 2].setZ(z);
	TempVertexBuffer[i + 2].setU(u1);
	TempVertexBuffer[i + 2].setV(v1);
	TempVertexBuffer[i + 2].setColor(color.red, color.green, color.blue, color.alpha);
	TempVertexBuffer[i + 3].setX(x1);
	TempVertexBuffer[i + 3].setY(y0);
	TempVertexBuffer[i + 3].setZ(z);
	TempVertexBuffer[i + 3].setU(u1);
	TempVertexBuffer[i + 3].setV(v0);
	TempVertexBuffer[i + 3].setColor(color.red, color.green, color.blue, color.alpha);

	// The corners go round the quad (unlike water.dat's grid order), so
	// split it along the 0-2 diagonal.
	TempBufferVerticesStored += 4;
	TempIndexBuffer[TempBufferIndicesStored++] = i+0;
	TempIndexBuffer[TempBufferIndicesStored++] = i+1;
	TempIndexBuffer[TempBufferIndicesStored++] = i+2;
	TempIndexBuffer[TempBufferIndicesStored++] = i+0;
	TempIndexBuffer[TempBufferIndicesStored++] = i+2;
	TempIndexBuffer[TempBufferIndicesStored++] = i+3;
}

// z on the triangle a b c at x, y, when x, y is inside it seen from above.
static bool
heightOnTriangle(const rw::V3d &a, const rw::V3d &b, const rw::V3d &c, float x, float y, float *z)
{
	float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
	if(fabsf(area) < 1e-6f)
		return false;
	float w0 = ((b.x - x) * (c.y - y) - (c.x - x) * (b.y - y)) / area;
	float w1 = ((c.x - x) * (a.y - y) - (a.x - x) * (c.y - y)) / area;
	float w2 = 1.0f - w0 - w1;
	if(w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
		return false;
	*z = w0 * a.z + w1 * b.z + w2 * c.z;
	return true;
}

bool
GetWaterHeight(float x, float y, float *z)
{
	if(params.water != GAME_SA)
		return false;
	// water.dat's quads are v0 v1 along one edge, v2 v3 along the other.
	for(int i = 0; i < numWaterQuads; i++){
		const int *n = waterQuads[i].indices;
		const rw::V3d &a = waterVertices[n[0]].pos, &b = waterVertices[n[1]].pos;
		const rw::V3d &c = waterVertices[n[2]].pos, &d = waterVertices[n[3]].pos;
		if(heightOnTriangle(a, b, c, x, y, z) || heightOnTriangle(b, d, c, x, y, z))
			return true;
	}
	for(int i = 0; i < numWaterTris; i++){
		const int *n = waterTris[i].indices;
		if(heightOnTriangle(waterVertices[n[0]].pos, waterVertices[n[1]].pos, waterVertices[n[2]].pos, x, y, z))
			return true;
	}
	// Beyond water.dat's area the sea goes on, at sea level.
	if(fabsf(x) >= SA_GAME_WATER_BOUND || fabsf(y) >= SA_GAME_WATER_BOUND){
		*z = 0.0f;
		return true;
	}
	return false;
}

// Part of the sea past the map, as tiles in reach of the camera. Fog is
// worked out at the corners, so one quad thousands of units wide took the fog
// of its far corners across all of it, the sky's colour; tiles the size of
// water.dat's keep it to the distance.
static void
RenderEditorWaterRect(float x0, float y0, float x1, float y1, float z, rw::RGBA const &color)
{
	const float tile = 250.0f;
	const rw::V3d &cam = TheCamera.m_position;
	float reach = TheCamera.m_rwcam->farPlane + tile;
	float ax = std::max(x0, cam.x - reach), bx = std::min(x1, cam.x + reach);
	float ay = std::max(y0, cam.y - reach), by = std::min(y1, cam.y + reach);
	if(ax >= bx || ay >= by)
		return;
	// The grid is fixed to the world, so tiles do not swim as the camera moves.
	ax = std::max(x0, x0 + floorf((ax - x0) / tile) * tile);
	ay = std::max(y0, y0 + floorf((ay - y0) / tile) * tile);
	rw::Sphere sph;
	sph.radius = tile * 0.7072f;
	for(float x = ax; x < bx; x += tile)
		for(float y = ay; y < by; y += tile){
			float tx = std::min(x + tile, x1), ty = std::min(y + tile, y1);
			sph.center.x = (x + tx) * 0.5f;
			sph.center.y = (y + ty) * 0.5f;
			sph.center.z = z;
			if(TheCamera.m_rwcam->frustumTestSphere(&sph) != rw::Camera::SPHEREOUTSIDE)
				RenderEditorWaterQuad(x, y, tx, ty, z, color);
		}
}

static void
RenderEditorWaterExtension(void)
{
	if(params.water != GAME_SA || SA_MP_WORLD_BOUND <= SA_GAME_WATER_BOUND)
		return;

	rw::RGBA color;
	rw::convColor(&color, &Timecycle::currentColours.water);
	const float outer = (float)SA_MP_WORLD_BOUND;
	const float inner = (float)SA_GAME_WATER_BOUND;
	// Sea level, like the native water. The ring starts where water.dat has
	// to stop, so the two never overlap and cannot z-fight.
	const float z = 0.0f;

	// Four sides and four corners form a ring around the native ±3,000 area.
	RenderEditorWaterRect(-outer, -inner, -inner, inner, z, color);
	RenderEditorWaterRect(inner, -inner, outer, inner, z, color);
	RenderEditorWaterRect(-inner, -outer, inner, -inner, z, color);
	RenderEditorWaterRect(-inner, inner, inner, outer, z, color);
	RenderEditorWaterRect(-outer, -outer, -inner, -inner, z, color);
	RenderEditorWaterRect(inner, -outer, outer, -inner, z, color);
	RenderEditorWaterRect(-outer, inner, -inner, outer, z, color);
	RenderEditorWaterRect(inner, inner, outer, outer, z, color);
}

// water.dat's quads are v0 v1 along one edge and v2 v3 along the other, some
// of them thousands of units long. Fog is worked out at the corners, so one
// that long took the fog of its far ends across all of it: a light strip
// across the sea (the bay west of the Gant Bridge). Drawn as cells of about
// WATER_CELL, it keeps to the distance.
static const float WATER_CELL = 125.0f;

static void
RenderWaterQuad(WaterQuad *q)
{
	rw::RGBA color;
	rw::convColor(&color, &Timecycle::currentColours.water);

	const rw::V3d &p0 = waterVertices[q->indices[0]].pos, &p1 = waterVertices[q->indices[1]].pos;
	const rw::V3d &p2 = waterVertices[q->indices[2]].pos, &p3 = waterVertices[q->indices[3]].pos;
	float lenS = std::max(length(sub(p1, p0)), length(sub(p3, p2)));
	float lenT = std::max(length(sub(p2, p0)), length(sub(p3, p1)));
	int ns = clamp((int)ceilf(lenS / WATER_CELL), 1, 64);
	int nt = clamp((int)ceilf(lenT / WATER_CELL), 1, 64);
	auto at = [&](int i, int j){
		float s = (float)i / ns, t = (float)j / nt;
		rw::V3d a = add(p0, scale(sub(p1, p0), s));
		rw::V3d b = add(p2, scale(sub(p3, p2), s));
		return add(a, scale(sub(b, a), t));
	};
	for(int j = 0; j < nt; j++){
		if(TempBufferVerticesStored + 2*(ns+1) >= TEMPBUFFERVERTSIZE ||
		   TempBufferIndicesStored + 6*ns >= TEMPBUFFERINDEXSIZE)
			RenderAndEmptyRenderBuffer();
		int base = TempBufferVerticesStored;
		for(int row = 0; row < 2; row++)
			for(int i = 0; i <= ns; i++){
				rw::V3d v = at(i, j + row);
				int k = TempBufferVerticesStored++;
				TempVertexBuffer[k].setX(v.x);
				TempVertexBuffer[k].setY(v.y);
				TempVertexBuffer[k].setZ(v.z);
				TempVertexBuffer[k].setU(v.x*uvscale);
				TempVertexBuffer[k].setV(v.y*uvscale);
				TempVertexBuffer[k].setColor(color.red, color.green, color.blue, color.alpha);
			}
		for(int i = 0; i < ns; i++){
			int a = base + i, b = base + i + 1, c = base + ns + 1 + i, d = c + 1;
			TempIndexBuffer[TempBufferIndicesStored++] = a;
			TempIndexBuffer[TempBufferIndicesStored++] = b;
			TempIndexBuffer[TempBufferIndicesStored++] = c;
			TempIndexBuffer[TempBufferIndicesStored++] = c;
			TempIndexBuffer[TempBufferIndicesStored++] = b;
			TempIndexBuffer[TempBufferIndicesStored++] = d;
		}
	}
}

static void
RenderWaterTri(WaterTri *t)
{
	if(TempBufferVerticesStored+3 >= TEMPBUFFERVERTSIZE ||
	   TempBufferIndicesStored+3 >= TEMPBUFFERINDEXSIZE)
		RenderAndEmptyRenderBuffer();

	rw::RGBA color;

	rw::convColor(&color, &Timecycle::currentColours.water);

	WaterVertex *v;
	int i = TempBufferVerticesStored;
	int j;
	for(j = 0; j < 3; j++){
		v = &waterVertices[t->indices[j]];
		TempVertexBuffer[i + j].setX(v->pos.x);
		TempVertexBuffer[i + j].setY(v->pos.y);
		TempVertexBuffer[i + j].setZ(v->pos.z);
		TempVertexBuffer[i + j].setU(v->pos.x*uvscale);
		TempVertexBuffer[i + j].setV(v->pos.y*uvscale);
		TempVertexBuffer[i + j].setColor(color.red, color.green, color.blue, color.alpha);
	}

	TempBufferVerticesStored += 3;
	TempIndexBuffer[TempBufferIndicesStored++] = i+0;
	TempIndexBuffer[TempBufferIndicesStored++] = i+1;
	TempIndexBuffer[TempBufferIndicesStored++] = i+2;
}

void
RenderWater(void)
{
	int i;

	BindWaterTexture();
	rw::SetRenderState(rw::VERTEXALPHA, 1);
	rw::SetRenderState(rw::FOGENABLE, gEnableFog);

	RenderEditorWaterExtension();

	for(i = 0; i < numWaterQuads; i++)
		RenderWaterQuad(&waterQuads[i]);
	for(i = 0; i < numWaterTris; i++)
		RenderWaterTri(&waterTris[i]);
	RenderAndEmptyRenderBuffer();
}

void
Render(void)
{
	SetRenderState(rw::CULLMODE, rw::CULLNONE);
	if(params.water == GAME_SA)
		RenderWater();
	else
		RenderWaterpro();
}

}
