#include "haumea.h"
#include "particles.h"
#include "coordinatepin.h"
#include "layoutpaths.h"
#include "alignmentmath.h"
#include "i18n.h"
#include "crashreports.h"
#include "imgui/imgui_internal.h"
#include "telemetry.h"
#include "modelmeta.h"
#include "editoricons.h"
#include "trains.h"
#include "anim.h"
#include "npcpreview.h"
#include "testserver.h"
#include "maptools.h"
#include "areas.h"
#include "samp_loader.h"
#include "agentbridge.h"
#include "multiplayer.h"
#include "modloader.h"
#include "workspace.h"
#include "rec.h"
#include "shortcuts.h"
#include "playerview.h"
#include "version.h"
#include "versionutil.h"
#ifdef LIBRW_GLFW
#include <GLFW/glfw3.h>
#endif
#include <chrono>
#include <thread>
#include <limits.h>
#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

int gameversion;
int gameplatform;

Params params;
// The editor produces open.mp Pawn code; it does not write the original game
// installation. Keep the legacy destination state on the safe override side
// for the separate water/custom-asset helpers that still use it.
SaveDestination gSaveDestination = SAVE_DESTINATION_MODLOADER;

int gGizmoMode = GIZMO_TRANSLATE;
int gGizmoSpace = GIZMO_WORLD;
int gGizmoPivot = GIZMO_PIVOT_ACTIVE;
rw::V3d gGizmoCustomPivot = {0,0,0};
bool gGizmoEnabled = true;
bool gGizmoHovered = false;
bool gGizmoUsing = false;
bool gPlaceSnapToObjects = true;
bool gPlaceSnapToGround = true;
bool gDragFollowGround = false;
bool gDragAlignToSurface = false;

// Brush tool
bool gBrushMode = false;
float gBrushZOffset = 0.0f;
bool gBrushAlignToSurface = false;
float gBrushYawMin = 0.0f;
float gBrushYawMax = 0.0f;
float gBrushSpacing = 2.0f;
float gBrushRadius = 8.0f;
int gBrushCount = 1;
float gBrushDelayMs = 800.0f;
static const int BRUSH_MAX_PER_BURST = 128;
bool gGizmoSnap = false;
float gGizmoSnapAngle = 15.0f;
float gGizmoSnapTranslate = 1.0f;
bool gObjectSnapMode = false;
float gObjectSnapDistance = 0.5f;

// Rect-select (marquee selection)
bool gRectSelectActive = false;
static bool sRectSelectDragging = false;
static float sRectStartX, sRectStartY;
static const float RECT_SELECT_THRESHOLD_SQ = 25.0f;	// 5px squared

int gameTxdSlot;

int currentHour = 12;
int currentMinute = 0;
int extraColours = -1;
int currentArea;

// Options

bool gRenderOnlyLod;
bool gRenderOnlyHD;
bool gRenderBackground = true;
bool gRenderWater = true;
bool gRenderPostFX = true;
bool gEnableFog = true;
int gFogMode = FOG_LIGHT;
float gStreamDistance = 1000.0f;
bool gVSync = false;
int gFrameLimit = 0;
bool gFocusMyMap;

bool gEnableTimecycleBoxes = true;
bool gUseBlurAmb = gRenderPostFX;
bool gOverrideBlurAmb = true;
bool gNoTimeCull;
bool gNoAreaCull;
bool gDoBackfaceCulling;	// init from params
bool gPawnEditMode;
bool gPawnWorldDeleteMode;
bool gPlayAnimations = true;
bool gUseViewerCam;
bool gDrawTarget = false;
int gSelectionHighlightOpacity = 25;
bool gOutlineMapObjects;
bool gOutlineSelection = true;	// outline selected and hovered objects
bool gFlyAcceleration = true;
float gFlySpeed = 2.0f;
float gFlyFastMul = 2.0f;
float gFlySlowMul = 0.5f;
float gFovWheelStep = 2.0f;
int gCameraLookButton = 3;
bool gCameraLooking;

// non-rendering things
bool gRenderCollision;
bool gRenderBounds;
bool gRenderZones;
bool gRenderMapZones;
bool gRenderNavigZones;
bool gRenderInfoZones;
bool gRenderCullZones;
bool gRenderAttribZones;
bool gRenderLegacyPedPaths;
bool gRenderLegacyCarPaths;
bool gRenderSaPedPaths;
bool gRenderSaPedPathWalkers;
bool gRenderSaCarPaths;
bool gRenderSaCarPathTraffic;
bool gRenderSaAreaGrid;
bool gRenderLightEffects = true;
bool gRenderEffects;
bool gRenderTimecycleBoxes;
int gSaPedPathWalkerCount = 12;
int gSaCarPathTrafficCount = 10;
float gSaCarPathTrafficSpeedScale = 1.0f;
bool gSaCarPathTrafficFreezeRoutes = false;
bool gRenderSaCarPathParkedCars = false;
int gSaCarPathParkedCarCount = 8;
uint32 gRequestedAASamples = 1;

// SA postfx
int  gColourFilter;
bool gRadiosity;

// SA building pipe
int gBuildingPipeSwitch = PLATFORM_PS2;
float gDayNightBalance;
float gWetRoadEffect;

// Neo stuff
float gNeoLightMapStrength = 0.5f;

static bool
buildIplVisibilityKey(const char *name, char *dst, size_t size)
{
	const char *base, *end, *dot;
	size_t len;

	if(dst == nil || size == 0 || name == nil || name[0] == '\0')
		return false;

	base = name;
	end = name + strlen(name);
	dot = end;
	for(const char *s = end; s > base; ){
		s--;
		if(*s == '.'){
			dot = s;
			break;
		}
	}
	len = dot - base;
	if(len == 0)
		len = end - base;
	if(len == 0)
		return false;

	if(len >= size)
		len = size - 1;
	memcpy(dst, base, len);
	dst[len] = '\0';
	return len > 0;
}

void
SetInstIplFilterKey(ObjectInst *inst, const char *sceneName)
{
	char key[256];

	if(inst == nil)
		return;
	if(!buildIplVisibilityKey(sceneName, key, sizeof(key)))
		key[0] = '\0';
	if(strcmp(inst->m_iplFilterKey, key) == 0)
		return;
	strncpy(inst->m_iplFilterKey, key, sizeof(inst->m_iplFilterKey)-1);
	inst->m_iplFilterKey[sizeof(inst->m_iplFilterKey)-1] = '\0';
}

bool
IsHourInRange(int h1, int h2)
{
	if(h1 > h2)
		return currentHour >= h1 || currentHour < h2;
	else
		return currentHour >= h1 && currentHour < h2;
}

static WeatherInfo weathersIII[] = {
	{ "SUNNY", Weather::Sunny },
	{ "CLOUDY", 0 },
	{ "RAINY", 0 },
	{ "FOGGY", Weather::Foggy }
};
static WeatherInfo weathersVC[] = {
	{ "SUNNY", Weather::Sunny },
	{ "CLOUDY", 0 },
	{ "RAINY", 0 },
	{ "FOGGY", Weather::Foggy },
	{ "EXTRASUNNY", Weather::Sunny | Weather::Extrasunny },
	{ "HURRICANE", 0 },
	{ "EXTRACOLOURS", 0 }
};
static WeatherInfo weathersLCS[] = {
	{ "SUNNY", Weather::Sunny },
	{ "CLOUDY", 0 },
	{ "RAINY", 0 },
	{ "FOGGY", Weather::Foggy },
	{ "EXTRASUNNY", Weather::Sunny | Weather::Extrasunny },
	{ "HURRICANE", 0 },
	{ "EXTRACOLOURS", 0 },
	{ "SNOW", 0 }
};
static WeatherInfo weathersVCS[] = {
	{ "SUNNY", Weather::Sunny },
	{ "CLOUDY", 0 },
	{ "RAINY", 0 },
	{ "FOGGY", Weather::Foggy },
	{ "EXTRASUNNY", Weather::Sunny | Weather::Extrasunny },
	{ "HURRICANE", 0 },
	{ "EXTRACOLOURS", 0 },
	{ "ULTRASUNNY", Weather::Sunny | Weather::Extrasunny }
};
static WeatherInfo weathersSA[] = {
	{ "EXTRASUNNY LA", Weather::Sunny | Weather::Extrasunny },
	{ "SUNNY LA", Weather::Sunny },
	{ "EXTRASUNNY SMOG LA", Weather::Sunny | Weather::Extrasunny },
	{ "SUNNY SMOG LA", Weather::Sunny },
	{ "CLOUDY LA", 0 },
	{ "SUNNY SF", Weather::Sunny },
	{ "EXTRASUNNY SF", Weather::Sunny | Weather::Extrasunny },
	{ "CLOUDY SF", 0 },
	{ "RAINY SF", 0 },
	{ "FOGGY SF", Weather::Foggy },
	{ "SUNNY VEGAS", Weather::Sunny },
	{ "EXTRASUNNY VEGAS", Weather::Sunny | Weather::Extrasunny },
	{ "CLOUDY VEGAS", 0 },
	{ "EXTRASUNNY COUNTRYSIDE", Weather::Sunny | Weather::Extrasunny },
	{ "SUNNY COUNTRYSIDE", Weather::Sunny },
	{ "CLOUDY COUNTRYSIDE", 0 },
	{ "RAINY COUNTRYSIDE", 0 },
	{ "EXTRASUNNY DESERT", Weather::Sunny | Weather::Extrasunny },
	{ "SUNNY DESERT", Weather::Sunny },
	{ "SANDSTORM DESERT", Weather::Foggy },
	{ "UNDERWATER", 0 },
	{ "EXTRACOLOURS 1", 0 },
	{ "EXTRACOLOURS 2", 0 }
};

void
InitParams(void)
{
	static const char *areasVC[] = {
		"Main Map", "Hotel", "Mansion", "Bank", "Mall", "Strip club",
		"Lawyer", "Coffee shop", "Concert hall", "Studio", "Rifle range",
		"Biker bar", "Police station", "Everywhere", "Dirt", "Blood", "Oval ring",
		"Malibu", "Print works"
	};

	params.initcampos.set(1356.0f, -1107.0f, 96.0f);
	params.initcamtarg.set(1276.0f, -984.0f, 68.0f);
	params.backfaceCull = true;
	params.alphaRefDefault = 2;
	params.alphaRef = 2;
	params.ps2AlphaTest = gameplatform == PLATFORM_PS2;
	params.map = gameversion;

	switch(gameversion){
	case GAME_III:
		params.initcampos.set(970.8f, -497.3f, 36.8f);
		params.initcamtarg.set(1092.5f, -417.3f, 3.8f);
		params.objFlagset = GAME_III;
		params.timecycle = GAME_III;
		params.numHours = 24;
		params.numWeathers = 4;
		params.weatherInfo = weathersIII;
		params.water = GAME_III;
		params.waterTex = "water_old";
		params.waterStart.set(-2048.0f, -2048.0f);
		params.waterEnd.set(2048.0f, 2048.0f);
		params.backfaceCull = false;
		params.checkColModels = true;
		params.maxNumColBoxes = 32;
		params.maxNumColSpheres = 128;
		params.maxNumColTriangles = 600;
		switch(gameplatform){
		case PLATFORM_PS2:
			break;
		case PLATFORM_PC:
			break;
		case PLATFORM_XBOX:
			// not so sure about the values
			// I think it's hardcoded by ID
			params.alphaRefDefault = 6;
			params.alphaRef = 128;
			params.txdFallbackGeneric = true;
			params.neoWorldPipe = GAME_III;
			break;
		}
		break;
	case GAME_VC:
		params.initcampos.set(131.5f, -1674.2f, 59.8f);
		params.initcamtarg.set(67.9f, -1542.0f, 26.3f);
		params.objFlagset = GAME_VC;
		params.numAreas = 19;
		params.areaNames = areasVC;
		params.timecycle = GAME_VC;
		params.numHours = 24;
		params.numWeathers = 7;
		params.extraColours = 6;
		params.numExtraColours = 1;
		params.weatherInfo = weathersVC;
		params.water = GAME_VC;
		params.waterTex = "waterclear256";
		params.waterStart.set(-2048.0f - 400.0f, -2048.0f);
		params.waterEnd.set(2048.0f - 400.0f, 2048.0f);
		switch(gameplatform){
		case PLATFORM_PS2:
			params.backfaceCull = false;
			break;
		case PLATFORM_PC:
			break;
		case PLATFORM_XBOX:
			// not so sure about the values
			// I think it's hardcoded by ID
			params.alphaRefDefault = 6;
			params.alphaRef = 128;
			params.neoWorldPipe = GAME_VC;
			params.backfaceCull = false;
			break;
		}
		break;
	case GAME_SA:
		params.initcampos.set(1789.0f, -1667.4f, 66.4f);
		params.initcamtarg.set(1679.1f, -1569.4f, 41.5f);
		params.objFlagset = GAME_SA;
		params.numAreas = 19;
		params.areaNames = areasVC;
		params.timecycle = GAME_SA;
		params.numHours = 8;
		params.numWeathers = 23;
		params.extraColours = 21;
		params.numExtraColours = 2;
		params.weatherInfo = weathersSA;
		params.background = GAME_SA;
		params.daynightPipe = true;
		params.water = GAME_SA;
		params.waterTex = "waterclear256";

		gBuildingPipeSwitch = gameplatform;
		gColourFilter = PLATFORM_PC;
		gRadiosity = gColourFilter == PLATFORM_PS2;
		if(gameplatform == PLATFORM_PS2){
			gColourFilter = PLATFORM_PS2;
		}else{
			params.alphaRefDefault = 2;
			params.alphaRef = 100;
		}
		break;
	case GAME_LCS:
		// TODO
		params.map = GAME_III;
		params.initcampos.set(970.8f, -497.3f, 36.8f);
		params.initcamtarg.set(1092.5f, -417.3f, 3.8f);
		params.objFlagset = GAME_VC;
		params.numAreas = 19;
		params.areaNames = areasVC;
		params.timecycle = GAME_LCS;
		params.numHours = 24;
		params.numWeathers = 8;
		params.extraColours = 6;
		params.numExtraColours = 1;
		params.weatherInfo = weathersLCS;
		params.water = GAME_III;
		params.waterTex = "waterclear256";
		params.waterStart.set(-2048.0f, -2048.0f);
		params.waterEnd.set(2048.0f, 2048.0f);
		params.backfaceCull = false;
		params.ps2AlphaTest = true;

		params.leedsPipe = 1;
		gBuildingPipeSwitch = PLATFORM_PS2;
		break;
	case GAME_VCS:
		// TODO
		params.map = GAME_VC;
		params.initcampos.set(131.5f, -1674.2f, 59.8f);
		params.initcamtarg.set(67.9f, -1542.0f, 26.3f);
		params.objFlagset = GAME_VC;
		params.numAreas = 19;
		params.areaNames = areasVC;
		params.timecycle = GAME_VCS;
		params.numHours = 24;
		params.numWeathers = 8;
		params.extraColours = 7;
		params.numExtraColours = 1;
		params.weatherInfo = weathersVCS;
		params.water = GAME_VC;
		params.waterTex = "waterclear256";
		params.waterStart.set(-2048.0f - 400.0f, -2048.0f);
		params.waterEnd.set(2048.0f - 400.0f, 2048.0f);
		params.backfaceCull = false;
		params.ps2AlphaTest = true;

		params.leedsPipe = 1;
		gBuildingPipeSwitch = PLATFORM_PS2;
		TheCamera.m_LODmult = 1.5f;
		break;
	// more configs in the future (LCSPC, VCSPC, UG, ...)
	}

	if(params.ps2AlphaTest){
		params.alphaRefDefault = 128;
		params.alphaRef = 128;
	}
}

void
FindVersion(void)
{
	FILE *f;

	if(f = fopen_ci("data/gta3.dat", "r"), f)
		gameversion = GAME_III;
	// This is wrong of course, but we'll use it as a hack
	else if(f = fopen_ci("data/gta_lcs.dat", "r"), f)
		gameversion = GAME_LCS;
	else if(f = fopen_ci("data/gta_vc.dat", "r"), f)
		gameversion = GAME_VC;
	else if(f = fopen_ci("data/gta_vcs.dat", "r"), f)
		gameversion = GAME_VCS;
	else if(f = fopen_ci("data/gta.dat", "r"), f)
		gameversion = GAME_SA;
	else{
		gameversion = GAME_NA;
		return;
	}
	if(doesFileExist("SYSTEM.CNF"))
		gameplatform = PLATFORM_PS2;
	else if(doesFileExist("default.xbe"))
		gameplatform = PLATFORM_XBOX;
	else
		gameplatform = PLATFORM_PC;
	fclose(f);
}

/*
void
test(void)
{
	CPtrNode *p;
	ObjectInst *inst, *inst2;
	int i;
	for(p = instances.first; p; p = p->next){
		inst = (ObjectInst*)p->item;

//		if(inst->m_numChildren > 1)
//			printf("%s has %d lod children\n", GetObjectDef(inst->m_objectId)->m_name, inst->m_numChildren);
		i = 0;
		for(inst2 = inst; inst2; inst2 = inst2->m_lod)
			i++;
		if(i > 2){
			printf("%s has %d lod levels\n", GetObjectDef(inst->m_objectId)->m_name, i);
			for(inst2 = inst; inst2; inst2 = inst2->m_lod)
				printf(" %s\n", GetObjectDef(inst2->m_objectId)->m_name);
		}
	}
	if(0){
		int i;
		CPtrNode *p;
		i = 0;
		for(p = instances.first; p; p = p->next)
			i++;
		log("%d instances\n", i);
	}
}*/

void
RenderEverythingColourCoded(void)
{
	rw::SetRenderState(rw::FOGENABLE, 0);
	SetRenderState(rw::ALPHATESTREF, 10);
	int aref = params.alphaRef;
	params.alphaRef = 10;
	gta::renderColourCoded = 1;
	RenderEverything();
	gta::renderColourCoded = 0;
	params.alphaRef = aref;
}

int32
pick(void)
{
	static rw::RGBA black = { 0, 0, 0, 0xFF };
	// Pick with the camera that rendered the viewport. The viewer camera shares
	// the main camera's frame buffer, so using m_rwcam directly can leave the
	// colour pass with a different projection when the viewer camera is active.
	rw::Camera *camera = Scene.camera ? Scene.camera : TheCamera.m_rwcam;
	if(!gta::BeginColourCodePass(camera, &black))
		return -1;
	RenderEverythingColourCoded();
	int32 code = gta::GetColourCode(CPad::newMouseState.x, CPad::newMouseState.y);
	gta::EndColourCodePass();
	return code;
}

static rw::V3d
GetColVertex(CColModel *col, int idx)
{
	if(col->flags & 0x80)
		return col->compVertices[idx].Uncompress();
	return col->vertices[idx];
}

