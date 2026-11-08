#include "haumea.h"
#include "checkpoints.h"

#include <math.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

// Checkpoints drawn the way the SA-MP client shows them, with the game's own
// marker models as C3dMarkers places them (gta-reversed Checkpoint.cpp,
// 3dMarkers.cpp, TheScripts.cpp; ariane-a9's notes in HANDOFF):
// - a player checkpoint: the cylinder (1317) scaled by its size, red at alpha
//   76, pulsing every 2.048 s by 10%, sunk by size/20;
// - race checkpoints 0-2: the same cylinder as a tall tube (1.5, 1.5 and 30
//   times the size), alpha 128, 0.5 m down; type 0 adds the diamond (1559)
//   3 m up pointing at the next one, black and then (255, 64, 64) at 0.8 of
//   that; type 1 the finishFlag sprite (particle.txd) 3 m up;
// - air checkpoints 3-8: the hoop (1316) scaled by the size, facing the next
//   one, alpha 96, moving as types 5-8 do.
// A missing model (the synthetic test game) falls back to meshes made here.
// The colour-coded pick pass draws the same shapes in the element's id colour.

static const rw::RGBA CP_RED = { 255, 0, 0, 255 };

// ---- Triangles, batched through im3d ----

#define CP_VERTS 1200
#define CP_INDICES 3600
static rw::RWDEVICE::Im3DVertex gVerts[CP_VERTS];
static uint16 gIndices[CP_INDICES];
static int gNumVerts, gNumIndices;

static void
flush(void)
{
	if(gNumVerts && gNumIndices){
		rw::im3d::Transform(gVerts, gNumVerts, nil, rw::im3d::EVERYTHING);
		rw::im3d::RenderIndexedPrimitive(rw::PRIMTYPETRILIST, gIndices, gNumIndices);
		rw::im3d::End();
	}
	gNumVerts = 0;
	gNumIndices = 0;
}

static void
reserve(int verts, int indices)
{
	if(gNumVerts + verts > CP_VERTS || gNumIndices + indices > CP_INDICES)
		flush();
}

static int
vertex(rw::V3d p, rw::RGBA c)
{
	rw::RWDEVICE::Im3DVertex &v = gVerts[gNumVerts];
	v.setX(p.x);
	v.setY(p.y);
	v.setZ(p.z);
	v.setU(0.0f);
	v.setV(0.0f);
	v.setColor(c.red, c.green, c.blue, c.alpha);
	return gNumVerts++;
}

static void
triangle(int a, int b, int c)
{
	gIndices[gNumIndices++] = (uint16)a;
	gIndices[gNumIndices++] = (uint16)b;
	gIndices[gNumIndices++] = (uint16)c;
}

static void
quad(rw::V3d a, rw::V3d b, rw::V3d c, rw::V3d d, rw::RGBA ca, rw::RGBA cb, rw::RGBA cc, rw::RGBA cd)
{
	reserve(4, 6);
	int i = vertex(a, ca), j = vertex(b, cb), k = vertex(c, cc), l = vertex(d, cd);
	triangle(i, j, k);
	triangle(i, k, l);
}

static rw::RGBA
withAlpha(rw::RGBA c, int alpha)
{
	c.alpha = (uint8)std::max(0, std::min(255, alpha));
	return c;
}

// ---- Shapes ----

static const int SEGMENTS = 32;

// An open cylinder on the ground, opaque-ish at the bottom and gone at the top.
static void
cylinder(rw::V3d base, float radius, float height, rw::RGBA colour, bool solid)
{
	rw::RGBA bottom = solid ? colour : withAlpha(colour, 140);
	rw::RGBA top = solid ? colour : withAlpha(colour, 0);
	for(int k = 0; k < SEGMENTS; k++){
		float a0 = (float)k / SEGMENTS * 2.0f * (float)M_PI;
		float a1 = (float)(k + 1) / SEGMENTS * 2.0f * (float)M_PI;
		rw::V3d p0 = { base.x + radius * cosf(a0), base.y + radius * sinf(a0), base.z };
		rw::V3d p1 = { base.x + radius * cosf(a1), base.y + radius * sinf(a1), base.z };
		quad(p0, p1, { p1.x, p1.y, p1.z + height }, { p0.x, p0.y, p0.z + height }, bottom, bottom, top, top);
	}
}

