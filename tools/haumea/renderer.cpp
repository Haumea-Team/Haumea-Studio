#define WITH_D3D
#include "haumea.h"
#include "editoricons.h"
#include "radarart.h"
#include "trains.h"
#include "playerview.h"
#include "templates.h"
#include <algorithm>
#include <vector>
#include <unordered_map>

struct InstDist
{
	ObjectInst *inst;
	union { float dist, sort; };
};

static std::vector<ObjectInst*> visibleInsts;
static std::vector<InstDist> sortedInsts;
static std::vector<InstDist> lodList;

static uint16 currentScanCode;

static rw::ObjPipeline *colourCodePipe;

static ObjectInst*
findClosestRayHitInVisibleList(const std::vector<ObjectInst*> &insts, const Ray &ray,
	rw::V3d *bestHit, float *bestT)
{
	ObjectInst *bestInst = nil;

	for(ObjectInst *inst : insts){
		ObjectDef *obj = GetObjectDef(inst->m_objectId);
		if(obj == nil || obj->m_colModel == nil)
			continue;

		rw::V3d hitPos;
		if(!IntersectRayColModel(ray, inst, &hitPos))
			continue;

		float t = dot(sub(hitPos, ray.start), ray.dir);
		if(t < 0.0f || t >= *bestT)
			continue;

		*bestT = t;
		*bestHit = hitPos;
		bestInst = inst;
	}

	return bestInst;
}

static ObjectInst*
findClosestRayHitInSortedList(const std::vector<InstDist> &insts, const Ray &ray,
	rw::V3d *bestHit, float *bestT)
{
	ObjectInst *bestInst = nil;

	for(const InstDist &entry : insts){
		ObjectInst *inst = entry.inst;
		ObjectDef *obj = GetObjectDef(inst->m_objectId);
		if(obj == nil || obj->m_colModel == nil)
			continue;

		rw::V3d hitPos;
		if(!IntersectRayColModel(ray, inst, &hitPos))
			continue;

		float t = dot(sub(hitPos, ray.start), ray.dir);
		if(t < 0.0f || t >= *bestT)
			continue;

		*bestT = t;
		*bestHit = hitPos;
		bestInst = inst;
	}

	return bestInst;
}

enum Visibility
{
	VIS_INVISIBLE,
	VIS_VISIBLE,
	VIS_CULLED,
	VIS_STREAMME
};

static void
AddToRenderList(ObjectInst *inst, float dist)
{
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil) return;
	// Labels, sounds, map icons, gang zones and path points are drawn as
	// editor icons, never as their stand-in model (editoricons.cpp).
	if(InstUsesEditorIcon(inst)){
		QueueElementIcon(inst);
		return;
	}
	if(inst->m_rwObject == nil) return;

	inst->PreRender();
	if(obj->m_drawLast){
		InstDist e;
		e.inst = inst;
		e.dist = dist;
		sortedInsts.push_back(e);
	}else
		visibleInsts.push_back(inst);
}

// This is a list of potentially rendered LODs
static void
AddToLodRenderList(ObjectInst *inst, float dist)
{
	// Selection must still resolve HD/LOD
	// coverage. Bypassing this queue drew the selected building's LOD on top.
	InstDist e;
	e.inst = inst;
	e.dist = dist;
	lodList.push_back(e);
}

// One hard range for every model, including selected objects. Individual
// IDE distances, weather, altitude and projected size never shorten it.
static bool
beyondDrawDistance(float camdist)
{
	return camdist >= gStreamDistance;
}

// Don't assume there are LODs, just draw if below draw distance
static int
SetupVisibilitySimple(ObjectInst *inst, float *distout)
{
	ObjectDef *obj, *hdobj;
	float camdist;
	rw::Atomic *atm, *instatm;
	rw::Clump *instclump;

	obj = GetObjectDef(inst->m_objectId);
	if(obj == nil) return VIS_INVISIBLE;
	camdist = TheCamera.distanceTo(inst->m_translation);
	if(beyondDrawDistance(camdist))
		return VIS_INVISIBLE;

	// Selection bypasses area/time filters, but keeps the common distance limit
	if(!inst->m_selected && (inst->m_highlight == HIGHLIGHT_NONE || inst->m_highlight == HIGHLIGHT_MATCH)){
		if(!gNoAreaCull && inst->m_area != currentArea && inst->m_area != 13)
			return VIS_INVISIBLE;

		hdobj = inst->m_isBigBuilding ? obj->m_relatedModel : nil;

		if(obj->m_isHidden)
			return VIS_INVISIBLE;


		if(!gNoTimeCull && obj->m_isTimed && !IsHourInRange(obj->m_timeOn, obj->m_timeOff))
			return VIS_INVISIBLE;
	}else
		camdist = TheCamera.distanceTo(inst->m_translation);
	*distout = camdist;

	if(!obj->IsLoaded())
		return VIS_STREAMME;

	if(!inst->IsOnScreen())
		return VIS_CULLED;

	instatm = nil;
	instclump = nil;
	if(inst->m_rwObject == nil)
		if(inst->CreateRwObject() == nil)
			return VIS_INVISIBLE;	// this shouldn't happen

	if(obj->m_type == ObjectDef::ATOMIC){
		atm = obj->GetAtomicForDist(0.0f);
		if(atm == nil)	// this shouldn't happen but it does (in xbox III)
			return VIS_INVISIBLE;
		instatm = (rw::Atomic*)inst->m_rwObject;
		if(instatm->geometry != atm->geometry)
			inst->SetBaseGeometry(atm->geometry);
	}

	return VIS_VISIBLE;
}