bool
IntersectRaySphere(const Ray &ray, const CSphere &sphere, float *t)
{
	rw::V3d diff = sub(ray.start, sphere.center);
	float a = dot(ray.dir, ray.dir);
	float b = 2.0f * dot(ray.dir, diff);
	float c = dot(diff, diff) - sq(sphere.radius);
	float discr = sq(b) - 4.0f*a*c;
	if(discr < 0.0f)
		return false;

	float root = sqrt(discr);
	float inv2a = 0.5f / a;
	float t0 = (-b - root) * inv2a;
	float t1 = (-b + root) * inv2a;
	float hit = t0 >= 0.0f ? t0 : t1;
	if(hit < 0.0f)
		return false;

	*t = hit;
	return true;
}

static bool
IntersectRayBox(const Ray &ray, const CBox &box, float *t)
{
	float tmin = 0.0f;
	float tmax = 1.0e30f;
	float rayStart[3] = { ray.start.x, ray.start.y, ray.start.z };
	float rayDir[3] = { ray.dir.x, ray.dir.y, ray.dir.z };
	float bmin[3] = { box.min.x, box.min.y, box.min.z };
	float bmax[3] = { box.max.x, box.max.y, box.max.z };

	for(int axis = 0; axis < 3; axis++){
		if(fabs(rayDir[axis]) < 0.0001f){
			if(rayStart[axis] < bmin[axis] || rayStart[axis] > bmax[axis])
				return false;
			continue;
		}

		float invDir = 1.0f / rayDir[axis];
		float t0 = (bmin[axis] - rayStart[axis]) * invDir;
		float t1 = (bmax[axis] - rayStart[axis]) * invDir;
		if(t0 > t1){
			float tmp = t0;
			t0 = t1;
			t1 = tmp;
		}

		tmin = max(tmin, t0);
		tmax = min(tmax, t1);
		if(tmin > tmax)
			return false;
	}

	*t = tmin;
	return true;
}

static rw::V3d
GetBoxHitNormal(const Ray &ray, const CBox &box, float t)
{
	const float eps = 0.01f;
	rw::V3d hit = add(ray.start, scale(ray.dir, t));
	rw::V3d normal = { 0.0f, 0.0f, 0.0f };

	if(fabs(hit.x - box.min.x) < eps) normal.x = -1.0f;
	else if(fabs(hit.x - box.max.x) < eps) normal.x = 1.0f;
	else if(fabs(hit.y - box.min.y) < eps) normal.y = -1.0f;
	else if(fabs(hit.y - box.max.y) < eps) normal.y = 1.0f;
	else if(fabs(hit.z - box.min.z) < eps) normal.z = -1.0f;
	else if(fabs(hit.z - box.max.z) < eps) normal.z = 1.0f;

	if(normal.x == 0.0f && normal.y == 0.0f && normal.z == 0.0f){
		float dxMin = fabs(hit.x - box.min.x);
		float dxMax = fabs(hit.x - box.max.x);
		float dyMin = fabs(hit.y - box.min.y);
		float dyMax = fabs(hit.y - box.max.y);
		float dzMin = fabs(hit.z - box.min.z);
		float dzMax = fabs(hit.z - box.max.z);
		float best = dxMin;
		normal.x = -1.0f;
		if(dxMax < best){ best = dxMax; normal.x = 1.0f; normal.y = 0.0f; normal.z = 0.0f; }
		if(dyMin < best){ best = dyMin; normal.x = 0.0f; normal.y = -1.0f; normal.z = 0.0f; }
		if(dyMax < best){ best = dyMax; normal.x = 0.0f; normal.y = 1.0f; normal.z = 0.0f; }
		if(dzMin < best){ best = dzMin; normal.x = 0.0f; normal.y = 0.0f; normal.z = -1.0f; }
		if(dzMax < best){ normal.x = 0.0f; normal.y = 0.0f; normal.z = 1.0f; }
	}

	return normal;
}

bool
IntersectRayTriangle(const Ray &ray, rw::V3d a, rw::V3d b, rw::V3d c, float *t)
{
	const float eps = 0.0001f;
	rw::V3d edge1 = sub(b, a);
	rw::V3d edge2 = sub(c, a);
	rw::V3d pvec = cross(ray.dir, edge2);
	float det = dot(edge1, pvec);
	if(fabs(det) < eps)
		return false;

	float invDet = 1.0f / det;
	rw::V3d tvec = sub(ray.start, a);
	float u = dot(tvec, pvec) * invDet;
	if(u < 0.0f || u > 1.0f)
		return false;

	rw::V3d qvec = cross(tvec, edge1);
	float v = dot(ray.dir, qvec) * invDet;
	if(v < 0.0f || u + v > 1.0f)
		return false;

	float hit = dot(edge2, qvec) * invDet;
	if(hit < 0.0f)
		return false;

	*t = hit;
	return true;
}

bool
IntersectRayColModelDetailed(const Ray &worldRay, ObjectInst *inst, rw::V3d *hitPos, rw::V3d *hitNormal)
{
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil || obj->m_colModel == nil)
		return false;

	CColModel *col = obj->m_colModel;
	rw::Matrix invMat;
	rw::Matrix::invert(&invMat, &inst->m_matrix);

	Ray ray;
	ray.start = worldRay.start;
	ray.dir = worldRay.dir;
	rw::V3d::transformPoints(&ray.start, &ray.start, 1, &invMat);
	rw::V3d::transformVectors(&ray.dir, &ray.dir, 1, &invMat);
	ray.dir = normalize(ray.dir);

	float broadT;
	if(!IntersectRayBox(ray, col->boundingBox, &broadT) &&
	   !IntersectRaySphere(ray, col->boundingSphere, &broadT))
		return false;

	float bestT = 1.0e30f;
	bool found = false;
	rw::V3d bestNormal = { 0.0f, 0.0f, 1.0f };

	for(int i = 0; i < col->numTriangles; i++){
		CColTriangle *tri = &col->triangles[i];
		rw::V3d a = GetColVertex(col, tri->a);
		rw::V3d b = GetColVertex(col, tri->b);
		rw::V3d c = GetColVertex(col, tri->c);
		float t;
		if(IntersectRayTriangle(ray, a, b, c, &t) && t < bestT){
			bestT = t;
			bestNormal = normalize(cross(sub(b, a), sub(c, a)));
			found = true;
		}
	}

	for(int i = 0; i < col->numBoxes; i++){
		float t;
		if(IntersectRayBox(ray, col->boxes[i].box, &t) && t < bestT){
			bestT = t;
			bestNormal = GetBoxHitNormal(ray, col->boxes[i].box, t);
			found = true;
		}
	}

	for(int i = 0; i < col->numSpheres; i++){
		float t;
		if(IntersectRaySphere(ray, col->spheres[i].sph, &t) && t < bestT){
			rw::V3d localHit = add(ray.start, scale(ray.dir, t));
			bestT = t;
			bestNormal = normalize(sub(localHit, col->spheres[i].sph.center));
			found = true;
		}
	}

	if(!found)
		return false;

	*hitPos = add(worldRay.start, scale(worldRay.dir, bestT));
	if(hitNormal){
		*hitNormal = bestNormal;
		rw::V3d::transformVectors(hitNormal, hitNormal, 1, &inst->m_matrix);
		*hitNormal = normalize(*hitNormal);
	}
	return true;
}

bool
IntersectRayColModel(const Ray &worldRay, ObjectInst *inst, rw::V3d *hitPos)
{
	return IntersectRayColModelDetailed(worldRay, inst, hitPos, nil);
}

static bool
CanSnapToInst(ObjectInst *inst)
{
	// Nothing lands on an editor icon: its stand-in model isn't there.
	if(inst == nil || inst->m_isDeleted || inst->m_editorHidden || InstUsesEditorIcon(inst))
		return false;
	if(!gNoAreaCull && inst->m_area != currentArea && inst->m_area != 13)
		return false;
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	return obj && obj->m_colModel;
}

static bool
PointInsideInstBounds(ObjectInst *inst, float x, float y)
{
	CRect bounds = inst->GetBoundRect();
	return x >= bounds.left && x <= bounds.right &&
	       y >= bounds.bottom && y <= bounds.top;
}

bool
ModelStandsOnGround(int modelId, int elementKind)
{
	ObjectDef *obj = GetObjectDef(modelId);
	if(obj && (obj->m_isVehicle || obj->m_isPed))
		return true;
	return elementKind == SAMP_ELEMENT_ACTOR || elementKind == SAMP_ELEMENT_CLASS ||
		elementKind == SAMP_ELEMENT_NPC || elementKind == SAMP_ELEMENT_PATHPOINT;
}

bool
InstStandsOnGround(const ObjectInst *inst)
{
	return inst && ModelStandsOnGround(inst->m_objectId, GetSampElementKind(inst));
}

// Something a car or a ped can't stand on: a prop under 1.2 m across on its
// narrow side (lamp posts, signs, fences, bollards), or a car or ped itself.
static bool
IsThinProp(ObjectInst *inst)
{
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil || obj->m_colModel == nil)
		return true;
	if(obj->m_isVehicle || obj->m_isPed || GetSampElementKind(inst) != SAMP_ELEMENT_OBJECT)
		return true;
	const CBox &box = obj->m_colModel->boundingBox;
	return min(box.max.x - box.min.x, box.max.y - box.min.y) < 1.2f;
}

static void
FindGroundHitInList(CPtrList *list, const Ray &ray, float x, float y, rw::V3d *bestHit, rw::V3d *bestNormal, float *bestT, bool ignoreSelection,
	bool forStanding)
{
	for(CPtrNode *p = list->first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(forStanding && IsThinProp(inst))
			continue;
		if(ignoreSelection){
			if(inst->m_selected)
				continue;
			if(inst->m_lod && inst->m_lod->m_selected)
				continue;
			bool linkedToSelection = false;
			for(CPtrNode *sel = selection.first; sel; sel = sel->next){
				ObjectInst *selectedInst = (ObjectInst*)sel->item;
				if(selectedInst->m_lod == inst){
					linkedToSelection = true;
					break;
				}
			}
			if(linkedToSelection)
				continue;
		}
		if(!CanSnapToInst(inst))
			continue;
		if(!PointInsideInstBounds(inst, x, y))
			continue;

		rw::V3d hitPos;
		rw::V3d hitNormal;
		if(!IntersectRayColModelDetailed(ray, inst, &hitPos, &hitNormal))
			continue;

		float t = dot(sub(hitPos, ray.start), ray.dir);
		if(t >= 0.0f && t < *bestT){
			*bestT = t;
			*bestHit = hitPos;
			if(bestNormal)
				*bestNormal = hitNormal;
		}
	}
}

bool
GetGroundPlacementSurface(rw::V3d pos, rw::V3d *hitPos, rw::V3d *hitNormal, bool ignoreSelection, bool forStanding)
{
	Ray ray;
	ray.start = pos;
	ray.start.z += 5000.0f;
	ray.dir = { 0.0f, 0.0f, -1.0f };

	float bestT = 1.0e30f;
	bool found = false;
	rw::V3d bestHit = pos;
	rw::V3d bestNormal = { 0.0f, 0.0f, 1.0f };

	if(pos.x >= worldBounds.left && pos.x < worldBounds.right &&
	   pos.y >= worldBounds.bottom && pos.y < worldBounds.top){
		Sector *s = GetSector(GetSectorIndexX(pos.x), GetSectorIndexY(pos.y));
		FindGroundHitInList(&s->buildings, ray, pos.x, pos.y, &bestHit, &bestNormal, &bestT, ignoreSelection, forStanding);
		FindGroundHitInList(&s->buildings_overlap, ray, pos.x, pos.y, &bestHit, &bestNormal, &bestT, ignoreSelection, forStanding);
		FindGroundHitInList(&s->bigbuildings, ray, pos.x, pos.y, &bestHit, &bestNormal, &bestT, ignoreSelection, forStanding);
		FindGroundHitInList(&s->bigbuildings_overlap, ray, pos.x, pos.y, &bestHit, &bestNormal, &bestT, ignoreSelection, forStanding);
	}

	FindGroundHitInList(&outOfBoundsSector.buildings, ray, pos.x, pos.y, &bestHit, &bestNormal, &bestT, ignoreSelection, forStanding);
	FindGroundHitInList(&outOfBoundsSector.bigbuildings, ray, pos.x, pos.y, &bestHit, &bestNormal, &bestT, ignoreSelection, forStanding);

	found = bestT < 1.0e30f;
	if(found){
		*hitPos = bestHit;
		if(hitNormal){
			if(bestNormal.z < 0.0f)
				bestNormal = scale(bestNormal, -1.0f);
			*hitNormal = bestNormal;
		}
	}
	return found;
}

float
GetPlacementBaseOffset(int objectId)
{
	ObjectDef *obj = GetObjectDef(objectId);
	if(obj == nil || obj->m_colModel == nil)
		return 0.0f;
	return -obj->m_colModel->boundingBox.min.z;
}

static void updateRwFrame(ObjectInst *inst);

rw::Quat
QuatFromMatrix(const rw::Matrix &matrix)
{
	rw::Quat q;
	float trace = matrix.right.x + matrix.up.y + matrix.at.z;
	if(trace > 0.0f){
		float s = sqrtf(trace + 1.0f) * 2.0f;
		q.w = 0.25f * s;
		q.x = (matrix.up.z - matrix.at.y) / s;
		q.y = (matrix.at.x - matrix.right.z) / s;
		q.z = (matrix.right.y - matrix.up.x) / s;
	}else if(matrix.right.x > matrix.up.y && matrix.right.x > matrix.at.z){
		float s = sqrtf(1.0f + matrix.right.x - matrix.up.y - matrix.at.z) * 2.0f;
		q.w = (matrix.up.z - matrix.at.y) / s;
		q.x = 0.25f * s;
		q.y = (matrix.up.x + matrix.right.y) / s;
		q.z = (matrix.at.x + matrix.right.z) / s;
	}else if(matrix.up.y > matrix.at.z){
		float s = sqrtf(1.0f + matrix.up.y - matrix.right.x - matrix.at.z) * 2.0f;
		q.w = (matrix.at.x - matrix.right.z) / s;
		q.x = (matrix.up.x + matrix.right.y) / s;
		q.y = 0.25f * s;
		q.z = (matrix.at.y + matrix.up.z) / s;
	}else{
		float s = sqrtf(1.0f + matrix.at.z - matrix.right.x - matrix.up.y) * 2.0f;
		q.w = (matrix.right.y - matrix.up.x) / s;
		q.x = (matrix.at.x + matrix.right.z) / s;
		q.y = (matrix.at.y + matrix.up.z) / s;
		q.z = 0.25f * s;
	}
	q.x = -q.x;
	q.y = -q.y;
	q.z = -q.z;
	float lenSq = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
	if(lenSq < 0.00000001f)
		return { 0.0f, 0.0f, 0.0f, 1.0f };
	float invLen = 1.0f / sqrtf(lenSq);
	q.x *= invLen;
	q.y *= invLen;
	q.z *= invLen;
	q.w *= invLen;
	return q;
}

static rw::Quat
NormalizeQuatOrIdentity(const rw::Quat &q)
{
	float lenSq = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
	if(lenSq < 0.00000001f)
		return { 0.0f, 0.0f, 0.0f, 1.0f };
	float invLen = 1.0f / sqrtf(lenSq);
	return { q.x * invLen, q.y * invLen, q.z * invLen, q.w * invLen };
}

static rw::V3d
NormalizeOr(const rw::V3d &v, const rw::V3d &fallback)
{
	float len = length(v);
	if(len < 0.0001f)
		return fallback;
	return scale(v, 1.0f / len);
}

void
ComputeGizmoRotation(const rw::V3d &position, const rw::Quat &rotation,
	const rw::Quat &leaderBefore, const rw::Quat &leaderAfter, const rw::V3d &pivot,
	bool individual, bool local, rw::V3d *newPosition, rw::Quat *newRotation)
{
	rw::Quat delta = NormalizeQuatOrIdentity(rw::mult(rw::conj(leaderBefore), leaderAfter));
	*newPosition = individual ? position : add(pivot, rw::rotate(sub(position, pivot), rw::conj(delta)));
	if(individual && local){
		rw::Quat localDelta = rw::mult(leaderAfter, rw::conj(leaderBefore));
		*newRotation = NormalizeQuatOrIdentity(rw::mult(localDelta, rotation));
	}else
		*newRotation = NormalizeQuatOrIdentity(rw::mult(rotation, delta));
}

static float
GetMinZOffsetForRotation(ObjectInst *inst, const rw::Quat &rotation)
{
	// An editor icon's element sits at its position, on the surface.
	if(InstUsesEditorIcon(inst))
		return 0.0f;
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil || obj->m_colModel == nil)
		return 0.0f;

	CBox box = obj->m_colModel->boundingBox;
	rw::Matrix rotMat;
	rotMat.rotate(conj(rotation), rw::COMBINEREPLACE);
	rotMat.pos.x = 0.0f;
	rotMat.pos.y = 0.0f;
	rotMat.pos.z = 0.0f;

	rw::V3d corners[8] = {
		{ box.min.x, box.min.y, box.min.z },
		{ box.min.x, box.min.y, box.max.z },
		{ box.min.x, box.max.y, box.min.z },
		{ box.min.x, box.max.y, box.max.z },
		{ box.max.x, box.min.y, box.min.z },
		{ box.max.x, box.min.y, box.max.z },
		{ box.max.x, box.max.y, box.min.z },
		{ box.max.x, box.max.y, box.max.z },
	};

	rw::V3d::transformPoints(corners, corners, 8, &rotMat);
	float minZ = corners[0].z;
	for(int i = 1; i < 8; i++)
		minZ = min(minZ, corners[i].z);
	return minZ;
}