// A hoop (torus) standing upright, its hole facing along dir.
static void
hoop(rw::V3d centre, rw::V3d dir, float radius, float thickness, rw::RGBA colour)
{
	// The ring's plane: perpendicular to dir (kept horizontal-free of roll).
	rw::V3d up = { 0.0f, 0.0f, 1.0f };
	rw::V3d side = { dir.y, -dir.x, 0.0f };
	float sideLength = sqrtf(side.x * side.x + side.y * side.y);
	if(sideLength < 0.001f)
		side = { 1.0f, 0.0f, 0.0f };
	else
		side = { side.x / sideLength, side.y / sideLength, 0.0f };
	const int tube = 8;
	for(int k = 0; k < SEGMENTS; k++){
		float a0 = (float)k / SEGMENTS * 2.0f * (float)M_PI;
		float a1 = (float)(k + 1) / SEGMENTS * 2.0f * (float)M_PI;
		for(int t = 0; t < tube; t++){
			float b0 = (float)t / tube * 2.0f * (float)M_PI;
			float b1 = (float)(t + 1) / tube * 2.0f * (float)M_PI;
			auto point = [&](float a, float b){
				// Around the ring (a) in the side/up plane, and around the tube (b).
				rw::V3d ring = { side.x * cosf(a) + up.x * sinf(a), side.y * cosf(a) + up.y * sinf(a),
					side.z * cosf(a) + up.z * sinf(a) };
				float r = radius + thickness * cosf(b);
				return rw::V3d{ centre.x + ring.x * r + dir.x * thickness * sinf(b),
					centre.y + ring.y * r + dir.y * thickness * sinf(b),
					centre.z + ring.z * r + dir.z * thickness * sinf(b) };
			};
			quad(point(a0, b0), point(a1, b0), point(a1, b1), point(a0, b1), colour, colour, colour, colour);
		}
	}
}

// An arrow pointing along dir: the shaft-and-head shape twice, crossed (once
// level, once upright), so it reads from any side as the game's arrow does.
static void
arrow(rw::V3d centre, rw::V3d dir, float length, rw::RGBA colour)
{
	float flat = sqrtf(dir.x * dir.x + dir.y * dir.y);
	rw::V3d d = flat > 0.001f ? rw::V3d{ dir.x / flat, dir.y / flat, 0.0f } : rw::V3d{ 1.0f, 0.0f, 0.0f };
	rw::V3d across[2] = { { -d.y, d.x, 0.0f }, { 0.0f, 0.0f, 1.0f } };
	float half = length * 0.5f, shaft = length * 0.12f, head = length * 0.3f, headLength = length * 0.45f;
	for(const rw::V3d &s : across){
		auto at = [&](float along, float side){
			return rw::V3d{ centre.x + d.x * along + s.x * side, centre.y + d.y * along + s.y * side,
				centre.z + s.z * side };
		};
		quad(at(-half, -shaft), at(half - headLength, -shaft), at(half - headLength, shaft), at(-half, shaft),
			colour, colour, colour, colour);
		reserve(3, 3);
		triangle(vertex(at(half - headLength, -head), colour), vertex(at(half, 0.0f), colour),
			vertex(at(half - headLength, head), colour));
	}
}

// The chequered finish flag on a pole, facing the camera's side of dir.
static void
finishFlag(rw::V3d foot, float height, rw::V3d facing)
{
	rw::RGBA pole = { 200, 200, 200, 255 };
	float flat = sqrtf(facing.x * facing.x + facing.y * facing.y);
	rw::V3d s = flat > 0.001f ? rw::V3d{ -facing.y / flat, facing.x / flat, 0.0f } : rw::V3d{ 1.0f, 0.0f, 0.0f };
	float w = height * 0.06f;
	quad({ foot.x - s.x * w, foot.y - s.y * w, foot.z }, { foot.x + s.x * w, foot.y + s.y * w, foot.z },
		{ foot.x + s.x * w, foot.y + s.y * w, foot.z + height }, { foot.x - s.x * w, foot.y - s.y * w, foot.z + height },
		pole, pole, pole, pole);
	const int cols = 4, rows = 3;
	float cell = height * 0.18f;
	for(int r = 0; r < rows; r++)
		for(int c = 0; c < cols; c++){
			rw::RGBA colour = (r + c) % 2 ? rw::RGBA{ 20, 20, 20, 255 } : rw::RGBA{ 245, 245, 245, 255 };
			float z0 = foot.z + height - (r + 1) * cell, z1 = z0 + cell;
			float x0 = w + c * cell, x1 = x0 + cell;
			quad({ foot.x + s.x * x0, foot.y + s.y * x0, z0 }, { foot.x + s.x * x1, foot.y + s.y * x1, z0 },
				{ foot.x + s.x * x1, foot.y + s.y * x1, z1 }, { foot.x + s.x * x0, foot.y + s.y * x0, z1 },
				colour, colour, colour, colour);
		}
}