static int
SetupVisibilityIII(ObjectInst *inst, float *distout)
{
	ObjectDef *obj, *hdobj;
	float camdist;
	rw::Atomic *atm, *instatm;
	rw::Clump *instclump;

	obj = GetObjectDef(inst->m_objectId);
	if(obj == nil) return VIS_INVISIBLE;
	camdist = TheCamera.distanceTo(inst->m_translation);
	if(beyondDrawDistance(camdist))
		return VIS_INVISIBLE;

	// Selection bypasses area/time filters, but keeps the common distance limit
	if(!inst->m_selected && (inst->m_highlight == HIGHLIGHT_NONE || inst->m_highlight == HIGHLIGHT_MATCH)){
		if(!gNoAreaCull && inst->m_area != currentArea && inst->m_area != 13)
			return VIS_INVISIBLE;

		hdobj = inst->m_isBigBuilding ? obj->m_relatedModel : nil;

		if(obj->m_isHidden)
			return VIS_INVISIBLE;

		if(inst->m_isBigBuilding && hdobj)
			if(hdobj->IsLoaded())
				return VIS_INVISIBLE;

		if(!gNoTimeCull && obj->m_isTimed && !IsHourInRange(obj->m_timeOn, obj->m_timeOff))
			return VIS_INVISIBLE;
	}else
		camdist = TheCamera.distanceTo(inst->m_translation);
	*distout = camdist;

	if(!obj->IsLoaded())
		return VIS_STREAMME;

	if(!inst->IsOnScreen())
		return VIS_CULLED;

	instatm = nil;
	instclump = nil;
	if(inst->m_rwObject == nil)
		if(inst->CreateRwObject() == nil)
			return VIS_INVISIBLE;	// this shouldn't happen

	if(obj->m_type == ObjectDef::ATOMIC){
		atm = obj->GetAtomicForDist(0.0f);
		if(atm == nil)	// this shouldn't happen but it does (in xbox III)
			return VIS_INVISIBLE;
		instatm = (rw::Atomic*)inst->m_rwObject;
		if(instatm->geometry != atm->geometry)
			inst->SetBaseGeometry(atm->geometry);
	}

	return VIS_VISIBLE;
}

static int
SetupVisibilitySA(ObjectInst *inst, float camdist)
{
	ObjectDef *obj;
	ObjectInst *lodinst;
	rw::Atomic *atm, *instatm;
	rw::Clump *instclump;

	obj = GetObjectDef(inst->m_objectId);
	if(obj == nil) return VIS_INVISIBLE;
	camdist = TheCamera.distanceTo(inst->m_translation);
	if(beyondDrawDistance(camdist))
		return VIS_INVISIBLE;
	lodinst = inst->m_lod;

	// Selection and player view can bypass area/time filters. The range is shared.
	if(!inst->m_selected && (inst->m_highlight == HIGHLIGHT_NONE || inst->m_highlight == HIGHLIGHT_MATCH) && !PlayerViewShows(inst)){
		if(!gNoAreaCull && inst->m_area != currentArea && inst->m_area != 13)
			return VIS_INVISIBLE;

		if(obj->m_isHidden)
			return VIS_INVISIBLE;


		if(!gNoTimeCull && obj->m_isTimed && !IsHourInRange(obj->m_timeOn, obj->m_timeOff))
			return VIS_INVISIBLE;
	}else
		camdist = TheCamera.distanceTo(inst->m_translation);

	if(!obj->IsLoaded())
		return VIS_STREAMME;

	instatm = nil;
	instclump = nil;
	if(inst->m_rwObject == nil)
		if(inst->CreateRwObject() == nil)
			return VIS_INVISIBLE;	// this shouldn't happen

	if(obj->m_type == ObjectDef::ATOMIC){
		atm = obj->GetAtomicForDist(0.0f);
		if(atm == nil)	// this shouldn't happen but it does (in xbox III)
			return VIS_INVISIBLE;
		instatm = (rw::Atomic*)inst->m_rwObject;
		if(instatm->geometry != atm->geometry)
			inst->SetBaseGeometry(atm->geometry);
	}

	int ret = VIS_VISIBLE;
	if(!inst->IsOnScreen())
		ret = VIS_CULLED;	// we want to render but we're culled

	if(lodinst == nil || ret != VIS_VISIBLE)
		return ret;

	// Only visible HD children should suppress the fallback LOD.
	lodinst->m_numChildrenRendered++;
	if(lodinst->m_numChildren > 1){
		AddToLodRenderList(inst, camdist);
		return VIS_INVISIBLE;
	}
	return ret;
}

static int
SetupBigBuildingVisibilitySA(ObjectInst *inst, float *distout)
{
	int v;
	ObjectInst *lodinst;
	float camdist;

	lodinst = inst->m_lod;

	camdist = TheCamera.distanceTo(inst->m_translation);
	*distout = camdist;
	if(inst->m_numChildrenRendered > 0){
		// There is something to render here...

		// tell LOD parent that we (or children) want to be rendered
		if(lodinst)
			lodinst->m_numChildrenRendered++;

		// Defer every fallback until all of its children have been scanned.
		// Keep the count until ProcessLodList, including single-child LODs.
		AddToLodRenderList(inst, camdist);
		return VIS_INVISIBLE;
	}else{
		// No child wants to be rendered (yet?)
		// but maybe this LOD...

		v = SetupVisibilitySA(inst, camdist);

		if(v == VIS_VISIBLE && inst->m_numChildren > 0){
			// Children may be visited later in the big-building scan.
			AddToLodRenderList(inst, camdist);
			return VIS_INVISIBLE;
		}
		return v;
	}
}

static int
SetupHdVisibilitySA(ObjectInst *inst, float *distout)
{
	float camdist = TheCamera.distanceTo(inst->m_translation);
	*distout = camdist;
	return SetupVisibilitySA(inst, camdist);
}

static void
ProcessLodList(void)
{
	// Resolve coverage before drawing any queued LOD. A visible detailed
	// child also covers its ancestors; this must not depend on sector order.
	for(const InstDist &entry : lodList){
		if(entry.inst->m_numChildrenRendered == 0)
			continue;
		ObjectInst *parent = entry.inst->m_lod;
		for(int depth = 0; parent && depth < 32; depth++, parent = parent->m_lod)
			parent->m_numChildrenRendered = max((int)parent->m_numChildrenRendered, 1);
	}
	for(const InstDist &entry : lodList){
		ObjectInst *inst = entry.inst;
		// As in SA's ProcessLodRenderLists, don't overlay a coarse LOD on
		// any of its visible detailed children. Frustum-culled children do
		// not count, and unloaded children leave their fallback available.
		if(inst->m_numChildrenRendered > 0)
			continue;
		if(inst->m_rwObject == nil && inst->CreateRwObject() == nil)
			continue;
		if(inst->IsOnScreen())
			AddToRenderList(inst, entry.dist);
	}
}

static rw::RGBA highlightColor;
static uint8 highlightAlpha = 128;

static void
SetClumpRenderCB(rw::Clump *clump, rw::Atomic::RenderCB cb)
{
	if(clump == nil)
		return;
	FORLIST(lnk, clump->atomics){
		rw::Atomic *atomic = rw::Atomic::fromClump(lnk);
		atomic->setRenderCB(cb);
	}
}

