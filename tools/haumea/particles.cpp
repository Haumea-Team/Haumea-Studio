// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "particles.h"
#include "particlefx.h"
#include "playerview.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace ParticlePreview
{
bool enabled = true;
namespace
{
ParticleFX::Project project;
bool loaded = false;
int textureSlot = -1;
Stats stats;
uint64_t frame = 0;
constexpr size_t MaxSystems = 256, MaxParticles = 12000, PerSystem = 1024;
struct State {
	const ParticleFX::System *system;
	ParticleFX::Instance simulation;
	ParticleFX::Matrix transform;
	uint64_t seen = 0;
	int drawn = 0;
	State(const ParticleFX::System *s, uint32_t seed) : system(s), simulation(s, seed) {}
};
std::unordered_map<uint64_t, std::unique_ptr<State>> states;
std::unordered_map<std::string, rw::Texture *> textures;
std::unordered_set<std::string> warned;
ParticleFX::Vec vec(rw::V3d v)
{
	return {v.x, v.y, v.z};
}
rw::V3d vec(ParticleFX::Vec v)
{
	return {v.x, v.y, v.z};
}
void load()
{
	if (loaded)
		return;
	loaded = true;
	FILE *f = fopen_ci("models/effects.fxp", "rb");
	if (!f) {
		log("Particle preview: models/effects.fxp was not found\n");
		return;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	rewind(f);
	if (size <= 0 || size > 32 * 1024 * 1024) {
		fclose(f);
		log("Particle preview: invalid FX project size\n");
		return;
	}
	std::string text((size_t)size, '\0');
	size_t got = fread(&text[0], 1, text.size(), f);
	fclose(f);
	std::istringstream input(text);
	std::string error;
	if (got != text.size() || !project.load(input, &error)) {
		log("Particle preview: %s\n", error.c_str());
		return;
	}
	textureSlot = AddTxdSlot("~particlefx");
	LoadTxd(textureSlot, "models/effectsPC.txd");
	log("Particle preview: loaded %d FX systems\n", (int)project.systems.size());
}
rw::Texture *texture(const std::string &name)
{
	if (name.empty())
		return nil;
	auto cached = textures.find(name);
	if (cached != textures.end())
		return cached->second;
	rw::Texture *found = nil;
	TxdDef *td = GetTxdDef(textureSlot);
	if (td && td->txd)
		FORLIST(link, td->txd->textures)
		{
			rw::Texture *candidate = rw::Texture::fromDict(link);
			if (rw::strcmp_ci(candidate->name, name.c_str()) == 0) {
				found = candidate;
				break;
			}
		}
	textures[name] = found;
	return found;
}
struct Source {
	uint64_t key;
	const ParticleFX::System *system;
	ParticleFX::Matrix transform;
	float distance;
};
struct Draw {
	State *state;
	const ParticleFX::Particle *particle;
	float distance;
};
std::vector<Source> sources;
std::vector<Draw> draws;
rw::RWDEVICE::Im3DVertex vertices[4096];
uint16 indices[6144];
int numVertices = 0;
void flush()
{
	if (!numVertices)
		return;
	rw::im3d::Transform(vertices, numVertices, nil, rw::im3d::EVERYTHING);
	rw::im3d::RenderIndexedPrimitive(rw::PRIMTYPETRILIST, indices, numVertices / 4 * 6);
	rw::im3d::End();
	numVertices = 0;
}
void quad(const rw::V3d corners[4], const ParticleFX::Appearance &a)
{
	if (numVertices + 4 > 4096)
		flush();
	static const float us[] = {0, 1, 1, 0}, vs[] = {1, 1, 0, 0};
	for (int k = 0; k < 4; k++) {
		auto &v = vertices[numVertices++];
		v.setX(corners[k].x);
		v.setY(corners[k].y);
		v.setZ(corners[k].z);
		v.setColor((uint8)a.red, (uint8)a.green, (uint8)a.blue, (uint8)a.alpha);
		v.setU(us[k]);
		v.setV(vs[k]);
	}
}
// The sprites in draws, facing a camera at eye with those right and up
// axes. cull: skip the ones off the 3D view's screen (not for previews).
void render(rw::V3d eye, rw::V3d cameraRight, rw::V3d cameraUp, bool cull)
{
	if (draws.empty())
		return;
	(void)eye;
	// Sort transparent sprites globally, including particles from different objects.
	std::sort(draws.begin(), draws.end(),
			  [](const Draw &a, const Draw &b) { return a.distance > b.distance; });
	const int savedStates[] = {rw::FOGENABLE, rw::VERTEXALPHA,	rw::ZTESTENABLE, rw::ZWRITEENABLE,
							   rw::CULLMODE,  rw::ALPHATESTREF, rw::SRCBLEND,	 rw::DESTBLEND};
	uint32 old[nelem(savedStates)];
	for (size_t n = 0; n < nelem(savedStates); n++)
		old[n] = rw::GetRenderState(savedStates[n]);
	void *oldRaster = rw::GetRenderStatePtr(rw::TEXTURERASTER);
	rw::SetRenderState(rw::FOGENABLE, gEnableFog);
	rw::SetRenderState(rw::VERTEXALPHA, 1);
	rw::SetRenderState(rw::ZTESTENABLE, 1);
	rw::SetRenderState(rw::ZWRITEENABLE, 0);
	rw::SetRenderState(rw::CULLMODE, rw::CULLNONE);
	rw::SetRenderState(rw::ALPHATESTREF, 0);
	rw::Raster *current = nil;
	int src = -1, dst = -1;
	for (const Draw &item : draws) {
		State &state = *item.state;
		const auto &p = *item.particle;
		const auto &primitive = state.system->primitives[p.primitive];
		ParticleFX::Appearance a = state.simulation.appearance(p);
		if (a.alpha < 1 || a.width <= 0 || a.height <= 0)
			continue;
		rw::V3d centre = vec(p.position), screen;
		float sw, sh;
		if (cull && !Sprite::CalcScreenCoors(centre, &screen, &sw, &sh, false))
			continue;
		unsigned image = p.texture;
		if (primitive.info[ParticleFX::AnimTexture].present) {
			float phase = state.simulation.time() / state.system->length;
			float index = primitive.value(ParticleFX::AnimTexture, 0, phase, p.age / p.life, 0);
			image = (unsigned)std::max(0.f, std::min(3.f, index));
		}
		rw::Texture *tex = texture(primitive.textures[image]);
		if (!tex)
			tex = texture(primitive.textures[0]);
		if (!tex || !tex->raster)
			continue;
		int source = primitive.alpha ? primitive.srcBlend + 1 : rw::BLENDONE;
		int destination = primitive.alpha ? primitive.dstBlend + 1 : rw::BLENDZERO;
		if (tex->raster != current || source != src || destination != dst) {
			flush();
			current = tex->raster;
			src = source;
			dst = destination;
			rw::SetRenderStatePtr(rw::TEXTURERASTER, current);
			rw::SetRenderState(rw::SRCBLEND, src);
			rw::SetRenderState(rw::DESTBLEND, dst);
		}
		rw::V3d right = cameraRight, up = cameraUp;
		if (a.flat) {
			const auto &flat = primitive.info[ParticleFX::Flat];
			float t = state.simulation.time() / state.system->length, age = p.age / p.life;
			ParticleFX::Vec r = {flat.sample(0, t, age, 1), flat.sample(1, t, age, 0),
								 flat.sample(2, t, age, 0)};
			ParticleFX::Vec u = {flat.sample(3, t, age, 0), flat.sample(4, t, age, 1),
								 flat.sample(5, t, age, 0)};
			right = vec(state.transform.vector(primitive.matrix.vector(r)));
			up = vec(state.transform.vector(primitive.matrix.vector(u)));
		}
		float angle = a.rotation * .01745329252f, c = std::cos(angle), s = std::sin(angle);
		rw::V3d r = add(scale(right, c), scale(up, s)), u = add(scale(up, c), scale(right, -s));
		float left = a.rectangular ? a.left : -.5f, rightEdge = a.rectangular ? a.right : .5f;
		float top = a.rectangular ? a.top : .5f, bottom = a.rectangular ? a.bottom : -.5f;
		rw::V3d corners[] = {add(centre, add(scale(r, left * a.width), scale(u, bottom * a.height))),
							 add(centre, add(scale(r, rightEdge * a.width), scale(u, bottom * a.height))),
							 add(centre, add(scale(r, rightEdge * a.width), scale(u, top * a.height))),
							 add(centre, add(scale(r, left * a.width), scale(u, top * a.height)))};
		quad(corners, a);
		stats.drawn++;
		state.drawn++;
	}
	flush();
	rw::SetRenderStatePtr(rw::TEXTURERASTER, oldRaster);
	for (size_t n = 0; n < nelem(savedStates); n++)
		rw::SetRenderState(savedStates[n], old[n]);
}
} // namespace
bool HasParticleEffect(const ObjectInst *inst)
{
	if (!inst || inst->m_isDeleted)
		return false;
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if (!obj)
		return false;
	for (int i = 0; i < obj->m_numEffects; i++)
		if (Effects::GetEffect(obj->m_effectIndex + i)->type == FX_PARTICLE)
			return true;
	return false;
}
void Restart(const ObjectInst *inst)
{
	if (!inst)
		return;
	for (auto it = states.begin(); it != states.end();)
		if ((uint32_t)(it->first >> 32) == (uint32_t)inst->m_id)
			it = states.erase(it);
		else
			++it;
}
const Stats &GetStats()
{
	return stats;
}
int CountForModel(int model, int kind)
{
	int count = 0;
	for (const auto &entry : states) {
		ObjectInst *inst = GetInstanceByID((int32)(entry.first >> 32));
		if (inst && PawnModelId(inst->m_objectId) == model)
			count += kind == 0	 ? 1
					 : kind == 1 ? (int)entry.second->simulation.particles.size()
								 : entry.second->drawn;
	}
	return count;
}

void Render(float dt)
{
	stats = {};
	++frame;
	sources.clear();
	draws.clear();
	if (!enabled || !isSA()) {
		states.clear();
		return;
	}
	load();
	stats.definitions = (int)project.systems.size();
	if (project.systems.empty())
		return;
	for (int q = 0; q < 1024; q++) {
		uint16 b = (uint16)(q * 4),
			   ix[] = {b, (uint16)(b + 1), (uint16)(b + 2), b, (uint16)(b + 2), (uint16)(b + 3)};
		for (int j = 0; j < 6; j++)
			indices[q * 6 + j] = ix[j];
	}
	for (CPtrNode *node = instances.first; node; node = node->next) {
		ObjectInst *inst = (ObjectInst *)node->item;
		if (inst->m_isDeleted || inst->m_editorHidden || PlayerViewHides(inst) ||
			!AttachmentWorkspaceShowsInstance(inst))
			continue;
		if (!inst->m_selected && !gNoAreaCull && inst->m_area != currentArea && inst->m_area != 13)
			continue;
		ObjectDef *obj = GetObjectDef(inst->m_objectId);
		if (!obj || obj->m_numEffects <= 0)
			continue;
		if (!inst->m_selected && !gNoTimeCull && obj->m_isTimed &&
			!IsHourInRange(obj->m_timeOn, obj->m_timeOff))
			continue;
		for (int n = 0; n < obj->m_numEffects; n++) {
			Effect *effect = Effects::GetEffect(obj->m_effectIndex + n);
			if (effect->type != FX_PARTICLE || !effect->prtcl.name[0])
				continue;
			rw::V3d pos;
			rw::V3d::transformPoints(&pos, &effect->pos, 1, &inst->m_matrix);
			float distance = TheCamera.distanceTo(pos);
			if (distance >= gStreamDistance)
				continue;
			const auto *system = project.find(effect->prtcl.name);
			if (!system) {
				stats.missing++;
				if (warned.insert(effect->prtcl.name).second)
					log("Particle preview: unknown FX system %s\n", effect->prtcl.name);
				continue;
			}
			ParticleFX::Matrix transform = {vec(inst->m_matrix.right), vec(inst->m_matrix.up),
											vec(inst->m_matrix.at), vec(pos)};
			uint64_t key = (uint64_t)(uint32_t)inst->m_id << 32 | (uint32_t)(obj->m_effectIndex + n);
			sources.push_back({key, system, transform, distance});
		}
	}
	std::sort(sources.begin(), sources.end(), [](const Source &a, const Source &b) {
		return a.distance == b.distance ? a.key < b.key : a.distance < b.distance;
	});
	if (sources.size() > MaxSystems) {
		stats.limited = true;
		sources.resize(MaxSystems);
	}
	size_t total = 0;
	for (const Source &source : sources) {
		auto &state = states[source.key];
		if (!state || state->system != source.system)
			state.reset(new State(source.system, (uint32_t)(source.key ^ (source.key >> 32))));
		state->seen = frame;
		state->transform = source.transform;
		state->drawn = 0;
		size_t budget = std::min(PerSystem, MaxParticles - total);
		state->simulation.advance(dt, source.transform, {.4f, .2f, 0}, budget);
		total += state->simulation.particles.size();
		if (total == MaxParticles)
			stats.limited = true;
		for (const auto &particle : state->simulation.particles) {
			rw::V3d delta = sub(vec(particle.position), TheCamera.m_position);
			float distance = dot(delta, delta);
			if (distance < gStreamDistance * gStreamDistance)
				draws.push_back({state.get(), &particle, distance});
		}
	}
	// Deleting, hiding, changing model, closing the map or leaving its world removes
	// both the emitter and all its live particles in this frame. Undo starts it again.
	for (auto it = states.begin(); it != states.end();)
		if (it->second->seen != frame)
			it = states.erase(it);
		else
			++it;
	stats.systems = (int)states.size();
	stats.live = (int)total;
	rw::V3d direction = rw::normalize(rw::sub(TheCamera.m_target, TheCamera.m_position));
	rw::V3d cameraRight = rw::normalize(rw::cross(direction, TheCamera.m_up));
	rw::V3d cameraUp = rw::normalize(rw::cross(cameraRight, direction));
	render(TheCamera.m_position, cameraRight, cameraUp, true);
}

// ---- A model's effects in a preview (Asset Browser tiles and the like) ----
// Its particle systems run for a while at the model's origin, so the picture
// shows the effect going rather than its first frame.
namespace
{
int previewModel = -1;
std::vector<std::unique_ptr<State>> previewStates;
}

bool PreviewModelParticles(int objectId, const std::vector<Effect> &effects, rw::V3d *centre, float *radius)
{
	previewStates.clear();
	previewModel = -1;
	if (!isSA())
		return false;
	bool any = false;
	for (const Effect &effect : effects)
		any |= effect.type == FX_PARTICLE && effect.prtcl.name[0];
	if (!any)
		return false;
	load();
	for (size_t n = 0; n < effects.size(); n++) {
		const Effect &effect = effects[n];
		if (effect.type != FX_PARTICLE || !effect.prtcl.name[0])
			continue;
		const auto *system = project.find(effect.prtcl.name);
		if (!system)
			continue;
		ParticleFX::Matrix transform = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, vec(effect.pos)};
		std::unique_ptr<State> state(new State(system, 1234u + (uint32_t)n));
		state->transform = transform;
		// About two and a half seconds, in steps of a frame.
		for (int step = 0; step < 75; step++)
			state->simulation.advance(1.0f / 30.0f, transform, {.4f, .2f, 0}, PerSystem);
		previewStates.push_back(std::move(state));
	}
	rw::V3d lo = {1e9f, 1e9f, 1e9f}, hi = {-1e9f, -1e9f, -1e9f};
	any = false;
	for (const auto &state : previewStates)
		for (const auto &p : state->simulation.particles) {
			ParticleFX::Appearance a = state->simulation.appearance(p);
			float r = std::max(a.width, a.height) * 0.5f;
			rw::V3d at = vec(p.position);
			lo = {std::min(lo.x, at.x - r), std::min(lo.y, at.y - r), std::min(lo.z, at.z - r)};
			hi = {std::max(hi.x, at.x + r), std::max(hi.y, at.y + r), std::max(hi.z, at.z + r)};
			any = true;
		}
	if (!any)
		return false;
	previewModel = objectId;
	*centre = scale(add(lo, hi), 0.5f);
	*radius = std::max(0.5f, length(sub(hi, lo)) * 0.5f);
	return true;
}

void RenderModelParticles(int objectId, rw::V3d eye, rw::V3d target, rw::V3d up)
{
	if (objectId != previewModel || previewStates.empty())
		return;
	for (int q = 0; q < 1024; q++) {
		uint16 b = (uint16)(q * 4),
			   ix[] = {b, (uint16)(b + 1), (uint16)(b + 2), b, (uint16)(b + 2), (uint16)(b + 3)};
		for (int j = 0; j < 6; j++)
			indices[q * 6 + j] = ix[j];
	}
	draws.clear();
	for (const auto &state : previewStates)
		for (const auto &particle : state->simulation.particles) {
			rw::V3d delta = sub(vec(particle.position), eye);
			draws.push_back({state.get(), &particle, dot(delta, delta)});
		}
	rw::V3d direction = rw::normalize(rw::sub(target, eye));
	rw::V3d right = rw::normalize(rw::cross(direction, up));
	rw::V3d cameraUp = rw::normalize(rw::cross(right, direction));
	bool fog = gEnableFog;
	gEnableFog = false;
	render(eye, right, cameraUp, false);
	gEnableFog = fog;
	draws.clear();
}
} // namespace ParticlePreview