// ---- The game's models ----

enum { MODEL_CYLINDER, MODEL_HOOP, MODEL_DIAMOND, NUM_MODELS };
struct MarkerModel { int id; rw::Atomic *atomic; rw::Clump *clump; bool tried; };
static MarkerModel gModels[NUM_MODELS] = { { 1317 }, { 1316 }, { 1559 } };

static MarkerModel*
model(int which)
{
	MarkerModel *m = &gModels[which];
	if(!m->tried){
		m->tried = true;
		if(GetObjectDef(m->id))
			CreateObjectPreviewRwObject(m->id, &m->atomic, &m->clump);
	}
	return m->atomic || m->clump ? m : nil;
}

// Position, a model z axis (GTA's "up") and a scale along x, y and z.
static rw::Matrix
placement(rw::V3d pos, rw::V3d zAxis, rw::V3d scale)
{
	float length = sqrtf(zAxis.x * zAxis.x + zAxis.y * zAxis.y + zAxis.z * zAxis.z);
	rw::V3d u = length > 0.0001f ? rw::V3d{ zAxis.x / length, zAxis.y / length, zAxis.z / length } :
		rw::V3d{ 0.0f, 0.0f, 1.0f };
	rw::V3d helper = fabsf(u.z) < 0.99f ? rw::V3d{ 0.0f, 0.0f, 1.0f } : rw::V3d{ 1.0f, 0.0f, 0.0f };
	rw::V3d right = { helper.y * u.z - helper.z * u.y, helper.z * u.x - helper.x * u.z, helper.x * u.y - helper.y * u.x };
	float rl = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
	right = { right.x / rl, right.y / rl, right.z / rl };
	rw::V3d forward = { u.y * right.z - u.z * right.y, u.z * right.x - u.x * right.z, u.x * right.y - u.y * right.x };
	rw::Matrix m;
	m.setIdentity();
	m.right = { right.x * scale.x, right.y * scale.x, right.z * scale.x };
	m.up = { forward.x * scale.y, forward.y * scale.y, forward.z * scale.y };
	m.at = { u.x * scale.z, u.y * scale.z, u.z * scale.z };
	m.pos = pos;
	m.update();
	return m;
}

// The model in one flat colour, as the game modulates its material colour:
// blended in the world pass; in the pick pass (gta::renderColourCoded) the
// colour-code pipeline draws the id colour solid by itself.
static void
drawModel(MarkerModel *m, const rw::Matrix &matrix, rw::RGBA colour)
{
	rw::Frame *frame = m->atomic ? m->atomic->getFrame() : m->clump->getFrame();
	frame->transform(&matrix, rw::COMBINEREPLACE);
	if(gta::renderColourCoded){
		rw::RGBA oldColour = gta::colourCode;
		gta::colourCode = colour;
		if(m->atomic)
			m->atomic->render();
		else
			m->clump->render();
		gta::colourCode = oldColour;
	}else if(m->atomic)
		RenderAtomicTinted(m->atomic, colour);
	else
		FORLIST(lnk, m->clump->atomics)
			RenderAtomicTinted(rw::Atomic::fromClump(lnk), colour);
}

// The finishFlag sprite, facing the camera.
static rw::Texture*
finishFlagTexture(void)
{
	static bool tried;
	static rw::Texture *texture;
	if(!tried){
		tried = true;
		int slot = AddTxdSlot("~cp_particle");
		LoadTxd(slot, "MODELS/PARTICLE.TXD");
		texture = FindTextureInTxd(slot, "finishFlag");
	}
	return texture;
}