void
RenderPlacementGhost(int objectId, rw::V3d position, rw::Quat rotation, rw::RGBA color)
{
	if(colourCodePipe == nil || objectId < 0)
		return;

	ObjectDef *obj = GetObjectDef(objectId);
	if(obj == nil)
		return;
	if(!obj->IsLoaded()){
		RequestObject(objectId);
		return;
	}

	// A few models at once: a train's engine and carriages, a prefab's parts.
	struct Ghost { int objectId; rw::Atomic *atomic; rw::Clump *clump; uint32 used; };
	static Ghost ghosts[8];
	static uint32 useCount;
	Ghost *ghost = nil;
	for(Ghost &g : ghosts)
		if(g.objectId == objectId && (g.atomic || g.clump))
			ghost = &g;
	if(ghost == nil){
		ghost = &ghosts[0];
		for(Ghost &g : ghosts)
			if(g.used < ghost->used)
				ghost = &g;
		if(ghost->atomic){
			ghost->atomic->getFrame()->destroy();
			ghost->atomic->destroy();
		}
		if(ghost->clump)
			ghost->clump->destroy();
		ghost->atomic = nil;
		ghost->clump = nil;
		ghost->objectId = objectId;

		if(obj->m_type == ObjectDef::ATOMIC && obj->m_atomics[0]){
			ghost->atomic = obj->m_atomics[0]->clone();
			if(ghost->atomic){
				rw::Frame *f = rw::Frame::create();
				if(f)
					ghost->atomic->setFrame(f);
				else{
					ghost->atomic->destroy();
					ghost->atomic = nil;
				}
			}
		}else if(obj->m_type == ObjectDef::CLUMP && obj->m_clump){
			ghost->clump = obj->m_clump->clone();
			SetClumpRenderCB(ghost->clump, myRenderCB);
		}
		if(ghost->atomic)
			ghost->atomic->setRenderCB(myRenderCB);
	}
	ghost->used = ++useCount;
	rw::Atomic *ghostAtomic = ghost->atomic;
	rw::Clump *ghostClump = ghost->clump;

	if(ghostAtomic == nil && ghostClump == nil)
		return;

	rw::Matrix matrix;
	BuildInstanceMatrix(&matrix, rotation, position, IsIplMapDocumentOpen());

	int32 zwrite = GetRenderState(rw::ZWRITEENABLE);
	int32 fog = rw::GetRenderState(rw::FOGENABLE);
	int32 vertexAlpha = GetRenderState(rw::VERTEXALPHA);
	int32 aref = GetRenderState(rw::ALPHATESTREF);
	uint32 oldCode = gta::renderColourCoded;
	rw::RGBA oldColor = gta::colourCode;

	SetRenderState(rw::ZWRITEENABLE, 0);
	SetRenderState(rw::FOGENABLE, 0);
	SetRenderState(rw::VERTEXALPHA, 1);
	SetRenderState(rw::ALPHATESTREF, 0);
	gta::renderColourCoded = 1;
	gta::colourCode = color;

	if(ghostAtomic){
		ghostAtomic->getFrame()->transform(&matrix, rw::COMBINEREPLACE);
		ghostAtomic->render();
	}else if(ghostClump){
		ghostClump->getFrame()->transform(&matrix, rw::COMBINEREPLACE);
		ghostClump->render();
	}

	gta::colourCode = oldColor;
	gta::renderColourCoded = oldCode;
	SetRenderState(rw::ZWRITEENABLE, zwrite);
	SetRenderState(rw::FOGENABLE, fog);
	SetRenderState(rw::VERTEXALPHA, vertexAlpha);
	SetRenderState(rw::ALPHATESTREF, aref);
}

// An atomic in one colour with its alpha blended (checkpoint markers). The
// colour-code pipeline turns blending off only in the pick pass, so this
// draws through it outside that pass.
void
RenderAtomicTinted(rw::Atomic *atomic, rw::RGBA colour)
{
	if(colourCodePipe == nil || atomic == nil)
		return;
	bool oldCoded = gta::renderColourCoded;
	rw::RGBA oldColour = gta::colourCode;
	gta::renderColourCoded = false;
	gta::colourCode = colour;
	colourCodePipe->render(atomic);
	gta::colourCode = oldColour;
	gta::renderColourCoded = oldCoded;
}

void
myRenderCB(rw::Atomic *atomic)
{
	if(gta::renderColourCoded)
		colourCodePipe->render(atomic);
	else if(highlightColor.red || highlightColor.green || highlightColor.blue){
		atomic->getPipeline()->render(atomic);
		gta::colourCode = highlightColor;
		gta::colourCode.alpha = highlightAlpha;
		int32 zwrite, fog, aref;
		zwrite = GetRenderState(rw::ZWRITEENABLE);
		fog = rw::GetRenderState(rw::FOGENABLE);
		aref = rw::GetRenderState(rw::ALPHATESTREF);
		SetRenderState(rw::ZWRITEENABLE, 0);
		SetRenderState(rw::FOGENABLE, 0);
		SetRenderState(rw::ALPHATESTREF, 10);
		colourCodePipe->render(atomic);
		SetRenderState(rw::ZWRITEENABLE, zwrite);
		SetRenderState(rw::FOGENABLE, fog);
		SetRenderState(rw::ALPHATESTREF, aref);
	}else
		atomic->getPipeline()->render(atomic);
}

// An object whose material colour took its prelighting away, as the
// SA-MP client does: the game lights it like any dynamic object, with the
// object ambient and the directional light, not like a building.
void
LitObjectRenderCB(rw::Atomic *atomic)
{
	if(gta::renderColourCoded){
		myRenderCB(atomic);
		return;
	}
	// As SA lights an object (gta-reversed CEntity::SetupLighting and
	// SetLightColoursForPedsCarsAndObjects): the objects' ambient and the
	// directional light (illumination * 0.99609375, SetLightsWithTimeOfDayColour), both
	// times GenerateLightsAffectingObject's 1 in the open * 0.5.
	const float lighting = 0.5f;
	rw::RGBAf ambient = pAmbient->color, direct = pDirect->color;
	uint32 directFlags = pDirect->getFlags();
	rw::RGBAf object = Timecycle::GetObjectAmbient();
	pAmbient->setColor(object.red * lighting, object.green * lighting, object.blue * lighting);
	const float sun = lighting * 0.99609375f;
	pDirect->setColor(direct.red * sun, direct.green * sun, direct.blue * sun);
	pDirect->setFlags(rw::Light::LIGHTATOMICS);
	myRenderCB(atomic);
	pDirect->setFlags(directFlags);
	pDirect->setColor(direct.red, direct.green, direct.blue);
	pAmbient->setColor(ambient.red, ambient.green, ambient.blue);
}