rw::Quat
BuildGroundAlignedRotationFromRotation(const rw::Quat &sourceRotation, rw::V3d groundNormal)
{
	rw::V3d fallbackAt = { 0.0f, 0.0f, 1.0f };
	rw::V3d fallbackRight = { 1.0f, 0.0f, 0.0f };
	rw::V3d fallbackForward = { 0.0f, 1.0f, 0.0f };
	rw::Matrix sourceMatrix;
	sourceMatrix.rotate(conj(sourceRotation), rw::COMBINEREPLACE);

	groundNormal = NormalizeOr(groundNormal, fallbackAt);
	rw::V3d currentVertical = NormalizeOr(sourceMatrix.at, fallbackAt);
	float d = clamp(dot(currentVertical, groundNormal), -1.0f, 1.0f);
	const float pi = 3.14159265358979323846f;

	rw::Quat swing = { 0.0f, 0.0f, 0.0f, 1.0f };
	if(d < 0.9999f){
		rw::V3d axis;
		if(d > -0.9999f)
			axis = NormalizeOr(cross(currentVertical, groundNormal), fallbackRight);
		else{
			axis = sub(sourceMatrix.up, scale(currentVertical, dot(sourceMatrix.up, currentVertical)));
			if(length(axis) < 0.0001f)
				axis = sub(sourceMatrix.right, scale(currentVertical, dot(sourceMatrix.right, currentVertical)));
			axis = NormalizeOr(axis, fallbackRight);
		}
		float angle = d > -0.9999f ? acosf(d) : pi;
		swing = rw::Quat::rotation(angle, axis);
	}

	rw::V3d forward = rotate(sourceMatrix.up, swing);
	forward = sub(forward, scale(groundNormal, dot(forward, groundNormal)));
	if(length(forward) < 0.0001f)
		forward = rotate(sourceMatrix.right, swing);
	forward = sub(forward, scale(groundNormal, dot(forward, groundNormal)));
	forward = NormalizeOr(forward, fallbackForward);

	rw::V3d right = NormalizeOr(cross(forward, groundNormal), fallbackRight);
	forward = NormalizeOr(cross(groundNormal, right), forward);

	rw::Matrix matrix;
	matrix.right = right;
	matrix.up = forward;
	matrix.at = groundNormal;
	matrix.pos.x = 0.0f;
	matrix.pos.y = 0.0f;
	matrix.pos.z = 0.0f;
	return QuatFromMatrix(matrix);
}

static rw::Quat
BuildGroundAlignedRotation(ObjectInst *inst, rw::V3d groundNormal)
{
	return BuildGroundAlignedRotationFromRotation(inst->m_rotation, groundNormal);
}

static int
FindOrAddTransform(std::vector<UndoTransform> &transforms, ObjectInst *inst)
{
	for(int i = 0; i < (int)transforms.size(); i++)
		if(transforms[i].inst == inst)
			return i;
	if((int)transforms.size() >= MAX_BATCH_OBJECTS)
		return -1;
	UndoTransform t = {};
	t.inst = inst;
	t.oldDirty = inst->m_isDirty;
	t.oldPos = inst->m_translation;
	t.newPos = inst->m_translation;
	t.oldRot = inst->m_rotation;
	t.newRot = inst->m_rotation;
	transforms.push_back(t);
	return (int)transforms.size() - 1;
}

static bool
HasSelectedHdChild(ObjectInst *lodInst)
{
	if(lodInst == nil)
		return false;
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!inst->m_isDeleted && inst->m_lod == lodInst)
			return true;
	}
	return false;
}

static int
CountSelectedHdChildren(ObjectInst *lodInst)
{
	int count = 0;
	if(lodInst == nil)
		return 0;
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!inst->m_isDeleted && inst->m_lod == lodInst)
			count++;
	}
	return count;
}

static void
ApplyTransform(UndoTransform &t)
{
	ObjectInst *inst = t.inst;
	bool refreshSectors = (t.flags & (UNDO_TRANSFORM_POS | UNDO_TRANSFORM_ROT)) != 0;
	if(refreshSectors)
		RemoveInstFromSectors(inst);
	if(t.flags & UNDO_TRANSFORM_POS)
		inst->m_translation = t.newPos;
	if(t.flags & UNDO_TRANSFORM_ROT)
		inst->m_rotation = t.newRot;
	inst->m_isDirty = true;
	StampChangeSeq(inst);
	inst->UpdateMatrix();
	updateRwFrame(inst);
	if(refreshSectors)
		InsertInstIntoSectors(inst);
}

static bool
FindOrAddLinkedTransformGroup(std::vector<UndoTransform> &transforms, ObjectInst *inst,
	const std::unordered_set<ObjectInst*> &eligibleLods)
{
	size_t before = transforms.size();
	if(!IsInstInIplMapDocument(inst))
		return false;
	if(FindOrAddTransform(transforms, inst) < 0)
		return false;

	if(inst->m_lod && !inst->m_lod->m_isDeleted){
		if(IsInstInIplMapDocument(inst->m_lod) &&
		   eligibleLods.find(inst->m_lod) != eligibleLods.end() &&
		   FindOrAddTransform(transforms, inst->m_lod) < 0){
			transforms.resize(before);
			return false;
		}
	}else{
		for(CPtrNode *q = instances.first; q; q = q->next){
			ObjectInst *child = (ObjectInst*)q->item;
			if(child != inst && child->m_lod == inst && !child->m_isDeleted &&
			   IsInstInIplMapDocument(child)){
				if(FindOrAddTransform(transforms, child) < 0){
					transforms.resize(before);
					return false;
				}
			}
		}
	}
	return true;
}

// SA-MP object rotations are Euler angles in degrees applied as
// R = Rz * Rx * Ry (Y first, then X, then Z). This is the basis samp.dll
// builds in SetObjectRuntimeRotationDegreesAndBasis (0x100A8340); keep the
// panel, Pawn import/export and the agent on the same convention.
rw::V3d
GetObjectRotationDegrees(const rw::Quat &rotation)
{
	rw::Matrix matrix;
	matrix.rotate(conj(NormalizeQuatOrIdentity(rotation)), rw::COMBINEREPLACE);
	const float toDegrees = 180.0f / 3.14159265358979323846f;
	float sinX = clamp(matrix.up.z, -1.0f, 1.0f);
	rw::V3d degrees;
	degrees.x = asinf(sinX) * toDegrees;
	if(fabsf(sinX) < 0.99999f){
		degrees.y = atan2f(-matrix.right.z, matrix.at.z) * toDegrees;
		degrees.z = atan2f(-matrix.up.x, matrix.up.y) * toDegrees;
	}else{
		// Gimbal lock: Y and Z rotate about the same axis; fold it into Z.
		degrees.y = 0.0f;
		degrees.z = atan2f(matrix.right.y, matrix.right.x) * toDegrees;
	}
	return degrees;
}

rw::Quat
MakeObjectRotationDegrees(const rw::V3d &degrees)
{
	const float toRadians = 3.14159265358979323846f / 180.0f;
	float sx = sinf(degrees.x*toRadians), cx = cosf(degrees.x*toRadians);
	float sy = sinf(degrees.y*toRadians), cy = cosf(degrees.y*toRadians);
	float sz = sinf(degrees.z*toRadians), cz = cosf(degrees.z*toRadians);
	rw::Matrix matrix;
	matrix.setIdentity();
	matrix.right = { cz*cy - sz*sx*sy, cz*sx*sy + sz*cy, -sy*cx };
	matrix.up = { -sz*cx, cz*cx, sx };
	matrix.at = { sz*sx*cy + cz*sy, sz*sy - cz*sx*cy, cy*cx };
	return NormalizeQuatOrIdentity(QuatFromMatrix(matrix));
}

rw::Quat
ApplyObjectRotationDelta(const rw::Quat &startRotation, const rw::V3d &degrees,
	bool worldSpace)
{
	rw::Quat start = NormalizeQuatOrIdentity(startRotation);
	rw::Quat delta = MakeObjectRotationDegrees(degrees);
	// Object rotations are stored conjugated relative to their rendered
	// orientation, so world-space and local-space composition are reversed.
	return NormalizeQuatOrIdentity(worldSpace ? rw::mult(start, delta) : rw::mult(delta, start));
}

static bool
TransformRotationsDiffer(const rw::Quat &a, const rw::Quat &b)
{
	rw::Quat na = NormalizeQuatOrIdentity(a);
	rw::Quat nb = NormalizeQuatOrIdentity(b);
	float dot = na.x*nb.x + na.y*nb.y + na.z*nb.z + na.w*nb.w;
	if(dot < 0.0f){
		nb.x = -nb.x;
		nb.y = -nb.y;
		nb.z = -nb.z;
		nb.w = -nb.w;
	}
	const float epsilon = 0.0000001f;
	return fabsf(na.x - nb.x) > epsilon || fabsf(na.y - nb.y) > epsilon ||
	       fabsf(na.z - nb.z) > epsilon || fabsf(na.w - nb.w) > epsilon;
}

bool
CaptureObjectTransformTargets(ObjectInst *leader, bool includeSelection,
	std::vector<UndoTransform> &transforms, bool *capped)
{
	transforms.clear();
	if(capped)
		*capped = false;
	if(leader == nil || leader->m_isDeleted || !IsInstInIplMapDocument(leader))
		return false;

	std::vector<ObjectInst*> directTargets;
	std::unordered_set<ObjectInst*> directTargetSet;
	directTargets.push_back(leader);
	directTargetSet.insert(leader);
	if(includeSelection){
		for(CPtrNode *p = selection.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(inst->m_isDeleted || directTargetSet.find(inst) != directTargetSet.end())
				continue;
			directTargets.push_back(inst);
			directTargetSet.insert(inst);
		}
	}

	// A shared LOD follows its HD children only if every live child is a direct
	// target. Moving just one child must not displace the LOD for its siblings.
	std::unordered_map<ObjectInst*, std::pair<int, int> > lodChildCounts;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *child = (ObjectInst*)p->item;
		if(child->m_isDeleted || child->m_lod == nil || child->m_lod->m_isDeleted ||
		   !IsInstInIplMapDocument(child) || !IsInstInIplMapDocument(child->m_lod))
			continue;
		std::pair<int, int> &counts = lodChildCounts[child->m_lod];
		counts.first++;
		if(directTargetSet.find(child) != directTargetSet.end())
			counts.second++;
	}
	std::unordered_set<ObjectInst*> eligibleLods;
	for(std::unordered_map<ObjectInst*, std::pair<int, int> >::const_iterator it = lodChildCounts.begin();
	    it != lodChildCounts.end(); ++it)
		if(it->second.first > 0 && it->second.first == it->second.second)
			eligibleLods.insert(it->first);

	bool overflow = false;
	for(int i = 0; i < (int)directTargets.size(); i++){
		if(!FindOrAddLinkedTransformGroup(transforms, directTargets[i], eligibleLods)){
			overflow = true;
			if((int)transforms.size() >= MAX_BATCH_OBJECTS)
				break;
		}
	}

	// The cap can stop capture after an indirectly linked shared LOD was added.
	// Keep that LOD only when every live child made it into the final snapshot.
	std::unordered_set<ObjectInst*> capturedSet;
	for(int i = 0; i < (int)transforms.size(); i++)
		capturedSet.insert(transforms[i].inst);
	std::unordered_map<ObjectInst*, int> capturedChildCounts;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *child = (ObjectInst*)p->item;
		if(!child->m_isDeleted && child->m_lod &&
		   capturedSet.find(child) != capturedSet.end())
			capturedChildCounts[child->m_lod]++;
	}
	for(int i = (int)transforms.size() - 1; i >= 0; i--){
		ObjectInst *possibleLod = transforms[i].inst;
		if(directTargetSet.find(possibleLod) != directTargetSet.end())
			continue;
		std::unordered_map<ObjectInst*, std::pair<int, int> >::const_iterator live =
			lodChildCounts.find(possibleLod);
		if(live != lodChildCounts.end() &&
		   capturedChildCounts[possibleLod] != live->second.first){
			capturedSet.erase(possibleLod);
			transforms.erase(transforms.begin() + i);
		}
	}
	if(capped)
		*capped = overflow;
	return !transforms.empty();
}

void
PreviewObjectTransformTargets(ObjectInst *leader,
	std::vector<UndoTransform> &transforms, const rw::V3d &leaderPos,
	const rw::Quat &leaderRot, uint8 requestedFlags)
{
	UndoTransform *leaderStart = nil;
	for(int i = 0; i < (int)transforms.size(); i++)
		if(transforms[i].inst == leader){
			leaderStart = &transforms[i];
			break;
		}
	if(leaderStart == nil)
		return;

	rw::V3d moveDelta = sub(leaderPos, leaderStart->oldPos);
	rw::Quat normalizedLeaderRot = NormalizeQuatOrIdentity(leaderRot);
	rw::Quat deltaQ = NormalizeQuatOrIdentity(
		rw::mult(rw::conj(NormalizeQuatOrIdentity(leaderStart->oldRot)), normalizedLeaderRot));
	rw::Quat worldQ = rw::conj(deltaQ);

	for(int i = 0; i < (int)transforms.size(); i++){
		UndoTransform &t = transforms[i];
		rw::V3d newPos = t.oldPos;
		rw::Quat newRot = t.oldRot;
		if(requestedFlags & UNDO_TRANSFORM_ROT){
			rw::V3d offset = sub(t.oldPos, leaderStart->oldPos);
			newPos = add(leaderStart->oldPos, rw::rotate(offset, worldQ));
			newRot = NormalizeQuatOrIdentity(rw::mult(t.oldRot, deltaQ));
		}
		if(requestedFlags & UNDO_TRANSFORM_POS)
			newPos = add(newPos, moveDelta);

		t.inst->m_translation = newPos;
		t.inst->m_rotation = newRot;
		t.inst->m_isDirty = true;
		t.inst->UpdateMatrix();
		updateRwFrame(t.inst);

		t.newPos = newPos;
		t.newRot = newRot;
		t.flags = 0;
		if(length(sub(newPos, t.oldPos)) > 0.0000001f)
			t.flags |= UNDO_TRANSFORM_POS;
		if(TransformRotationsDiffer(newRot, t.oldRot))
			t.flags |= UNDO_TRANSFORM_ROT;
	}
}

int
CommitObjectTransformTargets(std::vector<UndoTransform> &transforms)
{
	std::vector<UndoTransform> changed;
	changed.reserve(transforms.size());
	for(int i = 0; i < (int)transforms.size(); i++){
		if(transforms[i].flags == 0){
			transforms[i].inst->m_isDirty = transforms[i].oldDirty;
			continue;
		}
		RemoveInstFromSectors(transforms[i].inst);
		StampChangeSeq(transforms[i].inst);
		InsertInstIntoSectors(transforms[i].inst);
		changed.push_back(transforms[i]);
	}
	if(!changed.empty())
		UndoRecordTransformBatch(changed.data(), (int)changed.size());
	transforms.clear();
	return (int)changed.size();
}

int
SnapSelectedToGround(bool alignRotation)
{
	std::vector<ObjectInst*> targets;
	int skipped = 0;
	bool capped = false;
	targets.reserve(1024);

	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted)
			continue;
		if(!IsPawnObjectEditable(inst)){
			skipped++;
			continue;
		}
		if(inst->m_lod && CountSelectedHdChildren(inst->m_lod) > 1){
			skipped++;
			continue;
		}
		if(HasSelectedHdChild(inst)){
			skipped++;
			continue;
		}
		if((int)targets.size() < MAX_BATCH_OBJECTS)
			targets.push_back(inst);
		else{
			capped = true;
			break;
		}
	}
	if(capped)
		Toast(TOAST_SELECTION, _("Ground snap limited to first %d selected instance(s)"), MAX_BATCH_OBJECTS);

	if(targets.empty()){
		if(skipped > 0)
			Toast(TOAST_SELECTION, _("Ground snap skipped linked multi-selection"));
		return 0;
	}

	std::vector<UndoTransform> transforms;
	transforms.reserve(min((int)targets.size() * 2, MAX_BATCH_OBJECTS));
	int snapped = 0;

	for(int i = 0; i < (int)targets.size(); i++){
		ObjectInst *inst = targets[i];
		rw::V3d hitPos, hitNormal;
		if(!GetGroundPlacementSurface(inst->m_translation, &hitPos, &hitNormal, true, InstStandsOnGround(inst))){
			skipped++;
			continue;
		}

		rw::Quat newRot = inst->m_rotation;
		if(alignRotation)
			newRot = BuildGroundAlignedRotation(inst, hitNormal);

		rw::V3d newPos = inst->m_translation;
		newPos.z = hitPos.z - GetMinZOffsetForRotation(inst, newRot);
		rw::V3d delta = sub(newPos, inst->m_translation);

		if(length(delta) < 0.0001f && (!alignRotation || memcmp(&newRot, &inst->m_rotation, sizeof(newRot)) == 0))
			continue;

		size_t before = transforms.size();
		int selfIndex = FindOrAddTransform(transforms, inst);
		if(selfIndex < 0){
			capped = true;
			break;
		}
		UndoTransform *self = &transforms[selfIndex];
		self->flags |= UNDO_TRANSFORM_POS;
		self->newPos = newPos;
		if(alignRotation){
			self->flags |= UNDO_TRANSFORM_ROT;
			self->newRot = newRot;
		}

		if(inst->m_lod && !inst->m_lod->m_isDeleted){
			int lodIndex = FindOrAddTransform(transforms, inst->m_lod);
			if(lodIndex < 0){
				transforms.resize(before);
				capped = true;
				continue;
			}
			UndoTransform *lod = &transforms[lodIndex];
			lod->flags |= UNDO_TRANSFORM_POS;
			lod->newPos = add(inst->m_lod->m_translation, delta);
		}else{
			bool groupFits = true;
			for(CPtrNode *p = instances.first; p; p = p->next){
				ObjectInst *child = (ObjectInst*)p->item;
				if(child != inst && child->m_lod == inst && !child->m_isDeleted){
					int childIndex = FindOrAddTransform(transforms, child);
					if(childIndex < 0){
						groupFits = false;
						break;
					}
					UndoTransform *childTransform = &transforms[childIndex];
					childTransform->flags |= UNDO_TRANSFORM_POS;
					childTransform->newPos = add(child->m_translation, delta);
				}
			}
			if(!groupFits){
				transforms.resize(before);
				capped = true;
				continue;
			}
		}
		snapped++;
	}

	if(transforms.empty()){
		if(skipped > 0)
			Toast(TOAST_SELECTION, _("Ground snap skipped %d instance(s)"), skipped);
		return 0;
	}

	for(int i = 0; i < (int)transforms.size(); i++)
		ApplyTransform(transforms[i]);
	UndoRecordTransformBatch(transforms.data(), (int)transforms.size());

	if(snapped > 0){
		if(alignRotation)
			Toast(TOAST_SELECTION, _("Aligned %d instance(s) to ground"), snapped);
		else
			Toast(TOAST_SELECTION, _("Snapped %d instance(s) to ground"), snapped);
	}
	if(skipped > 0)
		Toast(TOAST_SELECTION, _("Skipped %d linked/conflicting instance(s)"), skipped);
	if(capped)
		Toast(TOAST_SELECTION, _("Snap skipped some linked instances at the %d object cap"), MAX_BATCH_OBJECTS);
	return snapped;
}

rw::V3d
GetPlacementPosition(void)
{
	rw::V3d hitPos, hitNormal;
	GetPlacementSurfaceHit(&hitPos, &hitNormal);
	hitPos.z += GetElementPlacementBaseOffset(GetSpawnElementKind(), GetSpawnObjectId());
	// A train goes on the track, as the client will put it (Alt: anywhere).
	rw::V3d onTrack;
	if(IsTrainEngineModel(GetSpawnObjectId()) && !CPad::IsAltDown() &&
	   SnapTrainToTrack(GetSpawnObjectId(), hitPos, 0.0f, &onTrack))
		hitPos = onTrack;
	return hitPos;
}