static void
flagSprite(rw::V3d centre, float half, rw::RGBA tint)
{
	rw::Texture *texture = finishFlagTexture();
	if(texture == nil){
		finishFlag({ centre.x, centre.y, centre.z - half }, half * 2.0f, { 0.0f, 1.0f, 0.0f });
		flush();
		return;
	}
	rw::Matrix *cam = TheCamera.m_rwcam->getFrame()->getLTM();
	rw::V3d r = { cam->right.x * half, cam->right.y * half, cam->right.z * half };
	rw::V3d u = { cam->up.x * half, cam->up.y * half, cam->up.z * half };
	rw::SetRenderStatePtr(rw::TEXTURERASTER, texture->raster);
	reserve(4, 6);
	auto corner = [&](float a, float b, float tu, float tv){
		rw::RWDEVICE::Im3DVertex &v = gVerts[gNumVerts];
		v.setX(centre.x + r.x * a + u.x * b);
		v.setY(centre.y + r.y * a + u.y * b);
		v.setZ(centre.z + r.z * a + u.z * b);
		v.setU(tu);
		v.setV(tv);
		v.setColor(tint.red, tint.green, tint.blue, tint.alpha);
		return gNumVerts++;
	};
	int i = corner(-1, 1, 0, 0), j = corner(1, 1, 1, 0), k = corner(1, -1, 1, 1), l = corner(-1, -1, 0, 1);
	triangle(i, j, k);
	triangle(i, k, l);
	flush();
	rw::SetRenderStatePtr(rw::TEXTURERASTER, nil);
}

// ---- Race preview ----

// One player's way round a race, as haumea_races.inc runs it.
struct RaceSimulation
{
	int count = 0, target = -1, mode = SAMP_RACE_POINT_TO_POINT, laps = 1, lap = 1;
	bool started = false;

	bool active(void) const { return target >= 0; }
	bool finalTarget(void) const
	{
		return active() && (mode == SAMP_RACE_POINT_TO_POINT ? target == count - 1 :
			target == 0 && started && lap == laps);
	}
	int type(int type) const
	{
		if(type == SAMP_RACE_CP_AUTO_GROUND) return finalTarget() ? 1 : 0;
		if(type == SAMP_RACE_CP_AUTO_AIR) return finalTarget() ? 4 : 3;
		return std::max(0, std::min(8, type));
	}
	void advance(void)
	{
		if(!active())
			return;
		if(finalTarget()){
			target = -1;
			return;
		}
		if(mode == SAMP_RACE_CIRCUIT && target == 0){
			if(started)
				lap++;
			started = true;
		}
		target = (target + 1) % count;
	}
};

static std::string gPreviewRace;
static RaceSimulation gPreview;

bool
StartRacePreview(const char *raceName)
{
	std::vector<ObjectInst*> race;
	GetRaceCheckpoints(raceName, race);
	if(race.empty())
		return false;
	const ObjectPawnProperties *p = GetObjectPawnProperties(race[0]);
	gPreview = RaceSimulation();
	gPreview.count = (int)race.size();
	gPreview.mode = p->cpRaceMode == SAMP_RACE_CIRCUIT && race.size() >= 2 ? SAMP_RACE_CIRCUIT : SAMP_RACE_POINT_TO_POINT;
	gPreview.laps = gPreview.mode == SAMP_RACE_CIRCUIT ? std::max(1, p->cpRaceLaps) : 1;
	gPreview.target = 0;
	gPreviewRace = raceName;
	return true;
}

void StopRacePreview(void) { gPreview.target = -1; gPreviewRace.clear(); }
void AdvanceRacePreview(void) { gPreview.advance(); if(!gPreview.active()) gPreviewRace.clear(); }
bool RacePreviewActive(void) { return gPreview.active() && !gPreviewRace.empty(); }
const char *RacePreviewRace(void) { return gPreviewRace.c_str(); }
int RacePreviewTarget(void) { return gPreview.target; }
int RacePreviewLap(void) { return gPreview.lap; }
int RacePreviewLaps(void) { return gPreview.laps; }
int RacePreviewCount(void) { return gPreview.count; }

// The previewed race's checkpoint for inst: -1 if it isn't the target, the
// type to show if it is, and where the one after it is.
static int
previewTypeFor(const ObjectInst *inst, const std::vector<ObjectInst*> &race, rw::V3d *next, bool *hasNext)
{
	if(gPreview.target < 0 || gPreview.target >= (int)race.size() || race[gPreview.target] != inst)
		return -1;
	RaceSimulation after = gPreview;
	after.advance();
	*hasNext = after.active() && after.target < (int)race.size();
	if(*hasNext)
		*next = race[after.target]->m_translation;
	return gPreview.type(GetObjectPawnProperties(inst)->cpRaceType);
}