static void
RenderInst(ObjectInst *inst)
{
	static rw::RGBA black = { 0, 0, 0, 255 };
	static rw::RGBA red = { 255, 0, 0, 255 };
	static rw::RGBA green = { 0, 255, 0, 255 };
	static rw::RGBA blue = { 0, 0, 255, 255 };
	static rw::RGBA yellow = { 255, 220, 60, 255 };
	static rw::RGBA highlightCols[] = { black, green, red, blue, yellow };
	ObjectDef *obj;

//	if(!inst->m_isUnimportant && !inst->m_isUnderWater && !inst->m_isTunnel && !inst->m_isTunnelTransition)
//		return;
	obj = GetObjectDef(inst->m_objectId);
	// The render list is built before the UI runs, and a model swap there
	// (a skin or pickup model change) replaces the RW object; when the new
	// model can't be built (a missing or broken DFF) there is none left.
	if(obj == nil || inst->m_rwObject == nil) return;
	if(InstUsesEditorIcon(inst))
		NoteElementProxyModelDraw();	// never: they are queued as icons instead
	obj->m_hasPreRendered = false;

	uint32 cull;
	if(obj->m_noBackfaceCulling){
		cull = GetRenderState(rw::CULLMODE);
		SetRenderState(rw::CULLMODE, rw::CULLNONE);
	}

	// The isolated attachment view uses a simple preview rig so models stay
	// readable without the map's sky and surrounding world lighting.
	pDirect->setFlags(AttachmentWorkspaceHidesWorld() ? rw::Light::LIGHTATOMICS : 0);
	gta::colourCode.red = inst->m_id & 0xFF;
	gta::colourCode.green = inst->m_id>>8 & 0xFF;
	gta::colourCode.blue = inst->m_id>>16 & 0xFF;
	gta::colourCode.alpha = 255;

	// Showing its material indexes, it isn't tinted: the outline still marks it.
	// A building drawn in place of its selected LOD (close up) is tinted as the
	// selection too, or flying in would make the selection look unselected.
	bool selected = inst->m_selected || (inst->m_lod && inst->m_lod->m_selected);
	if(selected && inst->m_highlight < HIGHLIGHT_SELECTION && !inst->m_showingMaterialIndexes)
		inst->m_highlight = HIGHLIGHT_SELECTION;
	highlightColor = highlightCols[inst->m_highlight];
	highlightAlpha = inst->m_highlight == HIGHLIGHT_SELECTION ?
		(uint8)(gSelectionHighlightOpacity * 255 / 100) : 128;
	// Focus my map: a grey veil over the game's buildings.
	static rw::RGBA veil = { 120, 122, 128, 255 };
	if(gFocusMyMap && inst->m_highlight == HIGHLIGHT_NONE && !gPlayerView && !IsUserMapObject(inst)){
		highlightColor = veil;
		highlightAlpha = 170;
	}

	// A train stands where the client puts it, its carriages behind.
	ObjectInst *carriages[TRAIN_MAX_CARS];
	int numCarriages = 0;
	if(inst->m_isVehicle && IsTrainEngineModel(inst->m_objectId))
		numCarriages = UpdateTrainVisual(inst, carriages);

	if(obj->m_type == ObjectDef::ATOMIC)
		((rw::Atomic*)inst->m_rwObject)->render();
	else if(obj->m_type == ObjectDef::CLUMP)
		((rw::Clump*)inst->m_rwObject)->render();
	for(int i = 0; i < numCarriages; i++)
		((rw::Clump*)carriages[i]->m_rwObject)->render();

	highlightColor = black;
	if(obj->m_noBackfaceCulling)
		SetRenderState(rw::CULLMODE, cull);
	pDirect->setFlags(rw::Light::LIGHTATOMICS);
}

void
RenderAttachmentWorkspaceGrid(void)
{
	if(!AttachmentWorkspaceHidesWorld())
		return;
	ObjectInst *target = GetAttachmentWorkspaceTarget();
	if(target == nil || target->m_isDeleted)
		return;

	float range = max(10.0f, min(40.0f, TheCamera.distanceToTarget() * 1.25f));
	int steps = (int)ceilf(range);
	rw::V3d origin = target->m_translation;
	float gridZ = origin.z;
	ObjectDef *obj = GetObjectDef(target->m_objectId);
	if(obj && obj->m_colModel){
		const CBox &box = obj->m_colModel->boundingBox;
		gridZ = 1.0e30f;
		for(int x = 0; x < 2; x++)
			for(int y = 0; y < 2; y++)
				for(int z = 0; z < 2; z++){
					rw::V3d corner = {
						x ? box.max.x : box.min.x,
						y ? box.max.y : box.min.y,
						z ? box.max.z : box.min.z
					};
					rw::V3d::transformPoints(&corner, &corner, 1, &target->m_matrix);
					gridZ = min(gridZ, corner.z);
				}
		gridZ -= 0.03f;
	}
	rw::RGBA grid = { 65, 72, 82, 255 };
	rw::RGBA xAxis = { 110, 78, 70, 255 };
	rw::RGBA yAxis = { 70, 100, 78, 255 };
	rw::V3d gridPoints[324];
	int numGridPoints = 0;
	for(int i = -steps; i <= steps; i++){
		if(i == 0)
			continue;
		float offset = (float)i;
		gridPoints[numGridPoints++] = { origin.x - range, origin.y + offset, gridZ };
		gridPoints[numGridPoints++] = { origin.x + range, origin.y + offset, gridZ };
		gridPoints[numGridPoints++] = { origin.x + offset, origin.y - range, gridZ };
		gridPoints[numGridPoints++] = { origin.x + offset, origin.y + range, gridZ };
	}
	SetRenderState(rw::CULLMODE, rw::CULLNONE);
	rw::SetRenderState(rw::FOGENABLE, 0);
	rw::SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAALWAYS);
	RenderLinesNow(gridPoints, numGridPoints, grid);
	rw::V3d axes[] = {
		{ origin.x - range, origin.y, gridZ }, { origin.x + range, origin.y, gridZ },
		{ origin.x, origin.y - range, gridZ }, { origin.x, origin.y + range, gridZ }
	};
	RenderLinesNow(axes, 2, xAxis);
	RenderLinesNow(axes + 2, 2, yAxis);
}

void
RenderAttachmentWorkspacePreview(void)
{
	if(!IsAttachmentWorkspaceActive())
		return;
	ObjectInst *preview = GetAttachmentWorkspacePlayerPreview();
	if(preview == nil || preview->m_rwObject == nil)
		return;
	preview->PreRender();
	RenderInst(preview);
}