bool
GetPlacementSurfaceHit(rw::V3d *hitPos, rw::V3d *hitNormal)
{
	rw::V3d origin = TheCamera.m_position;
	rw::V3d dir = normalize(TheCamera.m_mouseDir);
	Ray ray;
	ray.start = origin;
	ray.dir = dir;

	if(gPlaceSnapToObjects){
		float bestT = 1.0e30f;
		ObjectInst *targetInst = GetVisibleInstUnderRay(ray, nil, &bestT);
		rw::V3d p, n;
		if(CanSnapToInst(targetInst) && !targetInst->m_selected && IntersectRayColModelDetailed(ray, targetInst, &p, &n)){
			if(n.z < 0.0f) n = scale(n, -1.0f);
			if(hitPos) *hitPos = p;
			if(hitNormal) *hitNormal = n;
			return true;
		}
	}

	// Intersect ray with horizontal plane at camera target height
	float planeZ = TheCamera.m_target.z;
	rw::V3d surfacePos;
	if(fabs(dir.z) < 0.001f)
		surfacePos = add(origin, scale(dir, 50.0f));
	else{
		float t = (planeZ - origin.z) / dir.z;
		if(t < 1.0f) t = 50.0f;
		if(t > 5000.0f) t = 5000.0f;

		surfacePos.x = origin.x + dir.x * t;
		surfacePos.y = origin.y + dir.y * t;
		surfacePos.z = planeZ;
	}

	if(gPlaceSnapToGround){
		rw::V3d groundHit, groundNormal = { 0.0f, 0.0f, 1.0f };
		if(GetGroundPlacementSurface(surfacePos, &groundHit, &groundNormal, false,
		                             ModelStandsOnGround(GetSpawnObjectId(), GetSpawnElementKind()))){
			surfacePos = groundHit;
			if(hitNormal) *hitNormal = groundNormal;
		}else if(hitNormal)
			*hitNormal = { 0.0f, 0.0f, 1.0f };
	}else if(hitNormal)
		*hitNormal = { 0.0f, 0.0f, 1.0f };

	if(hitPos)
		*hitPos = surfacePos;
	return true;
}

// --- Brush tool ---

// Find the surface point under the mouse cursor and its normal.
// Tries a direct ray-cast against visible instances first (so the brush paints
// on whatever is under the cursor — terrain, buildings, rocks); falls back to
// intersecting the mouse ray with a horizontal plane at the camera target
// height and then snapping to ground.
// Selected instances are always skipped: after a brush placement the new
// instance is selected, so this prevents drag-paint from stacking up on its
// own just-painted objects.
bool
GetBrushSurfaceHit(rw::V3d *hitPos, rw::V3d *hitNormal)
{
	Ray ray;
	ray.start = TheCamera.m_position;
	ray.dir = normalize(TheCamera.m_mouseDir);

	// Direct hit against any visible object, ignoring the current selection.
	float bestT = 1.0e30f;
	ObjectInst *hitInst = GetVisibleInstUnderRay(ray, nil, &bestT);
	if(hitInst && !hitInst->m_selected){
		rw::V3d p, n;
		if(IntersectRayColModelDetailed(ray, hitInst, &p, &n)){
			if(n.z < 0.0f) n = scale(n, -1.0f);
			if(hitPos) *hitPos = p;
			if(hitNormal) *hitNormal = n;
			return true;
		}
	}

	// Fallback: project ray to camera-target plane, then snap down to ground.
	float planeZ = TheCamera.m_target.z;
	rw::V3d dir = ray.dir;
	rw::V3d origin = ray.start;
	rw::V3d surfacePos;
	if(fabs(dir.z) < 0.001f)
		surfacePos = add(origin, scale(dir, 50.0f));
	else{
		float t = (planeZ - origin.z) / dir.z;
		if(t < 1.0f) t = 50.0f;
		if(t > 5000.0f) t = 5000.0f;
		surfacePos.x = origin.x + dir.x * t;
		surfacePos.y = origin.y + dir.y * t;
		surfacePos.z = planeZ;
	}

	rw::V3d groundHit, groundNormal = { 0.0f, 0.0f, 1.0f };
	if(GetGroundPlacementSurface(surfacePos, &groundHit, &groundNormal, /*ignoreSelection=*/true,
	                             ModelStandsOnGround(GetSpawnObjectId(), GetSpawnElementKind()))){
		if(hitPos) *hitPos = groundHit;
		if(hitNormal) *hitNormal = groundNormal;
		return true;
	}
	return false;
}

void
EnterBrushMode(int objectId)
{
	if(objectId < 0 || GetObjectDef(objectId) == nil)
		return;
	// Mutually exclusive with place mode
	if(gPlaceMode)
		SpawnExitPlaceMode();
	if(gPrefabPlaceMode)
		ExitPrefabPlaceMode();
	SetSpawnObjectId(objectId);
	gBrushMode = true;
}

void
ExitBrushMode(void)
{
	gBrushMode = false;
}

// Drag-paint state: last sample position + time, used to gate continuous paint.
static rw::V3d sBrushLastPaintPos = { 0.0f, 0.0f, 0.0f };
static bool sBrushHasLastPaint = false;
static double sBrushLastPaintTime = 0.0;

static float
brushRandomUnit(void)
{
	return (float)rand() / (float)RAND_MAX;
}

// Pick a uniform random point inside the brush disc (in world XY), centered at `center`.
static rw::V3d
brushSampleInDisc(rw::V3d center, float radius)
{
	if(radius <= 0.0001f)
		return center;
	// Uniform disc: r = R*sqrt(u), theta = 2π*v
	float u = brushRandomUnit();
	float v = brushRandomUnit();
	float r = radius * sqrtf(u);
	float theta = v * 6.28318530718f;
	rw::V3d p = center;
	p.x += r * cosf(theta);
	p.y += r * sinf(theta);
	return p;
}

// Build the per-spawn rotation quaternion: random yaw in [yawMin,yawMax],
// optionally swung to the surface normal.
static rw::Quat
brushBuildRotation(rw::V3d hitNormal)
{
	rw::Quat rot = { 0.0f, 0.0f, 0.0f, 1.0f };
	float yawSpan = gBrushYawMax - gBrushYawMin;
	float yawDeg = gBrushYawMin + (yawSpan > 0.0f ? yawSpan * brushRandomUnit() : 0.0f);
	if(fabs(yawDeg) > 0.0001f){
		float yawRad = yawDeg * (3.14159265358979323846f / 180.0f);
		rot = rw::Quat::rotation(yawRad, { 0.0f, 0.0f, 1.0f });
	}
	if(gBrushAlignToSurface)
		rot = BuildGroundAlignedRotationFromRotation(rot, hitNormal);
	return rot;
}

// A single brush burst: fires N placements (N = clamp(gBrushCount) for radius > 0, else 1)
// and batches them into a single undo entry. Each sample is ground-snapped
// independently so the brush tracks terrain even across a wide disc.
static void
brushBurstAt(rw::V3d hitPos, rw::V3d hitNormal)
{
	int objId = GetSpawnObjectId();
	if(objId < 0) return;

	float baseOff = GetPlacementBaseOffset(objId);

	int count = 1;
	if(gBrushRadius > 0.01f){
		count = gBrushCount;
		if(count < 1) count = 1;
		if(count > BRUSH_MAX_PER_BURST) count = BRUSH_MAX_PER_BURST;
	}

	ObjectInst *batch[MAX_BATCH_OBJECTS];
	int n = 0;
	ClearSelection();

	for(int i = 0; i < count; i++){
		if(n + 2 > MAX_BATCH_OBJECTS)
			break;

		rw::V3d samplePos, sampleNormal = hitNormal;
		if(count == 1 || gBrushRadius <= 0.01f){
			samplePos = hitPos;
		}else{
			rw::V3d discPoint = brushSampleInDisc(hitPos, gBrushRadius);
			rw::V3d snapped;
			if(GetGroundPlacementSurface(discPoint, &snapped, &sampleNormal, /*ignoreSelection=*/true))
				samplePos = snapped;
			else
				continue;  // sample outside world bounds — skip
		}

		rw::Quat rot = brushBuildRotation(sampleNormal);

		samplePos.z += baseOff;
		samplePos.z += gBrushZOffset;

		n += SpawnPlaceObjectNoUndo(samplePos, &rot, &batch[n], MAX_BATCH_OBJECTS - n);
	}

	if(n > 0){
		UndoRecordPaste(batch, n);
		ObjectDef *obj = GetObjectDef(objId);
		if(count > 1)
			Toast(TOAST_SPAWN, _("Painted %d x %s"), count, obj ? obj->m_name : "?");
		else
			Toast(TOAST_SPAWN, _("Placed %s"), obj ? obj->m_name : "?");
	}

	sBrushLastPaintPos = hitPos;
	sBrushHasLastPaint = true;
	sBrushLastPaintTime = ImGui::GetTime();
}

static void
handleBrushTool(void)
{
	ImGuiIO &io = ImGui::GetIO();
	if(io.WantCaptureMouse || gGizmoHovered || gGizmoUsing)
		return;

	// Cancel: a right or middle click (not a camera drag) or the cancel shortcut (Esc).
	if(CPad::IsMButtonClicked(3) || CPad::IsMButtonClicked(2) || ShortcutPressed(SC_CANCEL)){
		ExitBrushMode();
		return;
	}

	rw::V3d hitPos, hitNormal;
	bool haveHit = GetBrushSurfaceHit(&hitPos, &hitNormal);

	if(!haveHit){
		sBrushHasLastPaint = false;
		return;
	}

	// LMB just pressed — place immediately, start a paint stroke.
	if(CPad::IsMButtonJustDown(1)){
		brushBurstAt(hitPos, hitNormal);
		return;
	}

	// LMB held and moved — continuous paint, gated by spacing AND optional delay.
	if(CPad::IsMButtonDown(1) && sBrushHasLastPaint){
		bool spacingOk = true;
		if(gBrushSpacing > 0.01f){
			rw::V3d d = sub(hitPos, sBrushLastPaintPos);
			float d2 = d.x*d.x + d.y*d.y + d.z*d.z;
			float s2 = gBrushSpacing * gBrushSpacing;
			spacingOk = d2 >= s2;
		}
		bool delayOk = true;
		if(gBrushDelayMs > 0.1f){
			double elapsedMs = (ImGui::GetTime() - sBrushLastPaintTime) * 1000.0;
			delayOk = elapsedMs >= gBrushDelayMs;
		}
		if(spacingOk && delayOk)
			brushBurstAt(hitPos, hitNormal);
	}

	// LMB released — end stroke.
	if(!CPad::IsMButtonDown(1))
		sBrushHasLastPaint = false;
}

// --- Rect-select (marquee selection) ---

// Called early in Draw(), before Camera.Process, to detect Shift+LMB start
// and set gRectSelectActive so the camera knows to skip LMB look.
static void
updateRectSelectEarly(void)
{
	if(AttachmentTargetPickActive()){
		gRectSelectActive = false;
		sRectSelectDragging = false;
		return;
	}
	if(CoordinatePinOwnsMouse() || LayoutPathsOwnsMouse() || MapToolsPickingLine()){
		gRectSelectActive = false;
		sRectSelectDragging = false;
		return;
	}
	ImGuiIO &io = ImGui::GetIO();
	bool blockInput = io.WantCaptureMouse || gGizmoHovered || gGizmoUsing;

	// Start: Shift+LMB just pressed, not blocked
	if(!gRectSelectActive && !blockInput &&
	   CPad::IsShiftDown() && CPad::IsMButtonJustDown(1) &&
	   !gPlaceMode && !gBrushMode){
		gRectSelectActive = true;
		sRectSelectDragging = false;
		sRectStartX = (float)CPad::newMouseState.x;
		sRectStartY = (float)CPad::newMouseState.y;
	}

	// Threshold: promote pending to dragging
	if(gRectSelectActive && !sRectSelectDragging && CPad::IsMButtonDown(1)){
		float dx = (float)CPad::newMouseState.x - sRectStartX;
		float dy = (float)CPad::newMouseState.y - sRectStartY;
		if(dx*dx + dy*dy > RECT_SELECT_THRESHOLD_SQ)
			sRectSelectDragging = true;
	}

	// Cancel if LMB released before threshold (was a Shift+click, not a drag)
	if(gRectSelectActive && !sRectSelectDragging && !CPad::IsMButtonDown(1))
		gRectSelectActive = false;
}

struct RectSelectCtx {
	float x1, y1, x2, y2;
	bool removeMode;
	int count;
};

// Project an instance's bounding box to a screen-space AABB and test overlap
// with the selection rectangle.  Returns true if the projected bounds overlap.
// Falls back to bounding-sphere center when no collision model is available.
static bool
instOverlapsScreenRect(ObjectInst *inst, float rx1, float ry1, float rx2, float ry2)
{
	ObjectDef *obj = GetObjectDef(inst->m_objectId);

	// An editor icon is a point on screen, not its stand-in model's box.
	if(obj && obj->m_colModel && !InstUsesEditorIcon(inst)){
		CColModel *col = obj->m_colModel;
		float sxMin = 1e30f, syMin = 1e30f;
		float sxMax = -1e30f, syMax = -1e30f;
		int projected = 0;

		for(int ix = 0; ix < 2; ix++)
		for(int iy = 0; iy < 2; iy++)
		for(int iz = 0; iz < 2; iz++){
			rw::V3d v;
			v.x = ix ? col->boundingBox.max.x : col->boundingBox.min.x;
			v.y = iy ? col->boundingBox.max.y : col->boundingBox.min.y;
			v.z = iz ? col->boundingBox.max.z : col->boundingBox.min.z;
			rw::V3d::transformPoints(&v, &v, 1, &inst->m_matrix);

			rw::V3d sp;
			float sw, sh;
			if(!Sprite::CalcScreenCoors(v, &sp, &sw, &sh, false))
				continue;
			if(sp.x < sxMin) sxMin = sp.x;
			if(sp.y < syMin) syMin = sp.y;
			if(sp.x > sxMax) sxMax = sp.x;
			if(sp.y > syMax) syMax = sp.y;
			projected++;
		}

		if(projected == 0)
			return false;

		// AABB overlap test
		return sxMin <= rx2 && sxMax >= rx1 && syMin <= ry2 && syMax >= ry1;
	}

	// No collision model — fall back to object position
	rw::V3d sp;
	float sw, sh;
	if(!Sprite::CalcScreenCoors(inst->m_translation, &sp, &sw, &sh, false))
		return false;
	return sp.x >= rx1 && sp.x <= rx2 && sp.y >= ry1 && sp.y <= ry2;
}

static void
rectSelectHighlightInst(ObjectInst *inst, void *data)
{
	if(inst->m_isDeleted || inst->m_editorHidden || !IsPawnObjectSelectable(inst)) return;
	RectSelectCtx *ctx = (RectSelectCtx*)data;

	if(instOverlapsScreenRect(inst, ctx->x1, ctx->y1, ctx->x2, ctx->y2)){
		if(inst->m_highlight < HIGHLIGHT_HOVER)
			inst->m_highlight = HIGHLIGHT_HOVER;
	}
}

// Called late in Draw(), after handleTool: draws overlay, previews, commits on release
static void
handleRectSelect(void)
{
	if(AttachmentTargetPickActive()){
		gRectSelectActive = false;
		sRectSelectDragging = false;
		return;
	}
	if(CoordinatePinOwnsMouse() || LayoutPathsOwnsMouse() || MapToolsPickingLine()){
		gRectSelectActive = false;
		sRectSelectDragging = false;
		return;
	}
	if(!sRectSelectDragging) return;

	float curX = (float)CPad::newMouseState.x;
	float curY = (float)CPad::newMouseState.y;

	// Normalise rect
	RectSelectCtx ctx;
	ctx.x1 = sRectStartX < curX ? sRectStartX : curX;
	ctx.y1 = sRectStartY < curY ? sRectStartY : curY;
	ctx.x2 = sRectStartX > curX ? sRectStartX : curX;
	ctx.y2 = sRectStartY > curY ? sRectStartY : curY;
	ctx.removeMode = CPad::IsAltDown();
	ctx.count = 0;

	// Draw selection rectangle overlay
	ImDrawList *dl = EditorForegroundDrawList();
	ImVec2 p0 = EditorUiPoint(ctx.x1, ctx.y1);
	ImVec2 p1 = EditorUiPoint(ctx.x2, ctx.y2);
	dl->AddRectFilled(p0, p1, IM_COL32(100, 150, 255, 40));
	dl->AddRect(p0, p1, IM_COL32(100, 150, 255, 200), 0.0f, 0, 1.5f);

	if(CPad::IsMButtonDown(1)){
		// Still dragging — preview highlights
		ForEachVisibleInst(rectSelectHighlightInst, &ctx);
	}else{
		// LMB released — commit selection via colour-coded picking pass
		bool addMode = CPad::IsCtrlDown();

		// Render scene with colour codes and read the selection rectangle
		static rw::RGBA black = { 0, 0, 0, 0xFF };
		rw::Camera *camera = Scene.camera ? Scene.camera : TheCamera.m_rwcam;
		if(!gta::BeginColourCodePass(camera, &black)){
			sRectSelectDragging = false;
			gRectSelectActive = false;
			Toast(TOAST_SELECTION, _("Rectangle select failed to create the picking buffer"));
			return;
		}
		RenderEverythingColourCoded();

		int rx = (int)ctx.x1;
		int ry = (int)ctx.y1;
		int rw = (int)(ctx.x2 - ctx.x1 + 0.5f);
		int rh = (int)(ctx.y2 - ctx.y1 + 0.5f);
		int32 codes[MAX_BATCH_OBJECTS];
		int numCodes = gta::GetColourCodesInRect(rx, ry, rw, rh, codes, MAX_BATCH_OBJECTS);
		gta::EndColourCodePass();
		if(numCodes < 0){
			sRectSelectDragging = false;
			gRectSelectActive = false;
			Toast(TOAST_SELECTION, _("Rectangle select failed to read the picking buffer"));
			return;
		}
		if(!addMode && !ctx.removeMode)
			ClearSelection();
		if(numCodes >= MAX_BATCH_OBJECTS)
			Toast(TOAST_SELECTION, _("Rectangle select limited to %d object(s)"), MAX_BATCH_OBJECTS);

		int count = 0;
		for(int i = 0; i < numCodes; i++){
			ObjectInst *inst = GetInstanceByID(codes[i]);
			if(inst && !inst->m_isDeleted && !inst->m_editorHidden && IsPawnObjectSelectable(inst)){
				if(ctx.removeMode)
					inst->Deselect();
				else
					inst->Select();
				count++;
			}
		}

		if(count > 0)
			Toast(TOAST_SELECTION, _("Selected %d object(s)"), count);

		sRectSelectDragging = false;
		gRectSelectActive = false;
	}
}