// ---- Checkpoints ----

// While previewing a race: the shown checkpoint's type and the one after it.
static int gDrawType = -1;
static bool gDrawHasNext;
static rw::V3d gDrawNext;

// Where a race checkpoint points: at the next one, or north for the last.
static rw::V3d
pointing(const ObjectInst *inst)
{
	ObjectInst *nextInst = gDrawType >= 0 ? nil : GetNextRaceCheckpoint(inst);
	if(gDrawType >= 0 ? !gDrawHasNext : nextInst == nil)
		return { 0.0f, 1.0f, 0.0f };
	rw::V3d next = gDrawType >= 0 ? gDrawNext : nextInst->m_translation;
	rw::V3d d = { next.x - inst->m_translation.x, next.y - inst->m_translation.y,
		next.z - inst->m_translation.z };
	float length = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
	return length > 0.01f ? rw::V3d{ d.x / length, d.y / length, d.z / length } : rw::V3d{ 0.0f, 1.0f, 0.0f };
}

static float
pulse(float time, float period, float amount)
{
	return 1.0f + amount * sinf(time / period * 2.0f * (float)M_PI);
}

// The model, or the made-here shape when the game has no such model.
static void
cylinderShape(rw::V3d base, float radius, float height, rw::RGBA colour, bool picking)
{
	if(MarkerModel *m = model(MODEL_CYLINDER))
		drawModel(m, placement(base, { 0.0f, 0.0f, 1.0f }, { radius, radius, height }), colour);
	else{
		cylinder(base, radius, std::min(height, radius * 2.0f), colour, picking);
		flush();
	}
}

static void
hoopShape(rw::V3d centre, rw::V3d facing, float size, rw::RGBA colour)
{
	if(MarkerModel *m = model(MODEL_HOOP))
		drawModel(m, placement(centre, facing, { size, size, size }), colour);
	else{
		hoop(centre, facing, size * 0.5f, size * 0.04f, colour);
		flush();
	}
}

void
DrawCheckpointMarker(const ObjectInst *inst, float time, const rw::RGBA *pickColour)
{
	const ObjectPawnProperties *p = GetObjectPawnProperties(inst);
	bool race = p->cpKind == SAMP_CHECKPOINT_RACE;
	int type = race ? (gDrawType >= 0 ? gDrawType : GetRaceCheckpointShownType(inst)) : -1;
	float size = std::max(p->cpSize, 0.5f);
	rw::V3d at = inst->m_translation;
	bool picking = pickColour != nil;
	auto colour = [&](int alpha){ return picking ? *pickColour : withAlpha(CP_RED, alpha); };

	if(type < 0){
		// A player checkpoint: the cylinder, pulsing, a little sunk.
		float s = size * pulse(time, 2.048f, 0.1f);
		cylinderShape({ at.x, at.y, at.z - size / 20.0f }, s, s, colour(76), picking);
		return;
	}
	if(type <= 2){
		// A tall tube; in the pick pass only its foot, so it doesn't hide what's behind.
		float s = 1.5f * size * pulse(time, 1.024f, 0.075f);
		cylinderShape({ at.x, at.y, at.z - 0.5f }, s, picking ? 2.0f * s : 20.0f * s, colour(128), picking);
		if(picking)
			return;
		rw::V3d above = { at.x, at.y, at.z + 3.0f };
		if(type == 0){
			rw::V3d fwd = pointing(inst);
			rw::V3d down = { -fwd.x, -fwd.y, -fwd.z };
			if(MarkerModel *m = model(MODEL_DIAMOND)){
				// No blending: black, then the red one inside it.
				rw::SetRenderState(rw::VERTEXALPHA, 0);
				float s = 0.625f * size;
				drawModel(m, placement(above, down, { s, s, s }), { 0, 0, 0, 255 });
				drawModel(m, placement(above, down, { 0.8f * s, 0.8f * s, 0.8f * s }), { 255, 64, 64, 255 });
				rw::SetRenderState(rw::VERTEXALPHA, 1);
			}else{
				arrow(above, fwd, std::max(1.5f, size * 0.5f), withAlpha(CP_RED, 230));
				flush();
			}
		}else if(type == 1)
			flagSprite(above, size * 0.25f, CP_RED);
		return;
	}

	// Air: the hoop facing the next checkpoint.
	rw::V3d fwd = pointing(inst);
	rw::V3d centre = at;
	switch(type){
	case 5: {	// its facing wobbles from side to side
		rw::V3d side = { -fwd.y, fwd.x, 0.0f };
		float w = 0.5f * sinf(time / 0.66f * 2.0f * (float)M_PI);
		fwd = { fwd.x + side.x * w, fwd.y + side.y * w, fwd.z };
		break;
	}
	case 6: {	// four hoops spread along the way, fading, then gone for a moment
		float cycle = fmodf(time, 1.5f) / 1.5f;
		if(cycle < 0.85f){
			float multi = 0.75f * cycle / 0.85f;
			for(int i = 0; i < 4; i++){
				rw::V3d c = { at.x + fwd.x * i * multi * size, at.y + fwd.y * i * multi * size,
					at.z + fwd.z * i * multi * size };
				hoopShape(c, fwd, size, colour(96 - 16 * i));
				if(picking)
					break;
			}
		}else if(picking)
			hoopShape(at, fwd, size, colour(96));
		return;
	}
	case 7:	// bobs up and down 0.2 m
		centre.z += 0.2f * sinf(time / 2.0f * 2.0f * (float)M_PI);
		break;
	case 8:	// sinks (towards the player, in game), then starts again
		centre.z -= fmodf(time, 3.0f) / 3.0f * 2.0f;
		break;
	}
	hoopShape(centre, fwd, size, colour(96));
}