static void
RenderTransparentInst(ObjectInst *inst)
{
	ObjectDef *obj;
	obj = GetObjectDef(inst->m_objectId);
	if(obj == nil) return;

	if(obj->m_noZwrite){
		SetRenderState(rw::ZWRITEENABLE, 0);
		SetRenderState(rw::ALPHATESTREF, 0);
	}else
		// The opaque pass uses SA's high cutout threshold (100), but sorted
		// draw-last objects also contain soft-alpha decals, grime and shadows.
		// Restore the platform's default threshold here so those pixels blend
		// instead of being discarded almost entirely.
		SetRenderState(rw::ALPHATESTREF, params.alphaRefDefault);

//	This is not handled that way by GTA, only on fading entities....
//	if(obj->m_additive)
//		SetRenderState(rw::DESTBLEND, rw::BLENDONE);

	RenderInst(inst);

	if(obj->m_noZwrite)
		SetRenderState(rw::ZWRITEENABLE, 1);
//	SetRenderState(rw::DESTBLEND, rw::BLENDINVSRCALPHA);
}

static void
ProcessBuilding(ObjectInst *inst)
{
	float dist;
	if(inst->m_isBigBuilding || inst->m_scanCode == currentScanCode)
		return;
	inst->m_scanCode = currentScanCode;
	int v;
	if(gRenderOnlyHD)
		v = SetupVisibilitySimple(inst, &dist);
	else
		v = isSA() ? 
			SetupHdVisibilitySA(inst, &dist) :
			SetupVisibilityIII(inst, &dist);
	if(v == VIS_VISIBLE)
		AddToRenderList(inst, dist);
	else if(v == VIS_STREAMME)
		RequestObject(inst->m_objectId);
}

static void
ProcessBigBuilding(ObjectInst *inst)
{
	float dist;
	if(!inst->m_isBigBuilding || inst->m_scanCode == currentScanCode)
		return;
	inst->m_scanCode = currentScanCode;
	int v;
	if(gRenderOnlyLod)
		v = SetupVisibilitySimple(inst, &dist);
	else
		v = isSA() ? 
			SetupBigBuildingVisibilitySA(inst, &dist) :
			SetupVisibilityIII(inst, &dist);
	if(v == VIS_VISIBLE)
		AddToRenderList(inst, dist);
	else if(v == VIS_STREAMME)
		RequestObject(inst->m_objectId);
}

static void
ScanInstList(CPtrList *list, void (*f)(ObjectInst *inst))
{
	CPtrNode *p;
	for(p = list->first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted || inst->m_editorHidden || PlayerViewHides(inst) ||
		   !AttachmentWorkspaceShowsInstance(inst))
			continue;
		f(inst);
	}
}

void
BuildRenderList(void)
{
	CPtrNode *p;
	ObjectInst *inst;

	currentScanCode++;
	visibleInsts.clear();
	sortedInsts.clear();
	lodList.clear();
	ClearElementIcons();

	rw::BBox frustBox = Scene.camera->frustumBoundBox;
//	printf("%f %f %f    %f %f %f\n",
//		frustBox.inf.x, frustBox.inf.y, frustBox.inf.z, 
//		frustBox.sup.x, frustBox.sup.y, frustBox.sup.z);
	// clip to world boundaries
	frustBox.inf.x = clamp(frustBox.inf.x, worldBounds.left, worldBounds.right-1.0f);
	frustBox.inf.y = clamp(frustBox.inf.y, worldBounds.bottom, worldBounds.top-1.0f);
	frustBox.sup.x = clamp(frustBox.sup.x, worldBounds.left, worldBounds.right-1.0f);
	frustBox.sup.y = clamp(frustBox.sup.y, worldBounds.bottom, worldBounds.top-1.0f);

	int x, xstart, xend;
	int y, ystart, yend;
	xstart = GetSectorIndexX(frustBox.inf.x);
	xend   = GetSectorIndexX(frustBox.sup.x);
	ystart = GetSectorIndexY(frustBox.inf.y);
	yend   = GetSectorIndexY(frustBox.sup.y);
//	log("x: %d - %d; y: %d - %d\n", xstart, xend, ystart, yend);

	if(!gRenderOnlyLod){
		for(x = xstart; x <= xend; x++)
			for(y = ystart; y <= yend; y++){
				Sector *s = GetSector(x, y);
				ScanInstList(&s->buildings, ProcessBuilding);
				ScanInstList(&s->buildings_overlap, ProcessBuilding);
			}
		ScanInstList(&outOfBoundsSector.buildings, ProcessBuilding);
	}
	if(!gRenderOnlyHD){
		for(x = xstart; x <= xend; x++)
			for(y = ystart; y <= yend; y++){
				Sector *s = GetSector(x, y);
				ScanInstList(&s->bigbuildings, ProcessBigBuilding);
				ScanInstList(&s->bigbuildings_overlap, ProcessBigBuilding);
			}
		ScanInstList(&outOfBoundsSector.bigbuildings, ProcessBigBuilding);
	}

//	ScanInstList(&instances, ProcessBuilding);
//	ScanInstList(&instances, ProcessBigBuilding);

	ProcessLodList();

	// Reset instances
	for(p = instances.first; p; p = p->next){
		inst = (ObjectInst*)p->item;
		inst->m_numChildrenRendered = 0;
		inst->m_highlight = HIGHLIGHT_NONE;
	}
	// And the other way: a LOD drawn far away for its selected building.
	for(p = instances.first; p; p = p->next){
		inst = (ObjectInst*)p->item;
		if(inst->m_selected && inst->m_lod && inst->m_lod->m_highlight < HIGHLIGHT_SELECTION)
			inst->m_lod->m_highlight = HIGHLIGHT_SELECTION;
	}

//	log("%d visible instances\n", numVisibleInsts);
}

void
RenderOpaque(void)
{
	SetRenderState(rw::ALPHATESTREF, params.alphaRef);

	SetRenderState(rw::CULLMODE, AttachmentWorkspaceHidesWorld() || !gDoBackfaceCulling ?
		rw::CULLNONE : rw::CULLBACK);
	int i, numVisibleInsts = (int)visibleInsts.size();
	for(i = 0; i < numVisibleInsts; i++)
		RenderInst(visibleInsts[i]);
}