void
handleTool(void)
{
	if(AttachmentTargetPickActive()){
		ImGuiIO &io = ImGui::GetIO();
		if(!io.WantCaptureMouse){
			if(CPad::IsMButtonClicked(3) || CPad::IsMButtonClicked(2) || ShortcutPressed(SC_CANCEL)){
				CancelAttachmentTargetPick();
				Toast(TOAST_SELECTION, _("Attachment target selection cancelled."));
			}else if(CPad::IsMButtonClicked(1)){
				int32 code = pick();
				if(code < 0){
					Toast(TOAST_SELECTION, _("Target picking failed. Click again or cancel."));
				}else{
					int attached = CompleteAttachmentTargetPick(GetInstanceByID(code));
					if(attached > 0)
						Toast(TOAST_SPAWN, _("Attached %d object(s) to the selected target."), attached);
					else
						Toast(TOAST_SELECTION,
							_("That target can't accept the selected objects. Click another target or cancel."));
				}
			}
		}
		return;
	}
	if(MapToolsHandleViewportClick()) return;
	if(LayoutPathsHandleViewport()) return;
	if(CoordinatePinHandleViewport()) return;
	// Don't process viewport clicks when ImGui wants the mouse
	ImGuiIO &io = ImGui::GetIO();
	// The gizmo reserves left clicks; a stationary right-click still opens
	// the object's menu, including when it is already selected.
	bool gizmoContextClick = gGizmoHovered && CPad::IsMButtonClicked(3);
	if(((io.WantCaptureMouse || gGizmoHovered) && !gizmoContextClick) || gGizmoUsing || gRectSelectActive)
		return;

	// Prefab placement mode intercepts all clicks
	if(gPrefabPlaceMode){
		if(CPad::IsMButtonClicked(1)){
			rw::V3d hitPos, hitNormal;
			GetPlacementSurfaceHit(&hitPos, &hitNormal);
			// Optionally a group named after the file, without folder or .haumea.
			std::string group;
			if(gPrefabPlaceAsGroup){
				const char *path = GetPrefabPlacePath();
				const char *slash = strrchr(path, '/');
				const char *backslash = strrchr(path, '\\');
				const char *base = slash > backslash ? slash + 1 : backslash ? backslash + 1 : path;
				group = base;
				if(group.size() > 7 && group.compare(group.size() - 7, 7, ".haumea") == 0)
					group.resize(group.size() - 7);
			}
			ImportPrefabAtEx(GetPrefabPlacePath(), hitPos, gPrefabPlaceHeading,
				gPrefabPlaceAsGroup ? group.c_str() : nil);
			if(!gPrefabPlaceRepeat && !CPad::IsKeyDown(KEY_LSHIFT) && !CPad::IsKeyDown(KEY_RSHIFT))
				ExitPrefabPlaceMode();
			return;
		}
		if(CPad::IsMButtonClicked(3) || CPad::IsMButtonClicked(2) || ShortcutPressed(SC_CANCEL)){
			ExitPrefabPlaceMode();
			return;
		}
		return;
	}

	// Place mode intercepts all clicks
	if(gPlaceMode){
		if(CPad::IsMButtonClicked(1)){
			rw::V3d pos = GetPlacementPosition();
			SpawnPlaceObject(pos);
			// Shift+click or Keep placing = stay in placement mode.
			if(!gPlaceRepeat && !CPad::IsKeyDown(KEY_LSHIFT) && !CPad::IsKeyDown(KEY_RSHIFT))
				gPlaceMode = false;
			return;
		}
		if(CPad::IsMButtonClicked(3) || CPad::IsMButtonClicked(2) || ShortcutPressed(SC_CANCEL)){
			SpawnExitPlaceMode();
			return;
		}
		return;  // Absorb clicks while in place mode
	}

	// The Nodes window's add and link tools
	if(SAPaths::HandleNodesViewportClick())
		return;
	// A dynamic area's handles and polygon corners
	if(DynamicAreaHandleViewport())
		return;

	// Brush mode intercepts all clicks (click-to-place + drag-to-paint)
	if(gBrushMode){
		handleBrushTool();
		return;
	}

	// select
	if(CPad::IsMButtonClicked(1)){
		if(!IsIplMapDocumentOpen() &&
		   (Path::hoveredNode || SAPaths::hoveredNode || Effects::hoveredEffect)){
			ClearSelection();
			Path::selectedNode = Path::hoveredNode;
			SAPaths::selectedNode = SAPaths::hoveredNode;
			Effects::selectedEffect = Effects::hoveredEffect;
		}else{
			int32 code = pick();
			if(code < 0){
				Toast(TOAST_SELECTION, _("Selection failed to create or read the picking buffer"));
				return;
			}
			ObjectInst *inst = GetInstanceByID(code);
			if(inst && !inst->m_isDeleted){
				if(CPad::IsShiftDown())
					inst->Select();
				else if(CPad::IsAltDown())
					inst->Deselect();
				else if(CPad::IsCtrlDown()){
					if(inst->m_selected) inst->Deselect();
					else inst->Select();
				}else{
					ClearSelection();
					inst->Select();
				}
			}else
				ClearSelection();

			Path::selectedNode = nil;
			SAPaths::selectedNode = nil;
			Effects::selectedEffect = nil;
		}
	}else if(CPad::IsMButtonClicked(2)){
		// Alt + short middle click resets wheel zoom while preserving plain
		// middle-click selection and middle-button drag camera controls.
		if(CPad::IsAltDown() && !CPad::IsCtrlDown()){
			TheCamera.m_fov = CAMERA_FOV_DEFAULT;
			Toast(TOAST_SELECTION, _("FOV reset to 70°"));
			return;
		}else if(!IsIplMapDocumentOpen() && CPad::IsCtrlDown()){
			Path::selectedNode = Path::hoveredNode;
			SAPaths::selectedNode = SAPaths::hoveredNode;
			Effects::selectedEffect = Effects::hoveredEffect;
		}else{
			if(!IsIplMapDocumentOpen() &&
			   (Path::hoveredNode || SAPaths::hoveredNode || Effects::hoveredEffect)){
				ClearSelection();
				Path::selectedNode = Path::hoveredNode;
				SAPaths::selectedNode = SAPaths::hoveredNode;
				Effects::selectedEffect = Effects::hoveredEffect;
			}else{
				int32 code = pick();
				if(code < 0){
					Toast(TOAST_SELECTION, _("Selection failed to create or read the picking buffer"));
					return;
				}
				ClearSelection();
				ObjectInst *inst = GetInstanceByID(code);
				if(inst && !inst->m_isDeleted)
					inst->Select();
			}
		}
	}else if(CPad::IsMButtonClicked(3)){
		// A right-click (not a camera drag) on what Map Objects lists opens
		// its menu; anywhere else it clears the selection.
		{
			int32 code = pick();
			if(code >= 0 && OpenViewportObjectMenu(GetInstanceByID(code)))
				return;
		}
		ClearSelection();
		Path::selectedNode = nil;
		SAPaths::selectedNode = nil;
		Effects::selectedEffect = nil;
	}
}

rw::Texture *(*originalFindCB)(const char *name);
rw::TexDictionary *fallbackTxd;
static rw::Texture*
findTextureCaseInsensitive(rw::TexDictionary *txd, const char *name)
{
	if(txd == nil || name == nil)
		return nil;
	FORLIST(lnk, txd->textures){
		rw::Texture *tex = rw::Texture::fromDict(lnk);
		if(tex && rw::strcmp_ci(tex->name, name) == 0)
			return tex;
	}
	return nil;
}
static rw::Texture*
fallbackFindCB(const char *name)
{
	rw::Texture *t = originalFindCB(name);
	if(t) return t;
	t = fallbackTxd->find(name);
	if(t) return t;
	return findTextureCaseInsensitive(fallbackTxd, name);
}

// ===== Startup splash =====
// LoadGame runs in one go before the first editor frame, so it reports
// progress through SplashProgress and a splash frame is drawn between steps.

static float splashProgress;
static char splashStatus[256];
static bool splashActive;	// only while LoadGame runs

static float splashExpand;	// 0..1 while the splash grows into the editor

static ImU32
SplashColor(int r, int g, int b, float alpha)
{
	return IM_COL32(r, g, b, (int)(clamp(alpha, 0.0f, 1.0f) * 255.0f));
}

// Cheap repeatable hash in [0, 1) for star positions.
static float
SplashHash(int i, int salt)
{
	uint32 h = (uint32)i * 2654435761u ^ (uint32)salt * 40503u;
	h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
	return (h & 0xFFFFFF) / (float)0x1000000;
}

// Haumea itself: an elongated icy dwarf planet with a ring and two moons,
// Hi'iaka and Namaka. Back halves of the ring and orbits go behind it.
static void
DrawSplashPlanet(ImDrawList *dl, ImVec2 c, float r, float t, ImU32 accent)
{
	const float tilt = -0.32f;
	ImVec2 body(r * 1.38f, r * 0.86f);
	ImVec2 ring(r * 2.15f, r * 0.42f);
	ImVec4 a = ImGui::ColorConvertU32ToFloat4(accent);
	int ar = (int)(a.x * 255), ag = (int)(a.y * 255), ab = (int)(a.z * 255);

	// Glow.
	for(int i = 8; i >= 1; i--){
		float k = i / 8.0f;
		dl->AddEllipseFilled(c, ImVec2(body.x * (1.0f + k * 0.9f), body.y * (1.0f + k * 1.3f)),
			SplashColor(ar, ag, ab, 0.035f * (1.0f - k) + 0.01f), tilt, 64);
	}

	struct Moon { float orbitX, orbitY, period, phase, size; };
	const Moon moons[] = {
		{ 2.65f, 0.62f, 11.0f, 0.6f, 0.085f },	// Hi'iaka
		{ 1.85f, 0.44f, 7.0f, 2.4f, 0.055f },	// Namaka
	};
	float ct = cosf(tilt), st = sinf(tilt);
	auto orbitPoint = [&](float ox, float oy, float angle){
		float x = cosf(angle) * ox * r, y = sinf(angle) * oy * r;
		return ImVec2(c.x + x * ct - y * st, c.y + x * st + y * ct);
	};
	auto drawMoons = [&](bool front){
		for(const Moon &m : moons){
			float angle = t * 6.2831853f / m.period + m.phase;
			if((sinf(angle) > 0.0f) != front)
				continue;
			ImVec2 p = orbitPoint(m.orbitX, m.orbitY, angle);
			float size = r * m.size * (front ? 1.0f : 0.85f);
			dl->AddCircleFilled(p, size * 2.6f, SplashColor(ar, ag, ab, 0.08f), 24);
			dl->AddCircleFilled(p, size, SplashColor(200, 205, 225, front ? 1.0f : 0.55f), 24);
			dl->AddCircleFilled(ImVec2(p.x - size * 0.3f, p.y - size * 0.3f), size * 0.55f,
				SplashColor(245, 247, 255, front ? 0.9f : 0.4f), 16);
		}
	};
	auto drawOrbitPath = [&](float ox, float oy, float from, float to, float alpha){
		dl->PathEllipticalArcTo(c, ImVec2(ox * r, oy * r), tilt, from, to, 64);
		dl->PathStroke(SplashColor(ar, ag, ab, alpha), 0, 1.0f);
	};
	auto drawRing = [&](float from, float to, float alpha){
		dl->PathEllipticalArcTo(c, ImVec2(ring.x * 1.06f, ring.y * 1.06f), tilt, from, to, 96);
		dl->PathStroke(SplashColor(ar, ag, ab, alpha * 0.35f), 0, r * 0.05f);
		dl->PathEllipticalArcTo(c, ring, tilt, from, to, 96);
		dl->PathStroke(SplashColor(230, 232, 255, alpha), 0, 2.0f);
	};

	// Behind the planet.
	for(const Moon &m : moons)
		drawOrbitPath(m.orbitX, m.orbitY, 3.14159265f, 6.2831853f, 0.12f);
	drawRing(3.14159265f, 6.2831853f, 0.45f);
	drawMoons(false);

	// The body, lit from the upper left.
	dl->AddEllipseFilled(c, body, SplashColor(34, 36, 62, 1.0f), tilt, 96);
	// Many faint layers so the shading blends instead of banding.
	const int layers = 28;
	for(int i = 0; i < layers; i++){
		float k = i / (float)layers;
		ImVec2 off(-body.x * 0.2f * k, -body.y * 0.24f * k);
		float shade = 0.2f + 0.8f * k;
		dl->AddEllipseFilled(ImVec2(c.x + off.x, c.y + off.y),
			ImVec2(body.x * (1.0f - k * 0.6f), body.y * (1.0f - k * 0.6f)),
			SplashColor((int)(70 + 175 * shade), (int)(74 + 172 * shade), (int)(110 + 140 * shade), 0.16f),
			tilt, 96);
	}
	// Its dark red spot turns with the planet (Haumea spins in four hours).
	float spin = t * 0.9f;
	float facing = cosf(spin);
	if(facing > 0.0f){
		float x = sinf(spin) * body.x * 0.62f;
		ImVec2 spot(c.x + x * ct, c.y + x * st + body.y * 0.12f);
		dl->AddEllipseFilled(spot, ImVec2(body.x * 0.16f * facing, body.y * 0.13f),
			SplashColor(150, 60, 70, 0.55f * facing), tilt, 32);
	}
	dl->AddEllipse(c, body, SplashColor(ar, ag, ab, 0.35f), tilt, 96, 1.5f);

	// In front of it.
	for(const Moon &m : moons)
		drawOrbitPath(m.orbitX, m.orbitY, 0.0f, 3.14159265f, 0.22f);
	drawRing(0.0f, 3.14159265f, 0.95f);
	drawMoons(true);
}