static void
setMarkerState(bool picking)
{
	rw::SetRenderState(rw::VERTEXALPHA, picking ? 0 : 1);
	rw::SetRenderState(rw::ZWRITEENABLE, picking ? 1 : 0);
	rw::SetRenderState(rw::ZTESTENABLE, 1);
	rw::SetRenderState(rw::FOGENABLE, 0);
	rw::SetRenderState(rw::SRCBLEND, rw::BLENDSRCALPHA);
	rw::SetRenderState(rw::DESTBLEND, rw::BLENDINVSRCALPHA);
	rw::SetRenderState(rw::CULLMODE, rw::CULLNONE);
	rw::SetRenderStatePtr(rw::TEXTURERASTER, nil);
}

static void
renderMarkers(float time, bool picking)
{
	int32 vertexAlpha = rw::GetRenderState(rw::VERTEXALPHA), zwrite = rw::GetRenderState(rw::ZWRITEENABLE);
	int32 fog = rw::GetRenderState(rw::FOGENABLE), cull = rw::GetRenderState(rw::CULLMODE);
	setMarkerState(picking);
	std::vector<ObjectInst*> previewed;
	if(RacePreviewActive())
		GetRaceCheckpoints(RacePreviewRace(), previewed);
	for(CPtrNode *n = instances.first; n; n = n->next){
		ObjectInst *inst = (ObjectInst*)n->item;
		if(inst->m_isDeleted || inst->m_editorHidden || GetSampElementKind(inst) != SAMP_ELEMENT_CHECKPOINT)
			continue;
		rw::RGBA id = { (uint8)(inst->m_id & 0xFF), (uint8)(inst->m_id >> 8 & 0xFF), (uint8)(inst->m_id >> 16 & 0xFF), 255 };
		gDrawType = -1;
		// A previewed race shows only the checkpoint to reach; the rest can
		// still be picked.
		if(!previewed.empty() && std::find(previewed.begin(), previewed.end(), inst) != previewed.end()){
			gDrawType = previewTypeFor(inst, previewed, &gDrawNext, &gDrawHasNext);
			if(gDrawType < 0 && !picking)
				continue;
		}
		DrawCheckpointMarker(inst, time, picking ? &id : nil);
	}
	gDrawType = -1;
	rw::SetRenderState(rw::VERTEXALPHA, vertexAlpha);
	rw::SetRenderState(rw::ZWRITEENABLE, zwrite);
	rw::SetRenderState(rw::FOGENABLE, fog);
	rw::SetRenderState(rw::CULLMODE, cull);
}

void
RenderCheckpointMarkers(float time)
{
	renderMarkers(time, false);
}

void
RenderCheckpointMarkersColourCoded(void)
{
	renderMarkers((float)ImGui::GetTime(), true);
}