void
RenderTransparent(void)
{
	SetRenderState(rw::CULLMODE, AttachmentWorkspaceHidesWorld() || !gDoBackfaceCulling ?
		rw::CULLNONE : rw::CULLBACK);
	std::sort(sortedInsts.begin(), sortedInsts.end(), [](const InstDist &a, const InstDist &b) {
		return a.dist < b.dist;
	});
	for(int i = (int)sortedInsts.size()-1; i >= 0; i--)
		RenderTransparentInst(sortedInsts[i].inst);
	// Checkpoint markers, then the icons over them.
	if(gta::renderColourCoded){
		RenderCheckpointMarkersColourCoded();
		RenderElementIconsColourCoded();
	}else{
		RenderCheckpointMarkers((float)ImGui::GetTime());
		RenderElementIcons();
		RenderRadarArtLayers();
	}
}

void
RenderAttachmentBoneMarker(void)
{
	ObjectInst *parent = nil;
	int boneId = 2;
	if(AttachmentTargetPickActive()){
		if(ImGui::GetIO().WantCaptureMouse)
			return;
		Ray ray;
		ray.start = TheCamera.m_position;
		ray.dir = normalize(TheCamera.m_mouseDir);
		parent = GetVisibleInstUnderRay(ray);
		if(!IsPlayerAttachmentTarget(parent))
			return;
		GetAttachmentTargetPickOptions(&boneId, nil);
	}else{
		ObjectInst *child = nil;
		for(CPtrNode *p = selection.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(inst && !inst->m_isDeleted && IsPlayerAttachedObject(inst)){
				child = inst;
				break;
			}
		}
		if(child){
			parent = GetAttachParent(child);
			boneId = GetObjectPawnProperties(child)->attachBone;
		}else{
			parent = GetAttachmentWorkspaceTarget();
			GetAttachmentTargetPickOptions(&boneId, nil);
		}
	}
	rw::Matrix bone;
	if(parent == nil || !GetAttachmentPlayerBoneTransform(parent, boneId, &bone))
		return;

	float length = max(0.16f, min(0.55f, TheCamera.distanceToTarget() * 0.014f));
	rw::V3d points[] = {
		bone.pos, add(bone.pos, scale(bone.right, length)),
		bone.pos, add(bone.pos, scale(bone.up, length)),
		bone.pos, add(bone.pos, scale(bone.at, length))
	};
	rw::RGBA red = { 255, 80, 70, 255 };
	rw::RGBA green = { 85, 255, 100, 255 };
	rw::RGBA blue = { 75, 145, 255, 255 };
	uint32 oldZTest = rw::GetRenderState(rw::ZTESTENABLE);
	uint32 oldFog = rw::GetRenderState(rw::FOGENABLE);
	uint32 oldCull = rw::GetRenderState(rw::CULLMODE);
	uint32 oldAlphaFunc = rw::GetRenderState(rw::ALPHATESTFUNC);
	SetRenderState(rw::ZTESTENABLE, 0);
	SetRenderState(rw::FOGENABLE, 0);
	SetRenderState(rw::CULLMODE, rw::CULLNONE);
	SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAALWAYS);
	RenderLinesNow(points, 2, red);
	RenderLinesNow(points + 2, 2, green);
	RenderLinesNow(points + 4, 2, blue);
	SetRenderState(rw::ZTESTENABLE, oldZTest);
	SetRenderState(rw::FOGENABLE, oldFog);
	SetRenderState(rw::CULLMODE, oldCull);
	SetRenderState(rw::ALPHATESTFUNC, oldAlphaFunc);
}

void
RenderEverything(void)
{
	RenderOpaque();
	RenderTransparent();
}

void
ForEachVisibleInst(void (*fn)(ObjectInst *inst, void *data), void *data)
{
	for(int i = 0; i < (int)visibleInsts.size(); i++)
		fn(visibleInsts[i], data);
	for(int i = 0; i < (int)sortedInsts.size(); i++)
		fn(sortedInsts[i].inst, data);
}

ObjectInst*
GetVisibleInstUnderRay(const Ray &ray, rw::V3d *hitPos, float *hitT)
{
	float bestT = 1.0e30f;
	rw::V3d bestHit = { 0.0f, 0.0f, 0.0f };
	ObjectInst *bestInst = findClosestRayHitInVisibleList(visibleInsts, ray, &bestHit, &bestT);

	ObjectInst *sortedHit = findClosestRayHitInSortedList(sortedInsts, ray, &bestHit, &bestT);
	if(sortedHit)
		bestInst = sortedHit;

	if(bestInst == nil)
		return nil;
	if(hitPos)
		*hitPos = bestHit;
	if(hitT)
		*hitT = bestT;
	return bestInst;
}

static void
RenderCollision(ObjectInst *inst)
{
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil) return;
	if(obj->m_colModel)
		RenderColModelWire(obj->m_colModel, &inst->m_matrix, !gRenderCollision || obj->m_isBigBuilding);
	else
		printf("object %s has no LOD\n", obj->m_name);
}

void
RenderEverythingCollisions(void)
{
	int i, numVisibleInsts = (int)visibleInsts.size();
	for(i = 0; i < numVisibleInsts; i++)
		RenderCollision(visibleInsts[i]);

	int numSortedInsts = (int)sortedInsts.size();
	for(i = 0; i < numSortedInsts; i++)
		RenderCollision(sortedInsts[i].inst);
}

void
setupLeedsPipe(void)
{
	gta::leedsPipe_amb = Timecycle::currentColours.amb;
	gta::leedsPipe_emiss = Timecycle::currentColours.amb_bl;
	switch(gBuildingPipeSwitch){
	case PLATFORM_PS2:
		gta::leedsPipe_platformSwitch = 1;
		break;
	case PLATFORM_PSP:
		gta::leedsPipe_platformSwitch = 0;
		break;
	default:
		gta::leedsPipe_platformSwitch = 2;
		break;
	}
}

#ifdef RW_D3D9
static void
leedsRenderCB(rw::Atomic *atomic, rw::d3d9::InstanceDataHeader *header)
{
	setupLeedsPipe();
	gta::leedsRenderCB(atomic, header);
}

void
MakeLeedsPipe(void)
{
	gta::MakeLeedsPipe();
	((rw::d3d9::ObjPipeline*)gta::leedsPipe)->renderCB = leedsRenderCB;
}
#endif

#ifdef RW_GL3
static void
leedsRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header)
{
	setupLeedsPipe();
	gta::leedsRenderCB(atomic, header);
}

void
MakeLeedsPipe(void)
{
	gta::MakeLeedsPipe();
	((rw::gl3::ObjPipeline*)gta::leedsPipe)->renderCB = leedsRenderCB;
}
#endif