static void
DrawSplash(float dt)
{
	// %s is the key of the tip's shortcut, from the shortcut table. N_(): drawn with _().
	static const struct { const char *text; int shortcut; } tips[] = {
		{ N_("Press %s to pick an object, then double-click it to place it."), SC_WINDOW_BROWSER },
		{ N_("Press %s to outline every object in your map, even invisible ones."), SC_OUTLINE_OBJECTS },
		{ N_("Drag a panel's title bar to dock it beside the 3D view."), -1 },
		{ N_("Hold Shift while dragging the gizmo to move in fixed steps."), -1 },
		{ N_("%s drops the selected objects onto the ground."), SC_SNAP_TO_GROUND },
		{ N_("%s pastes objects exactly where they were copied."), SC_PASTE_IN_PLACE },
		{ N_("%s points the camera at the selection."), SC_FOCUS_SELECTION },
		{ N_("%s lists every keyboard shortcut; change them in Preferences."), SC_WINDOW_SHORTCUTS },
		{ N_("The SA-MP tab places pickups, 3D labels, actors, classes, gang zones and map icons."), -1 },
		{ N_("Select several objects and edit a field: it changes on all of them."), -1 },
		{ N_("Search the browser by ID ranges, like 19000-19100."), -1 },
	};
	static int tip = -1;
	if(tip < 0)	// rand() is never seeded, so it would pick the same tip every time
		tip = (int)((uint64)std::chrono::system_clock::now().time_since_epoch().count() %
			IM_ARRAYSIZE(tips));

#ifdef LIBRW_GLFW
	// Keep the window responsive while loading.
	glfwPollEvents();
#endif
	SyncEditorWindowSize();
	rw::Camera *cam = TheCamera.m_rwcam;
	static rw::RGBA background = { 8, 8, 16, 255 };
	cam->clear(&background, rw::Camera::CLEARIMAGE | rw::Camera::CLEARZ);
	cam->beginUpdate();
	DefinedState();
	I18nNewFrame();
	ImGui_ImplRW_NewFrame(dt);

	static float time;
	time += dt;
	ImGuiIO &io = ImGui::GetIO();
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	ImFont *font = ImGui::GetFont();
	float fontSize = ImGui::GetFontSize();
	ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
	ImVec4 a = ImGui::ColorConvertU32ToFloat4(accent);
	int ar = (int)(a.x * 255), ag = (int)(a.y * 255), ab = (int)(a.z * 255);
	float W = io.DisplaySize.x, H = io.DisplaySize.y;

	// Deep space.
	dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H),
		IM_COL32(7, 7, 15, 255), IM_COL32(12, 10, 26, 255),
		IM_COL32(22, 14, 34, 255), IM_COL32(9, 9, 20, 255));
	// Nebula behind the planet.
	ImVec2 planet(W * 0.72f, H * 0.45f);
	for(int i = 0; i < 6; i++){
		float k = i / 6.0f;
		float wobble = sinf(time * 0.15f + i) * 0.04f;
		dl->AddEllipseFilled(ImVec2(planet.x - W * (0.12f - k * 0.05f), planet.y + H * (0.1f - k * 0.08f)),
			ImVec2(W * (0.34f - k * 0.04f), H * (0.42f + wobble - k * 0.05f)),
			i % 2 ? SplashColor(90, 40, 140, 0.035f) : SplashColor(ar, ag, ab, 0.03f),
			0.6f + k, 48);
	}
	// Stars drift slowly; nearer ones are bigger, faster and brighter.
	for(int i = 0; i < 190; i++){
		float depth = 0.25f + 0.75f * SplashHash(i, 3);
		float x = fmodf(SplashHash(i, 1) * W - time * 6.0f * depth + W * 4.0f, W);
		float y = SplashHash(i, 2) * H;
		float twinkle = 0.55f + 0.45f * sinf(time * (1.0f + 2.5f * SplashHash(i, 4)) + i);
		float size = 0.5f + 1.3f * depth * depth;
		dl->AddCircleFilled(ImVec2(x, y), size, SplashColor(220, 225, 255, (0.25f + 0.6f * depth) * twinkle), 8);
		if(depth > 0.92f)
			dl->AddCircleFilled(ImVec2(x, y), size * 3.5f, SplashColor(ar, ag, ab, 0.08f * twinkle), 12);
	}

	float planetRadius = min(W, H) * 0.2f;
	DrawSplashPlanet(dl, planet, planetRadius, time, accent);

	// Wordmark.
	float left = floorf(W * 0.075f);
	float y = floorf(H * 0.27f);
	ImFont *title = gTitleFont ? gTitleFont : font;
	float titleSize = floorf(min(min(W, H) * 0.17f, title->LegacySize));
	// "Haumea Studio" on one line, narrowed to stay clear of the planet.
	float wordmarkWidth = title->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Haumea Studio").x;
	if(wordmarkWidth > W * 0.5f)
		titleSize = floorf(titleSize * W * 0.5f / wordmarkWidth);
	float haumeaWidth = title->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Haumea ").x;
	dl->AddText(title, titleSize, ImVec2(left + 2.0f, y + 3.0f), IM_COL32(0, 0, 0, 110), "Haumea Studio");
	dl->AddText(title, titleSize, ImVec2(left, y), IM_COL32(245, 246, 255, 255), "Haumea");
	dl->AddText(title, titleSize, ImVec2(left + haumeaWidth, y), IM_COL32(190, 196, 230, 255), "Studio");
	y += titleSize * 1.02f;
	float bySize = floorf(fontSize * 1.15f);
	dl->AddText(font, bySize, ImVec2(left + 3.0f, y), SplashColor(ar, ag, ab, 1.0f), "by itsneufox");
	y += bySize + fontSize * 1.1f;
	dl->AddText(font, fontSize, ImVec2(left + 3.0f, y), IM_COL32(170, 172, 195, 230),
		_("The all-in-one editor for open.mp and SA-MP"));

	// Progress: a glowing bar with a light sweeping along it.
	float barWidth = floorf(min(W * 0.4f, 420.0f));
	float barY = floorf(H * 0.66f);
	ImVec2 barMin(left, barY), barMax(left + barWidth, barY + 6.0f);
	float fill = barWidth * splashProgress;
	dl->AddRectFilled(barMin, barMax, IM_COL32(255, 255, 255, 22), 3.0f);
	if(fill > 1.0f){
		for(int i = 3; i >= 1; i--)
			dl->AddRectFilled(ImVec2(barMin.x - i * 2.0f, barMin.y - i * 2.0f),
				ImVec2(barMin.x + fill + i * 2.0f, barMax.y + i * 2.0f),
				SplashColor(ar, ag, ab, 0.06f), 3.0f + i * 2.0f);
		dl->AddRectFilledMultiColor(barMin, ImVec2(barMin.x + fill, barMax.y),
			SplashColor(ar / 2, ag / 2, ab, 1.0f), SplashColor(ar, ag, ab, 1.0f),
			SplashColor(ar, ag, ab, 1.0f), SplashColor(ar / 2, ag / 2, ab, 1.0f));
		float sweep = fmodf(time * 0.8f, 1.4f) - 0.2f;
		float sx = barMin.x + fill * sweep;
		dl->PushClipRect(barMin, ImVec2(barMin.x + fill, barMax.y), true);
		dl->AddRectFilledMultiColor(ImVec2(sx - 40.0f, barMin.y), ImVec2(sx, barMax.y),
			IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 140),
			IM_COL32(255, 255, 255, 140), IM_COL32(255, 255, 255, 0));
		dl->AddRectFilledMultiColor(ImVec2(sx, barMin.y), ImVec2(sx + 40.0f, barMax.y),
			IM_COL32(255, 255, 255, 140), IM_COL32(255, 255, 255, 0),
			IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 140));
		dl->PopClipRect();
	}
	char percent[16];
	snprintf(percent, sizeof(percent), "%d%%", (int)(splashProgress * 100.0f + 0.5f));
	ImVec2 percentSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, percent);
	dl->AddText(font, fontSize, ImVec2(barMax.x - percentSize.x, barMax.y + 8.0f),
		IM_COL32(235, 236, 250, 230), percent);
	dl->AddText(font, fontSize, ImVec2(left, barMax.y + 8.0f), IM_COL32(180, 182, 205, 230), splashStatus);

	// Footer.
	char tipText[200];
	char tipBody[160];
	if(tips[tip].shortcut >= 0)
		snprintf(tipBody, sizeof(tipBody), _(tips[tip].text), ShortcutLabel(tips[tip].shortcut));
	else
		snprintf(tipBody, sizeof(tipBody), "%s", _(tips[tip].text));
	snprintf(tipText, sizeof(tipText), _("Tip: %s"), tipBody);
	dl->AddText(font, fontSize, ImVec2(left, H - fontSize * 2.6f), IM_COL32(170, 172, 195, 210), tipText);
	char version[64];
	const char *channel = HAUMEA_CHANNEL_DISPLAY;	// empty for production
	snprintf(version, sizeof(version), "%s%s%s", HaumeaFullVersion(), channel[0] ? "  \xC2\xB7  " : "", channel);
	ImVec2 versionSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, version);
	dl->AddText(font, fontSize, ImVec2(W - versionSize.x - left * 0.5f, H - fontSize * 2.6f),
		IM_COL32(150, 152, 175, 190), version);

	// Fade in at start, and into the editor's background as it grows.
	float fadeIn = clamp(time / 0.6f, 0.0f, 1.0f);
	float cover = max(1.0f - fadeIn, splashExpand * splashExpand);
	if(cover > 0.0f)
		ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H),
			SplashColor(8, 8, 16, cover));

	// First run: the game folder is chosen on the splash before loading.
	if(WorkspaceSetupNeeded())
		DrawStartScreen(false);

	ImGui::EndFrame();
	ImGui::Render();
	ImGui_ImplRW_RenderDrawLists(ImGui::GetDrawData());
	cam->endUpdate();
	cam->showRaster(0);
	ShowEditorWindow();
}

// Test runs and command-line modes (--pawn-roundtrip...): nobody watches the
// splash, and under software GL on a busy machine each frame of it costs more
// than the loading it shows.
static bool
splashUnwatched(void)
{
	return SettingsAreReadOnly() || getenv("HAUMEA_TEST_XVFB") != nil;
}

// Grows the splash window into the editor window, then applies the saved
// placement (and maximized state).
void
ExpandSplashWindow(void)
{
	using Clock = std::chrono::steady_clock;
	int tx, ty, tw, th;
	bool canPosition;
	if(!GetInitialEditorWindowRect(&tx, &ty, &tw, &th, &canPosition)){
		ApplyInitialEditorWindowState();
		return;
	}
	if(splashUnwatched()){
		// No animation: straight to the editor's size.
		SetEditorWindowDecorated(true);
		SetEditorWindowRect(tx, ty, tw, th, canPosition);
		ApplyInitialEditorWindowState();
		return;
	}
	if(!canPosition){
		// Wayland: the window cannot keep itself centred while growing, it
		// would grow off its top-left corner. Map it again at its final size
		// (the work area when maximized, so maximizing changes nothing) and
		// let the compositor place it. A few covered splash frames fill it
		// at once, while the first editor frame is still opening the map.
		HideEditorWindow();
		SetEditorWindowDecorated(true);
		SetEditorWindowRect(0, 0, tw, th, false);
		ApplyInitialEditorWindowState();
		ShowEditorWindow();
		splashActive = true;
		splashExpand = 1.0f;
		strncpy(splashStatus, _("Opening map"), sizeof(splashStatus) - 1);
		for(int i = 0; i < 3; i++)
			DrawSplash(1/60.0f);
		splashExpand = 0.0f;
		splashActive = false;
		return;
	}
	int sx, sy, sw, sh;
	GetEditorWindowRect(&sx, &sy, &sw, &sh);
	SetEditorWindowDecorated(true);
	splashActive = true;
	splashProgress = 1.0f;
	strncpy(splashStatus, _("Ready"), sizeof(splashStatus) - 1);
	const float duration = 0.6f;
	Clock::time_point start = Clock::now(), last = start;
	for(;;){
		Clock::time_point now = Clock::now();
		float k = clamp(std::chrono::duration<float>(now - start).count() / duration, 0.0f, 1.0f);
		float dt = clamp(std::chrono::duration<float>(now - last).count(), 1/240.0f, 0.1f);
		last = now;
		float e = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);	// ease out
		splashExpand = e;
		SetEditorWindowRect((int)(sx + (tx - sx) * e), (int)(sy + (ty - sy) * e),
			(int)(sw + (tw - sw) * e), (int)(sh + (th - sh) * e), canPosition);
		DrawSplash(dt);
		if(k >= 1.0f)
			break;
	}
	splashExpand = 0.0f;
	splashActive = false;
	ApplyInitialEditorWindowState();
}

// Draws at most every 33 ms (every second when unwatched), and never more
// often than 4x what the last frame took to draw, so drawing stays a small
// part of loading even with a slow GPU.
static void
SplashDrawThrottled(void)
{
	using Clock = std::chrono::steady_clock;
	static Clock::time_point last;
	static float cost;
	static bool drawn;
	Clock::time_point now = Clock::now();
	float elapsed = std::chrono::duration<float>(now - last).count();
	float interval = max(splashUnwatched() ? 1.0f : 0.033f, cost * 4.0f);
	if(drawn && elapsed < interval)
		return;
	DrawSplash(drawn ? min(elapsed, 0.1f) : 1/60.0f);
	last = Clock::now();
	cost = std::chrono::duration<float>(last - now).count();
	drawn = true;
}

void
SplashProgress(float progress, const char *status)
{
	if(!splashActive)
		return;
	splashProgress = clamp(progress, 0.0f, 1.0f);
	strncpy(splashStatus, status, sizeof(splashStatus) - 1);
	SplashDrawThrottled();
}

// For long loops inside a loading step: keeps the window responding and
// the splash animated. Does nothing once the editor is running, so files
// loaded later (imports, merges) never draw the splash.
void
SplashKeepAlive(void)
{
	if(splashActive)
		SplashDrawThrottled();
}

void
LoadGame(void)
{
// for debugging...
//	SetCurrentDirectory("C:/Users/aap/games/gta3");
//	SetCurrentDirectory("C:/Users/aap/games/gtavc");
//	SetCurrentDirectory("C:/Users/aap/games/gtasa");

	splashActive = true;
	SplashProgress(0.0f, _("Starting"));
	SAPaths::Reset();
	FindVersion();
	ModloaderInit();
	switch(gameversion){
	case GAME_III: debug("found III!\n"); break;
	case GAME_VC: debug("found VC!\n"); break;
	case GAME_SA: debug("found SA!\n"); break;
	case GAME_LCS: debug("found LCS!\n"); break;
	case GAME_VCS: debug("found VCS!\n"); break;
	default: panic("unknown game");
	}
	switch(gameplatform){
	case PLATFORM_PS2: debug("assuming PS2\n"); break;
	case PLATFORM_XBOX: debug("assuming Xbox\n"); break;
	default: debug("assuming PC\n"); break;
	}
	InitParams();

	TheCamera.m_position = params.initcampos;
	TheCamera.m_target = params.initcamtarg;
	TheCamera.setDistanceToTarget(50.0f);
	gDoBackfaceCulling = params.backfaceCull;

	defaultTxd = rw::TexDictionary::getCurrent();

	SplashProgress(0.04f, _("Loading textures and weather"));
	int particleTxdSlot = AddTxdSlot("particle");
	LoadTxd(particleTxdSlot, "MODELS/PARTICLE.TXD");

	gameTxdSlot = AddTxdSlot("generic");
	CreateTxd(gameTxdSlot);
	TxdMakeCurrent(gameTxdSlot);
	if(params.txdFallbackGeneric){
		fallbackTxd = rw::TexDictionary::getCurrent();
		originalFindCB = rw::Texture::findCB;
		rw::Texture::findCB = fallbackFindCB;
	}

	Timecycle::Initialize();
	if(params.neoWorldPipe)
		Timecycle::InitNeoWorldTweak();
	WaterLevel::Initialise();
	Clouds::Init();

	AddColSlot("generic");
	AddIplSlot("generic");

	SplashProgress(0.08f, _("Opening model archives"));

	AddCdImage("MODELS\\GTA3.IMG");
	if(isSA())
		AddCdImage("MODELS\\GTA_INT.IMG");

	if(ModloaderIsActive()){
		ModloaderDatEntry imgEntries[512];
		int nImg = ModloaderGetAdditions(imgEntries, 512);
		for(int i = 0; i < nImg; i++)
			if(strcmp(imgEntries[i].type, "IMG") == 0 || strcmp(imgEntries[i].type, "CDIMAGE") == 0)
				AddCdImage(imgEntries[i].logicalPath);
	}

	// SA-MP's own models (19xxx objects, matcolours etc.) from SAMP/SAMP.img.
	if(isSA())
		SampLoader::DiscoverAndRegisterImages();

	FileLoader::LoadLevel("data/default.dat", 0.12f, 0.18f);
	switch(gameversion){
	case GAME_III: FileLoader::LoadLevel("data/gta3.dat", 0.18f, 0.82f); break;
	case GAME_VC: FileLoader::LoadLevel("data/gta_vc.dat", 0.18f, 0.82f); break;
	case GAME_SA: FileLoader::LoadLevel("data/gta.dat", 0.18f, 0.82f); break;
	case GAME_LCS: FileLoader::LoadLevel("data/gta_lcs.dat", 0.18f, 0.82f); break;
	case GAME_VCS: FileLoader::LoadLevel("data/gta_vcs.dat", 0.18f, 0.82f); break;
	}

	if(isSA()){
		SplashProgress(0.84f, _("Loading vehicles"));
		FileLoader::LoadVehicleDefinitions();
		RefreshCdImageMappings();
	}

	if(ModloaderIsActive()){
		ModloaderDatEntry entries[256];
		int n = ModloaderGetAdditions(entries, 256);
		RefreshCdImageMappings();
		for(int i = 0; i < n; i++){
			if(strcmp(entries[i].type, "IPL") == 0){
				FileLoader::currentFile = NewGameFile((char*)entries[i].logicalPath);
				FileLoader::LoadScene(entries[i].logicalPath);
			}
		}
	}

	SplashProgress(0.88f, _("Loading SA-MP objects"));
	if(isSA())
		SampLoader::LoadDefinitions();

	InitLodLookup();
	InitObjectCategories();
	SampLoader::ExtendCategories();
	if(isSA())
		LoadTrainTracks();
	BuildModelMeta();	// the filters' facts, now every model and the world are in
	LoadFavourites();
	LoadRecentObjects();
	// InitPreviewRenderer called lazily on first use
	SplashProgress(0.95f, _("Building the world"));
	InitSectors();

	CPtrNode *p;
	ObjectInst *inst;
	int instCount = 0;
	for(p = instances.first; p; p = p->next){
		inst = (ObjectInst*)p->item;
		InsertInstIntoSectors(inst);
		if((++instCount & 1023) == 0)
			SplashKeepAlive();
	}

	// hide the islands
	ObjectDef *obj;
	if(params.map == GAME_III){
		obj = GetObjectDef("IslandLODInd", nil);
		if(obj) obj->m_isHidden = true;
		obj = GetObjectDef("IslandLODcomIND", nil);
		if(obj) obj->m_isHidden = true;
		obj = GetObjectDef("IslandLODcomSUB", nil);
		if(obj) obj->m_isHidden = true;
		obj = GetObjectDef("IslandLODsubIND", nil);
		if(obj) obj->m_isHidden = true;
		obj = GetObjectDef("IslandLODsubCOM", nil);
		if(obj) obj->m_isHidden = true;
	}else if(params.map == GAME_VC){
		obj = GetObjectDef("IslandLODmainland", nil);
		if(obj) obj->m_isHidden = true;
		obj = GetObjectDef("IslandLODbeach", nil);
		if(obj) obj->m_isHidden = true;
	}
	SplashProgress(1.0f, _("Ready"));
	splashActive = false;
}

static void
updateRwFrame(ObjectInst *inst)
{
	if(inst->m_rwObject == nil) return;
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil) return;
	rw::Frame *f;
	if(obj->m_type == ObjectDef::ATOMIC)
		f = ((rw::Atomic*)inst->m_rwObject)->getFrame();
	else
		f = ((rw::Clump*)inst->m_rwObject)->getFrame();
	f->transform(&inst->m_matrix, rw::COMBINEREPLACE);
}

struct ObjectSnapBounds
{
	rw::V3d min;
	rw::V3d max;
};

static void
addObjectSnapPoint(const rw::V3d &point, ObjectSnapBounds *out, bool *first)
{
	if(out == nil || first == nil)
		return;
	if(*first){
		out->min = point;
		out->max = point;
		*first = false;
	}else{
		out->min.x = min(out->min.x, point.x);
		out->min.y = min(out->min.y, point.y);
		out->min.z = min(out->min.z, point.z);
		out->max.x = max(out->max.x, point.x);
		out->max.y = max(out->max.y, point.y);
		out->max.z = max(out->max.z, point.z);
	}
}

static void
addObjectSnapGeometryBounds(rw::Geometry *geometry, rw::Frame *frame,
	const rw::Matrix &worldMatrix, ObjectSnapBounds *out, bool *first)
{
	if(geometry == nil)
		return;

	rw::Matrix local;
	local.setIdentity();
	bool haveLocal = frame != nil && frame->getLTM() != nil;
	if(haveLocal)
		local = *frame->getLTM();

	if(geometry->numMorphTargets > 0 && geometry->morphTargets &&
	   geometry->morphTargets[0].vertices && geometry->numVertices > 0){
		for(int i = 0; i < geometry->numVertices; i++){
			rw::V3d point = geometry->morphTargets[0].vertices[i];
			if(haveLocal)
				rw::V3d::transformPoints(&point, &point, 1, &local);
			rw::V3d::transformPoints(&point, &point, 1, &worldMatrix);
			addObjectSnapPoint(point, out, first);
		}
		return;
	}

	// Native or otherwise incomplete geometry can still provide a useful
	// broad-phase bound through its model-space bounding sphere.
	if(geometry->numMorphTargets > 0 && geometry->morphTargets){
		const rw::Sphere &sphere = geometry->morphTargets[0].boundingSphere;
		for(int ix = 0; ix < 2; ix++)
		for(int iy = 0; iy < 2; iy++)
		for(int iz = 0; iz < 2; iz++){
			rw::V3d point = {
				sphere.center.x + (ix ? sphere.radius : -sphere.radius),
				sphere.center.y + (iy ? sphere.radius : -sphere.radius),
				sphere.center.z + (iz ? sphere.radius : -sphere.radius)
			};
			if(haveLocal)
				rw::V3d::transformPoints(&point, &point, 1, &local);
			rw::V3d::transformPoints(&point, &point, 1, &worldMatrix);
			addObjectSnapPoint(point, out, first);
		}
	}
}