void
RegisterPipes(void)
{
#ifdef RW_GL3
	rw::gl3::registerUniform("u_dayparam");
	rw::gl3::registerUniform("u_nightparam");
	rw::gl3::registerUniform("u_texmat");
	rw::gl3::registerUniform("u_envmat");
	rw::gl3::registerUniform("u_envXform");
	rw::gl3::registerUniform("u_shininess");
	rw::gl3::registerUniform("u_colorscale");

	rw::gl3::registerUniform("tex0");
	rw::gl3::registerUniform("tex1");
	rw::gl3::registerUniform("u_lm");

	rw::gl3::registerUniform("u_postfxCol1");
	rw::gl3::registerUniform("u_postfxCol2");
	rw::gl3::registerUniform("u_postfxParams");

	rw::gl3::registerUniform("u_amb");
	rw::gl3::registerUniform("u_emiss");
#endif
}

void
RenderInit(void)
{
	colourCodePipe = gta::makeColourCodePipeline();
	MakeCustomBuildingPipelines();
	MakeNeoWorldPipe();
	MakeLeedsPipe();
}

// Object outlines: the edges that shape a model, drawn as lines on top of
// it. They show selected and hovered objects whatever their textures, so
// invisible and see-through objects can still be found and retextured.

// A smooth edge between two faces: on the silhouette when the camera sees
// one face and not the other. Smooth models (barrels, rocks) have no borders
// or creases, so only these give them a contour.
struct SilhouetteEdge
{
	rw::V3d a, b, n1, n2;
};

// Model-space segments (pairs of points) along a geometry's open borders
// and sharp creases. Vertices are welded by position first, so texture
// seams do not count as borders.
static void
BuildOutlineEdges(rw::Geometry *geo, std::vector<rw::V3d> &out, std::vector<SilhouetteEdge> &smooth)
{
	out.clear();
	smooth.clear();
	if(geo->triangles == nil || geo->numMorphTargets < 1 || geo->morphTargets[0].vertices == nil)
		return;
	rw::V3d *verts = geo->morphTargets[0].vertices;

	std::vector<int> weld(geo->numVertices);
	std::unordered_map<uint64, int> byPosition;
	for(int i = 0; i < geo->numVertices; i++){
		// 2 mm grid; 21 bits per axis covers +-2 km of model space.
		uint64 qx = (uint64)(lroundf(verts[i].x * 500.0f) & 0x1FFFFF);
		uint64 qy = (uint64)(lroundf(verts[i].y * 500.0f) & 0x1FFFFF);
		uint64 qz = (uint64)(lroundf(verts[i].z * 500.0f) & 0x1FFFFF);
		auto it = byPosition.emplace(qx << 42 | qy << 21 | qz, i);
		weld[i] = it.first->second;
	}

	struct EdgeInfo {
		rw::V3d normal, normal2;
		int count;
		bool crease;	// two faces meet at a sharp angle
		bool opposite;	// back-to-back faces, as on a double-sided sheet
	};
	const float creaseCos = 0.82f;	// about 35 degrees
	std::unordered_map<uint64, EdgeInfo> edges;
	for(int t = 0; t < geo->numTriangles; t++){
		int v[3];
		for(int k = 0; k < 3; k++)
			v[k] = weld[geo->triangles[t].v[k]];
		if(v[0] == v[1] || v[1] == v[2] || v[0] == v[2])
			continue;
		rw::V3d n = cross(sub(verts[v[1]], verts[v[0]]), sub(verts[v[2]], verts[v[0]]));
		float len = length(n);
		if(len < 1e-8f)
			continue;
		n = scale(n, 1.0f/len);
		for(int k = 0; k < 3; k++){
			uint32 a = v[k], b = v[(k+1)%3];
			uint64 key = a < b ? (uint64)a << 32 | b : (uint64)b << 32 | a;
			auto it = edges.find(key);
			if(it == edges.end()){
				EdgeInfo info = { n, n, 1, false, false };
				edges.emplace(key, info);
				continue;
			}
			EdgeInfo &info = it->second;
			info.count++;
			info.normal2 = n;
			float d = dot(n, info.normal);
			if(d < -0.98f)
				info.opposite = true;
			else if(d < creaseCos)
				info.crease = true;
		}
	}

	for(auto &edge : edges){
		const EdgeInfo &info = edge.second;
		// A double-sided sheet's border has exactly its front and back face.
		if(info.count == 1 || info.crease || (info.count == 2 && info.opposite)){
			out.push_back(verts[edge.first >> 32]);
			out.push_back(verts[edge.first & 0xFFFFFFFF]);
		}else if(info.count == 2)
			smooth.push_back({ verts[edge.first >> 32], verts[edge.first & 0xFFFFFFFF], info.normal, info.normal2 });
	}
}

struct OutlineCacheEntry
{
	int numVertices;
	int numTriangles;
	std::vector<rw::V3d> points;
	std::vector<SilhouetteEdge> smooth;
};
static std::unordered_map<rw::Geometry*, OutlineCacheEntry> outlineCache;

static const OutlineCacheEntry&
GetOutlineEdges(rw::Geometry *geo)
{
	auto it = outlineCache.find(geo);
	// A streamed-out model's geometry can be freed and the address reused.
	if(it != outlineCache.end() && it->second.numVertices == geo->numVertices &&
	   it->second.numTriangles == geo->numTriangles)
		return it->second;
	if(outlineCache.size() > 4096)
		outlineCache.clear();
	OutlineCacheEntry &entry = outlineCache[geo];
	entry.numVertices = geo->numVertices;
	entry.numTriangles = geo->numTriangles;
	BuildOutlineEdges(geo, entry.points, entry.smooth);
	return entry;
}

// silhouette: also the smooth edges on the contour from the camera; only
// for the few selected and hovered objects, since it changes every frame.
static void
AddAtomicOutline(rw::Atomic *atomic, std::vector<rw::V3d> &out, bool silhouette)
{
	rw::Geometry *geo = atomic->geometry;
	if(geo == nil || !(atomic->getFlags() & rw::Atomic::RENDER))
		return;
	const OutlineCacheEntry &entry = GetOutlineEdges(geo);
	const std::vector<rw::V3d> &edges = entry.points;
	rw::Matrix *ltm = atomic->getFrame()->getLTM();
	size_t start = out.size();
	if(!edges.empty()){
		out.resize(start + edges.size());
		rw::V3d::transformPoints(&out[start], edges.data(), (int32)edges.size(), ltm);
	}
	if(silhouette && !entry.smooth.empty()){
		// The camera in model space: which side of a face it is on doesn't
		// change under the object's transform, scaled or not.
		rw::Matrix inverse;
		rw::Matrix::invert(&inverse, ltm);
		rw::V3d eye;
		rw::V3d::transformPoints(&eye, &TheCamera.m_position, 1, &inverse);
		size_t silhouetteStart = out.size();
		for(const SilhouetteEdge &e : entry.smooth){
			rw::V3d toEye = sub(eye, e.a);
			if((dot(e.n1, toEye) > 0.0f) != (dot(e.n2, toEye) > 0.0f)){
				out.push_back(e.a);
				out.push_back(e.b);
			}
		}
		if(out.size() > silhouetteStart)
			rw::V3d::transformPoints(&out[silhouetteStart], &out[silhouetteStart],
				(int32)(out.size() - silhouetteStart), ltm);
	}
	// Pull the lines a little toward the camera so they win the depth test
	// against the surface they lie on.
	for(size_t i = start; i < out.size(); i++)
		out[i] = add(out[i], scale(sub(TheCamera.m_position, out[i]), 0.002f));
}