static bool
getObjectSnapBounds(ObjectInst *inst, const rw::V3d *position, ObjectSnapBounds *out)
{
	if(inst == nil || out == nil)
		return false;
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil)
		return false;
	rw::Matrix matrix = inst->m_matrix;
	if(position)
		matrix.pos = *position;
	bool first = true;
	if(obj->m_colModel){
		CBox box = obj->m_colModel->boundingBox;
		for(int ix = 0; ix < 2; ix++)
		for(int iy = 0; iy < 2; iy++)
		for(int iz = 0; iz < 2; iz++){
			rw::V3d point = {
				ix ? box.max.x : box.min.x,
				iy ? box.max.y : box.min.y,
				iz ? box.max.z : box.min.z
			};
			rw::V3d::transformPoints(&point, &point, 1, &matrix);
			addObjectSnapPoint(point, out, &first);
		}
		return !first;
	}

	// Some custom and decorative models have no COL at all. Use the loaded
	// render geometry so those models can still be snapped together.
	if(obj->m_type == ObjectDef::ATOMIC){
		for(int i = 0; i < obj->m_numAtomics && i < 3; i++){
			rw::Atomic *atomic = obj->m_atomics[i];
			if(atomic)
				addObjectSnapGeometryBounds(atomic->geometry, atomic->getFrame(),
					matrix, out, &first);
		}
	}else if(obj->m_type == ObjectDef::CLUMP && obj->m_clump){
		FORLIST(lnk, obj->m_clump->atomics){
			rw::Atomic *atomic = rw::Atomic::fromClump(lnk);
			if(atomic)
				addObjectSnapGeometryBounds(atomic->geometry, atomic->getFrame(),
					matrix, out, &first);
		}
	}
	return !first;
}

static void
addObjectSnapCandidates(CPtrList *list, std::vector<ObjectInst*> &candidates)
{
	if(list == nil)
		return;
	for(CPtrNode *p = list->first; p; p = p->next){
		ObjectInst *candidate = (ObjectInst*)p->item;
		if(candidate && std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
			candidates.push_back(candidate);
	}
}

static bool
snapBoundsOverlap(float minA, float maxA, float minB, float maxB, float tolerance)
{
	return minA <= maxB + tolerance && maxA + tolerance >= minB;
}

static void
tryObjectSnapAxis(float movingMin, float movingMax, float targetMin, float targetMax,
	float *best, float maxDistance)
{
	float candidates[4] = {
		targetMin - movingMax,
		targetMax - movingMin,
		targetMin - movingMin,
		targetMax - movingMax,
	};
	for(int i = 0; i < 4; i++){
		if(fabsf(candidates[i]) <= maxDistance + 0.0001f &&
		   fabsf(candidates[i]) < fabsf(*best))
			*best = candidates[i];
	}
}

// How good a snap is: the largest correction on one axis (smaller wins),
// then more axes closed at once, then the smaller total.
struct ObjectSnapScore
{
	float max;
	float sum;
	int axes;
};

static bool
snapScoreBetter(const ObjectSnapScore &a, const ObjectSnapScore &b)
{
	return a.max < b.max - 0.0001f ||
		(fabsf(a.max - b.max) <= 0.0001f &&
		 (a.axes > b.axes || (a.axes == b.axes && a.sum < b.sum)));
}

// The snap of one moving box against the objects around it. self is the
// object the box belongs to (nil for a whole selection's box); nothing in
// movingSet (the selection and the LODs moving with it) is a target.
static bool
findObjectSnapDeltaForBounds(const ObjectSnapBounds &moving, ObjectInst *self, int area,
	const std::unordered_set<ObjectInst*> &movingSet, rw::V3d *outDelta, ObjectSnapScore *outScore)
{

	std::vector<ObjectInst*> candidates;
	float radius = gObjectSnapDistance;
	float minX = moving.min.x - radius;
	float maxX = moving.max.x + radius;
	float minY = moving.min.y - radius;
	float maxY = moving.max.y + radius;
	if(numSectorsX > 0 && numSectorsY > 0 &&
	   maxX >= worldBounds.left && minX < worldBounds.right &&
	   maxY >= worldBounds.bottom && minY < worldBounds.top){
		int x0 = GetSectorIndexX(minX), x1 = GetSectorIndexX(maxX);
		int y0 = GetSectorIndexY(minY), y1 = GetSectorIndexY(maxY);
		for(int y = y0; y <= y1; y++)
			for(int x = x0; x <= x1; x++){
				Sector *sector = GetSector(x, y);
				addObjectSnapCandidates(&sector->buildings, candidates);
				addObjectSnapCandidates(&sector->buildings_overlap, candidates);
				addObjectSnapCandidates(&sector->bigbuildings, candidates);
				addObjectSnapCandidates(&sector->bigbuildings_overlap, candidates);
			}
	}
	addObjectSnapCandidates(&outOfBoundsSector.buildings, candidates);
	addObjectSnapCandidates(&outOfBoundsSector.buildings_overlap, candidates);
	addObjectSnapCandidates(&outOfBoundsSector.bigbuildings, candidates);
	addObjectSnapCandidates(&outOfBoundsSector.bigbuildings_overlap, candidates);

	ObjectSnapScore best = { 1.0e30f, 1.0e30f, 0 };
	bool found = false;
	rw::V3d bestDelta = { 0.0f, 0.0f, 0.0f };
	for(size_t i = 0; i < candidates.size(); i++){
		ObjectInst *candidate = candidates[i];
		if(candidate == nil || candidate == self || candidate->m_isDeleted || candidate->m_selected ||
		   InstUsesEditorIcon(candidate) ||
		   movingSet.count(candidate) || (self && (candidate->m_lod == self || self->m_lod == candidate)))
			continue;
		if(!gNoAreaCull && candidate->m_area != area &&
		   candidate->m_area != 13 && area != 13)
			continue;

		ObjectSnapBounds target;
		if(!getObjectSnapBounds(candidate, nil, &target))
			continue;
		rw::V3d candidateDelta = { 0.0f, 0.0f, 0.0f };
		int candidateAxes = 0;
		float xDelta = radius + 1.0f;
		if(snapBoundsOverlap(moving.min.y, moving.max.y, target.min.y, target.max.y, radius) &&
		   snapBoundsOverlap(moving.min.z, moving.max.z, target.min.z, target.max.z, radius))
			tryObjectSnapAxis(moving.min.x, moving.max.x, target.min.x, target.max.x, &xDelta, radius);
		if(fabsf(xDelta) <= radius){
			candidateDelta.x = xDelta;
			if(fabsf(xDelta) > 0.0001f)
				candidateAxes++;
		}

		float yDelta = radius + 1.0f;
		if(snapBoundsOverlap(moving.min.x, moving.max.x, target.min.x, target.max.x, radius) &&
		   snapBoundsOverlap(moving.min.z, moving.max.z, target.min.z, target.max.z, radius))
			tryObjectSnapAxis(moving.min.y, moving.max.y, target.min.y, target.max.y, &yDelta, radius);
		if(fabsf(yDelta) <= radius){
			candidateDelta.y = yDelta;
			if(fabsf(yDelta) > 0.0001f)
				candidateAxes++;
		}

		float zDelta = radius + 1.0f;
		if(snapBoundsOverlap(moving.min.x, moving.max.x, target.min.x, target.max.x, radius) &&
		   snapBoundsOverlap(moving.min.y, moving.max.y, target.min.y, target.max.y, radius))
			tryObjectSnapAxis(moving.min.z, moving.max.z, target.min.z, target.max.z, &zDelta, radius);
		if(fabsf(zDelta) <= radius){
			candidateDelta.z = zDelta;
			if(fabsf(zDelta) > 0.0001f)
				candidateAxes++;
		}

		if(candidateAxes == 0)
			continue;
		ObjectSnapScore score;
		score.max = max(fabsf(candidateDelta.x), max(fabsf(candidateDelta.y), fabsf(candidateDelta.z)));
		score.sum = sq(candidateDelta.x) + sq(candidateDelta.y) + sq(candidateDelta.z);
		score.axes = candidateAxes;
		// Prefer the closest target, and prefer a multi-axis correction when
		// it closes a corner in one gesture instead of one axis per frame.
		if(!found || snapScoreBetter(score, best)){
			best = score;
			bestDelta = candidateDelta;
			found = true;
		}
	}

	// Pins and ruler guides use the same snap mode and distance as objects.
	std::vector<rw::V3d> pinTargets;
	CoordinatePinSnapTargets(area, scale(add(moving.min, moving.max), .5f), pinTargets);
	for(const rw::V3d &point : pinTargets){
		if(point.x < moving.min.x - radius || point.x > moving.max.x + radius ||
		   point.y < moving.min.y - radius || point.y > moving.max.y + radius ||
		   point.z < moving.min.z - radius || point.z > moving.max.z + radius) continue;
		rw::V3d delta = {0,0,0};
		int axes = 0;
		for(int axis = 0; axis < 3; axis++){
			float d = Alignment::AxisCorrection((&moving.min.x)[axis], (&moving.max.x)[axis], (&point.x)[axis], radius);
			if(fabsf(d) <= radius){ (&delta.x)[axis] = d; if(fabsf(d) > .0001f) axes++; }
		}
		if(!axes) continue;
		ObjectSnapScore score = { std::max(fabsf(delta.x), std::max(fabsf(delta.y), fabsf(delta.z))),
			dot(delta, delta), axes };
		if(!found || snapScoreBetter(score, best)){ found = true; best = score; bestDelta = delta; }
	}
	if(!found || best.axes == 0)
		return false;
	*outDelta = bestDelta;
	*outScore = best;
	return true;
}

// Selections up to this size snap by each object's own box; bigger ones by
// the box around them all, which keeps a drag of thousands cheap.
#define MAX_OBJECT_SNAP_TESTED 64

// The snap for moving objs (each from startPositions[i]) by delta: the best
// of every object's own snap, so a group snaps by whichever member is
// nearest something, not only by the one under the gizmo.
bool
FindSelectionSnapDelta(const std::vector<ObjectInst*> &objs, const std::vector<rw::V3d> &startPositions,
	const rw::V3d &delta, rw::V3d *outDelta)
{
	if(objs.empty() || objs.size() != startPositions.size() || outDelta == nil || gObjectSnapDistance <= 0.0f)
		return false;
	std::unordered_set<ObjectInst*> movingSet(objs.begin(), objs.end());
	bool found = false;
	ObjectSnapScore best = { 1.0e30f, 1.0e30f, 0 };
	if(objs.size() > MAX_OBJECT_SNAP_TESTED){
		ObjectSnapBounds all;
		bool first = true;
		for(size_t i = 0; i < objs.size(); i++){
			ObjectSnapBounds b;
			rw::V3d pos = add(startPositions[i], delta);
			if(!getObjectSnapBounds(objs[i], &pos, &b))
				continue;
			addObjectSnapPoint(b.min, &all, &first);
			addObjectSnapPoint(b.max, &all, &first);
		}
		return !first && findObjectSnapDeltaForBounds(all, nil, objs[0]->m_area, movingSet, outDelta, &best);
	}
	for(size_t i = 0; i < objs.size(); i++){
		ObjectSnapBounds moving;
		rw::V3d pos = add(startPositions[i], delta);
		if(!getObjectSnapBounds(objs[i], &pos, &moving))
			continue;
		rw::V3d d;
		ObjectSnapScore score;
		if(findObjectSnapDeltaForBounds(moving, objs[i], objs[i]->m_area, movingSet, &d, &score) &&
		   (!found || snapScoreBetter(score, best))){
			best = score;
			*outDelta = d;
			found = true;
		}
	}
	return found;
}

void
dogizmo(void)
{
	gGizmoHovered = false;
	gGizmoUsing = false;
	if(AttachmentTargetPickActive()) return;
	if(MapToolsPickingLine()) return;
	if(LayoutPathsGizmo()) return;
	if(CoordinatePinOwnsMouse()) return;

	if(!gGizmoEnabled)
		return;

	if(!selection.first){
		static bool wasDraggingSaNode = false;
		static rw::V3d dragStartSaNodePos;

		if(!SAPaths::HasSelectedNode()){
			wasDraggingSaNode = false;
			return;
		}

		rw::V3d nodePos;
		if(!SAPaths::GetSelectedNodePosition(&nodePos))
			return;

		rw::Camera *cam = (rw::Camera*)rw::engine->currentCamera;
		float *fview = (float*)&cam->devView;
		float *fproj = (float*)&cam->devProj;
		rw::RawMatrix gizobj;
		rw::RawMatrix::setIdentity(&gizobj);
		gizobj.pos.x = nodePos.x;
		gizobj.pos.y = nodePos.y;
		gizobj.pos.z = nodePos.z;
		float *fobj = (float*)&gizobj;

		ImGuiIO &io = ImGui::GetIO();
		ImVec2 origin = ImGui::GetMainViewport()->Pos;
		ImGuizmo::SetRect(origin.x, origin.y, io.DisplaySize.x, io.DisplaySize.y);

		float snapValues[3];
		float *snapPtr = nil;
		bool useSnap = gGizmoSnap && CPad::IsShiftDown();
		if(useSnap){
			snapValues[0] = gGizmoSnapTranslate;
			snapValues[1] = gGizmoSnapTranslate;
			snapValues[2] = gGizmoSnapTranslate;
			snapPtr = snapValues;
		}

		ImGuizmo::Manipulate(fview, fproj, ImGuizmo::TRANSLATE, ImGuizmo::WORLD, fobj, nil, snapPtr);

		gGizmoHovered = ImGuizmo::IsOver();
		bool isUsing = ImGuizmo::IsUsing();
		gGizmoUsing = isUsing;

		if(isUsing && !wasDraggingSaNode)
			dragStartSaNodePos = nodePos;

		if(isUsing){
			rw::V3d newPos = { gizobj.pos.x, gizobj.pos.y, gizobj.pos.z };
			SAPaths::SetSelectedNodePosition(newPos, false);
		}else if(wasDraggingSaNode){
			rw::V3d finalPos;
			if(SAPaths::GetSelectedNodePosition(&finalPos) &&
			   length(sub(finalPos, dragStartSaNodePos)) >= 0.0001f)
				SAPaths::CommitSelectedNodeEdit();
		}

		wasDraggingSaNode = isUsing;
		return;
	}

	// The pivot is the first selected: the list has the latest first, so
	// Shift+clicking more objects would otherwise move the pivot each time.
	ObjectInst *inst = (ObjectInst*)selection.first->item;
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *candidate = (ObjectInst*)p->item;
		if(!candidate->m_isDeleted && candidate->m_selectSeq < inst->m_selectSeq)
			inst = candidate;
	}
	if(inst->m_isDeleted)
		return;

	static bool wasDragging = false;
	static rw::V3d dragStartLeaderPos;
	static rw::V3d dragStartPivot;
	static rw::Quat dragStartLeaderRot;
	static bool dragStartFollowGround;
	static bool dragStartAlignToSurface;
	static float dragGroundOffset;
	static rw::Quat dragGroundBaseRot;
	// Snapshot of all affected objects for multi-select translate
	static std::vector<UndoTransform> dragTransforms;
	if(!IsPawnObjectEditable(inst)){
		wasDragging = false;
		return;
	}

	rw::Camera *cam;
	rw::RawMatrix gizobj;
	float *fview, *fproj, *fobj;

	// Build object matrix from instance
	rw::convMatrix(&gizobj, &inst->m_matrix);
	rw::V3d pivot = gGizmoPivot==GIZMO_PIVOT_CENTRE ? MapToolsSelectionCentre() :
		gGizmoPivot==GIZMO_PIVOT_CUSTOM ? gGizmoCustomPivot : inst->m_translation;
	// A rotated bounding box can have a different centre. Keep the chosen
	// pivot fixed throughout the gesture instead of chasing the changing box.
	if(wasDragging)
		pivot = gGizmoMode==GIZMO_ROTATE ? dragStartPivot :
			add(dragStartPivot, sub(inst->m_translation, dragStartLeaderPos));
	gizobj.pos.x=pivot.x;gizobj.pos.y=pivot.y;gizobj.pos.z=pivot.z;

	cam = (rw::Camera*)rw::engine->currentCamera;
	fview = (float*)&cam->devView;
	fproj = (float*)&cam->devProj;
	fobj = (float*)&gizobj;

	ImGuiIO &io = ImGui::GetIO();
	ImVec2 origin = ImGui::GetMainViewport()->Pos;
	ImGuizmo::SetRect(origin.x, origin.y, io.DisplaySize.x, io.DisplaySize.y);

	ImGuizmo::OPERATION op = gGizmoMode == GIZMO_ROTATE ? ImGuizmo::ROTATE : ImGuizmo::TRANSLATE;
	float snapValues[3];
	float *snapPtr = nil;
	bool useSnap = gGizmoSnap && CPad::IsShiftDown();
	if(useSnap){
		if(gGizmoMode == GIZMO_ROTATE){
			snapValues[0] = gGizmoSnapAngle;
			snapValues[1] = gGizmoSnapAngle;
			snapValues[2] = gGizmoSnapAngle;
		}else{
			snapValues[0] = gGizmoSnapTranslate;
			snapValues[1] = gGizmoSnapTranslate;
			snapValues[2] = gGizmoSnapTranslate;
		}
		snapPtr = snapValues;
	}
	ImGuizmo::MODE space = gGizmoSpace == GIZMO_WORLD ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
	ImGuizmo::Manipulate(fview, fproj, op, space, fobj, nil, snapPtr);

	gGizmoHovered = ImGuizmo::IsOver();
	bool isUsing = ImGuizmo::IsUsing();
	gGizmoUsing = isUsing;

	// Capture start state when drag begins
	if(isUsing && !wasDragging){
		dragStartPivot=pivot;
		dragStartLeaderPos = inst->m_translation;
		dragStartLeaderRot = NormalizeQuatOrIdentity(inst->m_rotation);
		dragStartFollowGround = gGizmoMode == GIZMO_TRANSLATE && gDragFollowGround;
		dragStartAlignToSurface = dragStartFollowGround && gDragAlignToSurface;
		dragGroundBaseRot = dragStartLeaderRot;
		dragGroundOffset = 0.0f;
		if(dragStartFollowGround){
			rw::V3d hitPos, hitNormal;
			if(GetGroundPlacementSurface(inst->m_translation, &hitPos, &hitNormal, true, InstStandsOnGround(inst)))
				dragGroundOffset = inst->m_translation.z - (hitPos.z - GetMinZOffsetForRotation(inst, inst->m_rotation));
		}

		// Build deduplicated snapshot of all affected objects.
		bool dragOverflow = false;
		CaptureObjectTransformTargets(inst, true, dragTransforms, &dragOverflow);
		if(dragOverflow)
			Toast(TOAST_SELECTION, _("Selection too large: some objects won't move (max %d)"), MAX_BATCH_OBJECTS);
	}
	// Record undo when drag ends
	if(!isUsing && wasDragging){
		std::vector<UndoTransform> finalTransforms;
		finalTransforms.reserve(dragTransforms.size());
		for(int i = 0; i < (int)dragTransforms.size(); i++){
			ObjectInst *obj = dragTransforms[i].inst;
			uint8 flags = 0;
			if(length(sub(obj->m_translation, dragTransforms[i].oldPos)) > 0.0000001f)
				flags |= UNDO_TRANSFORM_POS;
			if(TransformRotationsDiffer(obj->m_rotation, dragTransforms[i].oldRot))
				flags |= UNDO_TRANSFORM_ROT;
			if(flags != 0){
				RemoveInstFromSectors(obj);
				InsertInstIntoSectors(obj);
				StampChangeSeq(obj);
				UndoTransform t = {};
				t.inst = obj;
				t.oldPos = dragTransforms[i].oldPos;
				t.newPos = obj->m_translation;
				t.oldRot = dragTransforms[i].oldRot;
				t.newRot = obj->m_rotation;
				t.flags = flags;
				finalTransforms.push_back(t);
			}else
				obj->m_isDirty = dragTransforms[i].oldDirty;
		}
		if(!finalTransforms.empty())
			UndoRecordTransformBatch(finalTransforms.data(), (int)finalTransforms.size());
	}
	wasDragging = isUsing;

	if(isUsing){
		// Extract position from the gizmo result
		rw::V3d newLeaderPos;
		newLeaderPos.x = gizobj.pos.x;
		newLeaderPos.y = gizobj.pos.y;
		newLeaderPos.z = gizobj.pos.z;
		newLeaderPos=add(dragStartLeaderPos,sub(newLeaderPos,dragStartPivot));

		if(gGizmoMode == GIZMO_TRANSLATE){
			rw::Quat newLeaderRot = inst->m_rotation;
			if(dragStartFollowGround){
				rw::V3d groundHit, groundNormal;
				if(GetGroundPlacementSurface(newLeaderPos, &groundHit, &groundNormal, true, InstStandsOnGround(inst))){
					if(dragStartAlignToSurface)
						newLeaderRot = BuildGroundAlignedRotationFromRotation(dragGroundBaseRot, groundNormal);
					newLeaderPos.z = groundHit.z - GetMinZOffsetForRotation(inst, newLeaderRot) + dragGroundOffset;
				}
			}

			// Compute total delta from leader's start position (avoids frame-by-frame drift)
			rw::V3d totalDelta = sub(newLeaderPos, dragStartLeaderPos);
			if(gObjectSnapMode){
				std::vector<ObjectInst*> moving;
				std::vector<rw::V3d> starts;
				moving.reserve(dragTransforms.size());
				starts.reserve(dragTransforms.size());
				for(int i = 0; i < (int)dragTransforms.size(); i++){
					moving.push_back(dragTransforms[i].inst);
					starts.push_back(dragTransforms[i].oldPos);
				}
				rw::V3d snapDelta;
				if(FindSelectionSnapDelta(moving, starts, totalDelta, &snapDelta))
					totalDelta = add(totalDelta, snapDelta);
			}

			// Pass 1: compute new positions and rotations from snapshot
			if(gGizmoPivot==GIZMO_PIVOT_CUSTOM)gGizmoCustomPivot=add(dragStartPivot,totalDelta);
			// (don't update matrices yet, so raycasts see old collision state)
			for(int i = 0; i < (int)dragTransforms.size(); i++){
				ObjectInst *obj = dragTransforms[i].inst;
				rw::V3d newPos = add(dragTransforms[i].oldPos, totalDelta);
				obj->m_rotation = dragTransforms[i].oldRot;

				if(dragStartFollowGround && obj != inst){
					// Per-object ground follow: each object individually snaps to terrain
					rw::V3d groundHit, groundNormal;
					if(GetGroundPlacementSurface(newPos, &groundHit, &groundNormal, true, InstStandsOnGround(obj))){
						rw::Quat rot = dragTransforms[i].oldRot;
						if(dragStartAlignToSurface)
							rot = BuildGroundAlignedRotationFromRotation(dragTransforms[i].oldRot, groundNormal);
						newPos.z = groundHit.z - GetMinZOffsetForRotation(obj, rot);
						obj->m_rotation = rot;
					}
				}

				// Trains slide along the track (Alt: anywhere).
				rw::V3d onTrack;
				if(obj->m_isVehicle && IsTrainEngineModel(obj->m_objectId) && !CPad::IsAltDown() &&
				   SnapTrainToTrack(obj->m_objectId, newPos, GetTrainInstAngle(obj), &onTrack))
					newPos = onTrack;

				obj->m_translation = newPos;
				obj->m_isDirty = true;
			}

			// Leader align-to-surface (uses its own ground-followed rotation)
			if(dragStartAlignToSurface)
				inst->m_rotation = newLeaderRot;

			// Pass 2: update all matrices and frames at once
			for(int i = 0; i < (int)dragTransforms.size(); i++){
				dragTransforms[i].inst->UpdateMatrix();
				updateRwFrame(dragTransforms[i].inst);
			}
		}else if(gGizmoMode == GIZMO_ROTATE){
			// Extract leader's new rotation from gizmo result
			inst->m_matrix.right.x = gizobj.right.x;
			inst->m_matrix.right.y = gizobj.right.y;
			inst->m_matrix.right.z = gizobj.right.z;
			inst->m_matrix.up.x = gizobj.up.x;
			inst->m_matrix.up.y = gizobj.up.y;
			inst->m_matrix.up.z = gizobj.up.z;
			inst->m_matrix.at.x = gizobj.at.x;
			inst->m_matrix.at.y = gizobj.at.y;
			inst->m_matrix.at.z = gizobj.at.z;
			rw::Quat newLeaderRot = QuatFromMatrix(inst->m_matrix);

			for(int i = 0; i < (int)dragTransforms.size(); i++){
				ObjectInst *obj = dragTransforms[i].inst;
				ComputeGizmoRotation(dragTransforms[i].oldPos, dragTransforms[i].oldRot,
					dragStartLeaderRot, newLeaderRot, dragStartPivot,
					gGizmoPivot==GIZMO_PIVOT_INDIVIDUAL, gGizmoSpace==GIZMO_LOCAL,
					&obj->m_translation, &obj->m_rotation);
				obj->m_isDirty = true;
				obj->UpdateMatrix();
				updateRwFrame(obj);
			}
		}
	}
}

static uint64 frameCounter;

void
updateFPS(void)
{
	static float history[100];
	static float total;
	static int n;
	static int i;

	total += timeStep - history[i];
	history[i] = timeStep;
	i = (i+1) % 100;
	n = i > n ? i : n;
	avgTimeStep = total / n;
}

// The sky, the world and the editor's markers in it.
static void
renderWorld(void)
{
	if(gRenderBackground){
		SetRenderState(rw::ALPHATESTREF, 0);
		SetRenderState(rw::CULLMODE, rw::CULLNONE);
		rw::RGBA skytop, skybot;
		rw::convColor(&skytop, &Timecycle::currentColours.skyTop);
		rw::convColor(&skybot, &Timecycle::currentColours.skyBottom);
		if(params.background == GAME_SA){
			Clouds::RenderSkyPolys();
			Clouds::RenderLowClouds();
		}else{
			Clouds::RenderBackground(skytop.red, skytop.green, skytop.blue,
				skybot.red, skybot.green, skybot.blue, 255);
			Clouds::RenderLowClouds();
			if(params.timecycle != GAME_VCS)
				Clouds::RenderFluffyClouds();
			Clouds::RenderHorizon();
		}
		SetRenderState(rw::ALPHATESTREF, params.alphaRef);
	}

	rw::SetRenderState(rw::FOGENABLE, gEnableFog);
	RenderOpaque();
	RenderAttachmentWorkspacePreview();
	if(gRenderWater)
		if(!AttachmentWorkspaceHidesWorld())
			WaterLevel::Render();
	RenderTransparent();
	RenderAttachmentBoneMarker();
	if(gRenderLightEffects)
		Effects::RenderLights();
	ParticlePreview::Render(timeStep);

	if(gPlaceMode && GetSpawnObjectId() >= 0){
		ImGuiIO &io = ImGui::GetIO();
		if(!io.WantCaptureMouse){
			rw::V3d hitPos, hitNormal;
			if(GetPlacementSurfaceHit(&hitPos, &hitNormal)){
				rw::V3d pos = hitPos;
				pos.z += GetElementPlacementBaseOffset(GetSpawnElementKind(), GetSpawnObjectId());
				rw::Quat rot = { 0.0f, 0.0f, 0.0f, 1.0f };
				rw::RGBA col = { 80, 220, 120, 110 };
				TrainCar cars[TRAIN_MAX_CARS];
				if(ElementKindUsesIcon(GetSpawnElementKind()))
					RenderElementIconGhost(GetSpawnElementKind(), pos);
				else if(IsTrainEngineModel(GetSpawnObjectId()) && !CPad::IsAltDown() &&
				   ComputeTrainCars(GetSpawnObjectId(), pos, 0.0f, cars) > 0)
					RenderTrainPlacementGhost(GetSpawnObjectId(), pos, 0.0f);
				else
					RenderPlacementGhost(GetSpawnObjectId(), pos, rot, col);
			}
		}
	}
	if(gPrefabPlaceMode){
		ImGuiIO &io = ImGui::GetIO();
		if(!io.WantCaptureMouse){
			rw::V3d hitPos, hitNormal;
			if(GetPlacementSurfaceHit(&hitPos, &hitNormal))
				RenderPrefabPlacementGhost(GetPrefabPlacePath(), hitPos, gPrefabPlaceHeading);
		}
	}
	// DEBUG render object picking
	//RenderEverythingColourCoded();


	if(gRenderPostFX)
		RenderPostFX();

	DefinedState();
	rw::SetRenderState(rw::FOGENABLE, 0);

	SetRenderState(rw::CULLMODE, rw::CULLNONE);

	if(gDrawTarget)
		TheCamera.DrawTarget();
	if(gRenderCollision || gRenderBounds)
		RenderEverythingCollisions();
	if(gRenderTimecycleBoxes)
		Timecycle::RenderBoxes();
	if(gRenderZones)
		Zones::Render();
	RenderTrainTracksOverlay();
	if(gRenderCullZones)
		Zones::RenderCullZones();
	if(gRenderAttribZones)
		Zones::RenderAttribZones();
	Path::hoveredNode = nil;
	SAPaths::hoveredNode = nil;
	Effects::hoveredEffect = nil;
	if(gRenderLegacyPedPaths)
		Path::RenderPedPaths();
	if(gRenderLegacyCarPaths)
		Path::RenderCarPaths();
	if(gRenderSaPedPaths)
		SAPaths::RenderPedPaths();
	if(gRenderSaCarPaths)
		SAPaths::RenderCarPaths();
	if(gRenderSaAreaGrid)
		SAPaths::RenderAreaGrid();
	if(gRenderEffects)
		Effects::Render();

	rw::SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAALWAYS);	// don't mess up GUI
	RenderObjectOutlines();
	// This fucks up the z buffer, but what else can we do?
	RenderDebugLines();
}

void
Draw(void)
{
	const auto frameStart = std::chrono::steady_clock::now();
	static rw::RGBA clearcol = { 0x80, 0x80, 0x80, 0xFF };

	SyncEditorWindowSize();

	CPad *pad = CPad::GetPad(0);
	if(pad->NewState.start && pad->NewState.select){
		sk::globals.quit = 1;
		return;
	}

	// HACK: we load a lot in the first frame
	// which messes up the average
	if(frameCounter == 0)
		timeStep = 1/30.0f;

	if(!gOverrideBlurAmb)
		gUseBlurAmb = gRenderPostFX;

	updateFPS();

	Weather::Update();
	Timecycle::Update();
	Timecycle::SetLights();

	UpdateDayNightBalance();

	// Fade the outer half of the common range into the horizon. Fog follows
	// the distance slider, independent of weather, model and local zones.
	TheCamera.m_rwcam->setFarPlane(gStreamDistance);
	TheCamera.m_rwcam->fogPlane = gStreamDistance*0.5f;
	TheCamera.m_rwcam_viewer->setFarPlane(gStreamDistance);
	TheCamera.m_rwcam_viewer->fogPlane = gStreamDistance*0.5f;
	gEnableFog = gFogMode != FOG_OFF;

	CPad::UpdatePads();
	// TextDraw mode hides the world; the attachment workspace can render its
	// isolated target and attached objects against the empty clear colour.
	bool noWorld = TextDrawModeHidesWorld();
	bool attachmentVoid = AttachmentWorkspaceHidesWorld();
	bool noMapEditing = InTextDrawMode();
	if(!noMapEditing){
		updateRectSelectEarly();
		TheCamera.Process();
	}
	AgentBridgeUpdate();
	MultiplayerUpdate();
	TelemetryUpdate();
	CrashReportsUpdate();
	{
		// What a crash report says Haumea was doing.
		const char *mode = InTextDrawMode() ? "textdraw" : gPlaceMode ? "place" : gPrefabPlaceMode ? "prefab" :
			gBrushMode ? "brush" : "edit";
		ImGuiContext *g = ImGui::GetCurrentContext();
		const char *window = g && g->NavWindow ? g->NavWindow->Name : "";
		CrashReportsSetContext(mode, window);
	}
	TheCamera.update();
	if(gUseViewerCam)
		Scene.camera = TheCamera.m_rwcam_viewer;
	else
		Scene.camera = TheCamera.m_rwcam;
	// Keep PS2's framebuffer-only alpha emulation out of the offscreen browser
	// passes. It performs an extra depth-mask pass for blended materials and is
	// intended for the visible world, not thumbnails or the selected preview.
	int previewGsAlphaTest = rw::GetRenderState(rw::GSALPHATEST);
	rw::SetRenderState(rw::GSALPHATEST, 0);
	if(GetSpawnObjectId() >= 0)
		RenderPreviewObject(GetSpawnObjectId());
	RenderRequestedObjectThumbnails();
	RenderRequestedPrefabThumbnails();
	TextDrawsBeforeWorldRender();
	rw::SetRenderState(rw::GSALPHATEST, previewGsAlphaTest);

	Scene.camera->beginUpdate();

	DefinedState();

	I18nNewFrame();	// a language chosen in Preferences applies from here
	ImGui_ImplRW_NewFrame(timeStep);
	ImGuizmo::BeginFrame();

	// Mouse wheel over the 3D viewport adjusts FOV, Ctrl+wheel anywhere the
	// interface size. Both have keys in the shortcut table too.
	// Must run after NewFrame: AddMouseWheelEvent() only queues the event, and
	// io.MouseWheel is populated by NewFrame and cleared by EndFrame.
	{
		ImGuiIO &io = ImGui::GetIO();
		// Placing a prefab: Q/E (or , and .) and the wheel turn it, 15 degrees
		// a step, 5 with Shift.
		if(gPrefabPlaceMode && !io.WantTextInput){
			float step = io.KeyShift ? 5.0f : 15.0f;
			if(ImGui::IsKeyPressed(ImGuiKey_Q) || ImGui::IsKeyPressed(ImGuiKey_Comma))
				gPrefabPlaceHeading += step;
			if(ImGui::IsKeyPressed(ImGuiKey_E) || ImGui::IsKeyPressed(ImGuiKey_Period))
				gPrefabPlaceHeading -= step;
		}
		if(io.MouseWheel != 0.0f && io.KeyMods == ImGuiMod_Ctrl)
			StepUiScale(io.MouseWheel > 0.0f ? 1.0f : -1.0f);
		else if(gPrefabPlaceMode && !io.WantCaptureMouse && io.MouseWheel != 0.0f)
			gPrefabPlaceHeading += (io.MouseWheel > 0.0f ? 1.0f : -1.0f) * (io.KeyShift ? 5.0f : 15.0f);
		else if(!io.WantCaptureMouse && io.MouseWheel != 0.0f && !noMapEditing){
			TheCamera.m_fov = ClampCameraFov(TheCamera.m_fov - io.MouseWheel * gFovWheelStep);
		}
	}

	LoadAllRequestedObjects();
	UpdateAttachmentWorkspace();
	// Attached objects follow last frame's edits before the scene is listed.
	UpdateObjectAttachments();
	// .rec playback poses its elements for this frame.
	RecPlayerUpdate();
	UpdateNpcVehicleSeats();
	NpcPreviewUpdate(timeStep);
	TestServerUpdate();
	AnimUpdate(timeStep);
	PlayerViewUpdate();
	if(!noWorld || attachmentVoid)
		BuildRenderList();

	// Has to be called for highlighting some objects
	// but also can mess with timecycle mid frame :/
	gui();

	if(!noMapEditing){
		dogizmo();
		handleTool();
		handleRectSelect();
	}

	DefinedState();
	Scene.camera->clear(&clearcol, rw::Camera::CLEARIMAGE|rw::Camera::CLEARZ);
	if(!noWorld && !attachmentVoid)
		renderWorld();
	else if(attachmentVoid){
		rw::RGBAf oldAmbient = pAmbient->color;
		rw::RGBAf oldDirect = pDirect->color;
		uint32 oldDirectFlags = pDirect->getFlags();
		pAmbient->setColor(0.72f, 0.72f, 0.72f);
		pDirect->setColor(0.52f, 0.52f, 0.52f);
		pDirect->setFlags(rw::Light::LIGHTATOMICS);
		RenderOpaque();
		RenderAttachmentWorkspacePreview();
		RenderTransparent();
		RenderAttachmentBoneMarker();
		RenderAttachmentWorkspaceGrid();
		pDirect->setFlags(oldDirectFlags);
		pDirect->setColor(oldDirect.red, oldDirect.green, oldDirect.blue);
		pAmbient->setColor(oldAmbient.red, oldAmbient.green, oldAmbient.blue);
	}
	rw::SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAALWAYS);	// don't mess up GUI
	TextDrawsAfterWorldRender();
	AgentBridgeCaptureAfterWorldRender();
	ImGui::EndFrame();
	ImGui::Render();

	ImGui_ImplRW_RenderDrawLists(ImGui::GetDrawData());
	UiScriptAfterRender();	// --ui-script frame grabs
	AgentBridgeCaptureAfterUiRender();

	Scene.camera->endUpdate();
	Scene.camera->showRaster(gVSync ? rw::Raster::FLIPWAITVSYNCH : 0);
#if defined(_WIN32) && defined(RW_D3D9)
	RenderDetachedEditorWindows();
#endif
	if(gFrameLimit > 0){
		auto budget = std::chrono::duration<double>(1.0/gFrameLimit);
		std::this_thread::sleep_until(frameStart + std::chrono::duration_cast<std::chrono::steady_clock::duration>(budget));
	}
	frameCounter++;
}

void
Idle(void)
{
	static int state = 0;
	switch(state){
	case 0:
		if(WorkspaceSetupNeeded()){
			strncpy(splashStatus, _("Choose the GTA San Andreas folder"), sizeof(splashStatus) - 1);
			DrawSplash(1/60.0f);
			break;
		}
		LoadGame();
		ExpandSplashWindow();
		state = 1;
		break;
	case 1:
		Draw();
		break;
	}
}