static void
AddInstOutline(ObjectInst *inst, std::vector<rw::V3d> &out, bool silhouette = false)
{
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil || inst->m_rwObject == nil)
		return;
	if(obj->m_type == ObjectDef::ATOMIC)
		AddAtomicOutline((rw::Atomic*)inst->m_rwObject, out, silhouette);
	else if(obj->m_type == ObjectDef::CLUMP){
		FORLIST(lnk, ((rw::Clump*)inst->m_rwObject)->atomics)
			AddAtomicOutline(rw::Atomic::fromClump(lnk), out, silhouette);
	}
}

void
RenderObjectOutlines(void)
{
	enum { OUTLINE_MAP, OUTLINE_HOVER, OUTLINE_SELECTED, OUTLINE_MATCH, NUM_OUTLINES };
	static const rw::RGBA colours[NUM_OUTLINES] = {
		{ 90, 200, 255, 170 },	// map objects
		{ 255, 255, 255, 255 },	// hovered
		{ 255, 190, 40, 255 },	// selected
		{ 255, 220, 60, 255 },	// search matches
	};
	static std::vector<rw::V3d> points[NUM_OUTLINES];
	const size_t maxMapPoints = 400000;
	const float maxMapDistance = 300.0f;

	for(int k = 0; k < NUM_OUTLINES; k++)
		points[k].clear();
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted || inst->m_editorHidden || PlayerViewHides(inst) ||
		   !AttachmentWorkspaceShowsInstance(inst) || inst->m_rwObject == nil ||
		   InstUsesEditorIcon(inst))
			continue;
		if(inst->m_highlight == HIGHLIGHT_MATCH && points[OUTLINE_MATCH].size() < maxMapPoints)
			AddInstOutline(inst, points[OUTLINE_MATCH]);
		else if(inst->m_selected && gOutlineSelection)
			AddInstOutline(inst, points[OUTLINE_SELECTED], points[OUTLINE_SELECTED].size() < maxMapPoints);
		else if(inst->m_highlight == HIGHLIGHT_HOVER && gOutlineSelection)
			AddInstOutline(inst, points[OUTLINE_HOVER], true);
		else if(gOutlineMapObjects && points[OUTLINE_MAP].size() < maxMapPoints &&
		        PawnObjectNeedsExport(inst) &&
		        length(sub(inst->m_translation, TheCamera.m_position)) < maxMapDistance)
			AddInstOutline(inst, points[OUTLINE_MAP]);
	}
	if(points[OUTLINE_MAP].empty() && points[OUTLINE_HOVER].empty() && points[OUTLINE_SELECTED].empty() && points[OUTLINE_MATCH].empty())
		return;

	rw::SetRenderState(rw::VERTEXALPHA, 1);
	rw::SetRenderState(rw::ZWRITEENABLE, 0);
	// Faintly where something is in front, so an object behind a wall or
	// inside another can still be found.
	rw::SetRenderState(rw::ZTESTENABLE, 0);
	for(int k = OUTLINE_HOVER; k < NUM_OUTLINES; k++){
		rw::RGBA faint = colours[k];
		faint.alpha = 70;
		RenderLinesNow(points[k].data(), (int)points[k].size(), faint);
	}
	rw::SetRenderState(rw::ZTESTENABLE, 1);
	for(int k = 0; k < NUM_OUTLINES; k++)
		RenderLinesNow(points[k].data(), (int)points[k].size(), colours[k]);
	rw::SetRenderState(rw::ZWRITEENABLE, 1);
}

// Where selected moving objects go (MoveObject): a line, a ghost at the end
// and one gliding there and back at the object's speed, pausing at each end.
// The client moves at a constant speed and turns in step with the distance
// (reSA-MP MoveObjectRuntimeToTarget); with no distance the turn takes a second here.
void
RenderObjectMovePreviews(void)
{
	if(!gPreviewObjectMoves)
		return;
	float now = (float)ImGui::GetTime();
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted || inst->m_isVehicle || GetSampElementKind(inst) != SAMP_ELEMENT_OBJECT)
			continue;
		const ObjectPawnProperties *properties = GetObjectPawnProperties(inst);
		if(!properties->moveEnabled)
			continue;
		rw::V3d from = inst->m_translation, to = add(from, properties->moveOffset);
		rw::Quat startRot = inst->m_rotation;
		rw::Quat endRot = properties->moveRotates ? MakeObjectRotationDegrees(properties->moveRotation) : startRot;
		rw::V3d line[2] = { from, to };
		RenderLinesNow(line, 2, { 120, 230, 255, 255 });
		RenderPlacementGhost(inst->m_objectId, to, endRot, { 120, 230, 255, 70 });

		float distance = length(properties->moveOffset);
		float duration = distance > 0.001f ? distance / std::max(properties->moveSpeed, 0.01f) : 1.0f;
		const float pause = 0.5f;
		float cycle = 2.0f * (duration + pause);
		float t = fmodf(now, cycle);
		float f = t < duration ? t / duration : t < duration + pause ? 1.0f :
			t < 2.0f * duration + pause ? 1.0f - (t - duration - pause) / duration : 0.0f;
		// Normalised lerp, the short way round.
		rw::Quat a = startRot, b = endRot;
		if(a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w < 0.0f)
			b = { -b.x, -b.y, -b.z, -b.w };
		rw::Quat q = { a.x + (b.x - a.x)*f, a.y + (b.y - a.y)*f, a.z + (b.z - a.z)*f, a.w + (b.w - a.w)*f };
		float n = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
		if(n > 1e-6f)
			q = { q.x/n, q.y/n, q.z/n, q.w/n };
		RenderPlacementGhost(inst->m_objectId, add(from, scale(properties->moveOffset, f)), q, { 120, 230, 255, 150 });
	}
}
